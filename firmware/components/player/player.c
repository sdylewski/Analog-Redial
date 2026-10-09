#include "player.h"
#include "audio.h"
#include "storage.h"
#include "audio_convert.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static const char *TAG = "player";

typedef struct {
    bool active;
    volatile bool stop_requested;
} line_state_t;

static line_state_t s_line[AUDIO_NUM_LINES];

typedef struct {
    int    line;
    char   name[STORAGE_MAX_NAME];
    player_done_cb_t done_cb;
    void  *ctx;
} play_args_t;

static void finish(int line, bool call_cb, player_done_cb_t cb, void *ctx)
{
    s_line[line].active = false;
    if (call_cb && cb) cb(line, ctx);
}

/* Read this much up front to find the "data" chunk -- covers the fixed
 * 44-byte header our own `record` command writes, plus real-world WAV
 * files with extra chunks (afconvert on macOS pads with a "FLLR" filler
 * chunk before "data", observed at a 4096-byte offset). */
#define HEADER_SCAN_BYTES 8192
#define PLAYBACK_CHUNK_BYTES 4096

#if defined(CONFIG_PLAYER_MP3)
/* Streams and decodes an MP3 frame-by-frame, reusing `buf` (already primed
 * with `have` bytes from the initial header scan -- unlike the WAV path,
 * which re-seeks to wav.data_offset and reads fresh, there's no header to
 * skip past here, so those same first bytes are exactly where decoding
 * starts) as a sliding input window up to HEADER_SCAN_BYTES capacity.
 * Mono only (matches the WAV path's `wav.channels != 1` rejection) -- our
 * own encode pipeline (gen_prompt.py) never produces anything else, and
 * downmixing an arbitrary stereo source isn't needed yet. Returns true if
 * playback ran to completion (not stopped, no decode error). */
static bool play_mp3(int line, storage_reader_t *rd, uint8_t *buf, size_t have,
                     volatile bool *stop_requested, const char *name)
{
    audio_mp3_decoder_t *dec = audio_mp3_decoder_create();
    if (!dec) {
        ESP_LOGE(TAG, "line %d: MP3 decoder alloc failed", line);
        return false;
    }

    size_t pos = 0; /* consumed offset within buf[0..have) */
    bool eof = false;
    bool decode_ok = true;
    bool rs_ready = false;
    bool need_resample = false;
    audio_resampler_t rs;
    int16_t pcm[AUDIO_CONVERT_MP3_MAX_SAMPLES];
    int16_t *resampled = NULL;
    size_t resampled_cap = 0;

    if (*stop_requested) {
        audio_mp3_decoder_destroy(dec);
        return false;
    }
    audio_tx_external(line, true);
    while (!*stop_requested) {
        if (pos > 0) {
            memmove(buf, buf + pos, have - pos);
            have -= pos;
            pos = 0;
        }
        if (have < HEADER_SCAN_BYTES && !eof) {
            size_t got = storage_read_chunk(rd, buf + have, HEADER_SCAN_BYTES - have);
            have += got;
            if (got == 0) eof = true;
        }
        if (have == 0) break; /* nothing left to decode */

        int out_channels = 0;
        uint32_t out_rate = 0;
        size_t consumed = 0;
        int samples = audio_mp3_decode_frame(dec, buf, have, pcm, &out_channels, &out_rate, &consumed);
        pos = consumed;

        if (consumed == 0 && samples <= 0) {
            /* Not enough buffered for even one frame, even after topping
             * up to a full HEADER_SCAN_BYTES window -- either truly done
             * (eof) or a real frame here is implausibly large. Either way,
             * this stream has nothing more usable to offer. */
            break;
        }
        if (samples <= 0) continue; /* skipped ID3/garbage -- no audio this round */

        if (out_channels != 1) {
            ESP_LOGW(TAG, "line %d: '%s' is %d-channel MP3, only mono is supported",
                     line, name, out_channels);
            decode_ok = false;
            break;
        }
        if (!rs_ready) {
            need_resample = (out_rate != audio_sample_rate());
            if (need_resample) {
                audio_resampler_init(&rs, out_rate, audio_sample_rate());
                resampled_cap = audio_resampler_max_out(&rs, AUDIO_CONVERT_MP3_MAX_SAMPLES);
                resampled = malloc(resampled_cap * sizeof(int16_t));
                if (!resampled) {
                    ESP_LOGE(TAG, "line %d: resample buffer alloc failed", line);
                    decode_ok = false;
                    break;
                }
                ESP_LOGI(TAG, "line %d: '%s' is %u Hz, resampling to device rate %u Hz",
                         line, name, (unsigned)out_rate, (unsigned)audio_sample_rate());
            }
            rs_ready = true;
        }

        int16_t *out_pcm = pcm;
        size_t n_samples = (size_t)samples;
        if (need_resample) {
            n_samples = audio_resampler_push(&rs, pcm, n_samples, resampled, resampled_cap);
            out_pcm = resampled;
        }
        size_t sent = 0;
        while (sent < n_samples && !*stop_requested) {
            size_t wrote = audio_ext_write(line, out_pcm + sent, n_samples - sent);
            if (wrote == 0) { vTaskDelay(pdMS_TO_TICKS(5)); continue; }
            sent += wrote;
        }
    }
    free(resampled);
    audio_mp3_decoder_destroy(dec);

    bool stopped = *stop_requested;
    if (!stopped) {
        /* let what's still queued in the ext stream buffer actually play
         * out before switching the line's audio source away -- same
         * settle delay the WAV path uses. */
        vTaskDelay(pdMS_TO_TICKS(120));
        audio_tx_silence(line);
    }
    /* Stopped: whoever stopped us already owns the line's audio (see the
     * matching comment in play_task()). */
    return !stopped && decode_ok;
}
#endif /* CONFIG_PLAYER_MP3 */

static void play_task(void *arg)
{
    play_args_t *a = (play_args_t *)arg;
    int line = a->line;
    bool ok = false;
    storage_reader_t rd = { 0 };
    uint8_t *buf = malloc(HEADER_SCAN_BYTES);
    if (!buf) {
        ESP_LOGE(TAG, "line %d: malloc(%u) failed", line, (unsigned)HEADER_SCAN_BYTES);
        goto out;
    }

    if (storage_read_open(a->name, &rd) != ESP_OK) {
        ESP_LOGW(TAG, "line %d: '%s' not found in storage", line, a->name);
        goto out;
    }

    {
        size_t scanned = storage_read_chunk(&rd, buf, HEADER_SCAN_BYTES);

#if defined(CONFIG_PLAYER_MP3)
        /* Checked before WAV parsing -- a WAV always starts "RIFF", which
         * never collides with an MP3's ID3 tag or frame-sync magic, so this
         * is an unambiguous fork, not a fallback guess. */
        if (audio_convert_mp3_magic(buf, scanned)) {
            ok = play_mp3(line, &rd, buf, scanned, &s_line[line].stop_requested, a->name);
            goto out;
        }
#endif

        audio_wav_info_t wav;
        if (!audio_wav_parse(buf, scanned, &wav) || wav.audio_format != 1 || wav.channels != 1 ||
            wav.bits_per_sample != 16) {
            ESP_LOGW(TAG, "line %d: '%s' isn't mono 16-bit PCM WAV (or header is bigger "
                          "than %u bytes)", line, a->name, (unsigned)HEADER_SCAN_BYTES);
            goto out;
        }
        bool need_resample = (wav.sample_rate != audio_sample_rate());
        audio_resampler_t rs;
        int16_t *resampled = NULL;
        size_t resampled_cap = 0;
        if (need_resample) {
            audio_resampler_init(&rs, wav.sample_rate, audio_sample_rate());
            resampled_cap = audio_resampler_max_out(&rs, PLAYBACK_CHUNK_BYTES / sizeof(int16_t));
            resampled = malloc(resampled_cap * sizeof(int16_t));
            if (!resampled) {
                ESP_LOGE(TAG, "line %d: resample buffer alloc failed", line);
                goto out;
            }
            ESP_LOGI(TAG, "line %d: '%s' is %u Hz, resampling to device rate %u Hz",
                     line, a->name, (unsigned)wav.sample_rate, (unsigned)audio_sample_rate());
        }
        if (storage_read_seek(&rd, wav.data_offset) != ESP_OK) {
            ESP_LOGW(TAG, "line %d: seek to data chunk failed", line);
            free(resampled);
            goto out;
        }

        if (s_line[line].stop_requested) {
            free(resampled);
            goto out;
        }
        audio_tx_external(line, true);
        size_t remaining = wav.data_bytes;
        while (remaining > 0 && !s_line[line].stop_requested) {
            size_t want = (remaining < PLAYBACK_CHUNK_BYTES) ? remaining : PLAYBACK_CHUNK_BYTES;
            size_t got = storage_read_chunk(&rd, buf, want);
            if (got == 0) break; /* short file vs. its own data_bytes header field */
            remaining -= got;

            size_t n_samples = got / sizeof(int16_t); /* drops a possible odd trailing byte */
            const int16_t *pcm = (const int16_t *)buf;
            if (need_resample) {
                n_samples = audio_resampler_push(&rs, pcm, n_samples, resampled, resampled_cap);
                pcm = resampled;
            }
            size_t sent = 0;
            while (sent < n_samples && !s_line[line].stop_requested) {
                size_t wrote = audio_ext_write(line, pcm + sent, n_samples - sent);
                if (wrote == 0) { vTaskDelay(pdMS_TO_TICKS(5)); continue; }
                sent += wrote;
            }
        }
        free(resampled);
        bool stopped = s_line[line].stop_requested;
        if (!stopped) {
            /* let what's still queued in the ext stream buffer actually
             * play out before switching the line's audio source away. */
            vTaskDelay(pdMS_TO_TICKS(120));
            audio_tx_silence(line);
        }
        /* Stopped: leave the line's audio alone. The FSM stops a clip and
         * sets the line's next source in the same step (bridging a
         * barged-in call, dial tone after a quick re-pickup), and this
         * lower-priority task only notices the stop afterwards -- silencing
         * here used to cut that new audio off, e.g. one-way audio when
         * picking up during the answering machine's greeting. */
        ok = !stopped;
    }

out:
    storage_read_close(&rd);
    free(buf);
    finish(line, ok, a->done_cb, a->ctx);
    free(a);
    vTaskDelete(NULL);
}

esp_err_t player_play(int line, const char *storage_name, player_done_cb_t done_cb, void *ctx)
{
    if (line < 0 || line >= AUDIO_NUM_LINES) return ESP_ERR_INVALID_ARG;
    if (s_line[line].active) return ESP_ERR_INVALID_STATE;

    /* Existence pre-check, so a missing clip fails synchronously here
     * instead of after already spawning the play task. Probes with
     * storage_read_open() itself rather than re-deriving its fallback
     * chain (writable storage -- SD or internal, whichever actually has
     * the file -- then the read-only bundled defaults) via storage_list()
     * calls: that used to be two separate, independently-maintained checks,
     * and they drifted out of sync twice. First: storage_list() alone
     * missed anything that only existed in `bundled` (e.g. a clip baked in
     * at build time but never uploaded), 404ing even operator.wav despite
     * storage_read_open() reading it fine. Fixed by also checking
     * storage_list_bundled() -- but that fix kept the same underlying
     * flaw: storage_list() alone only reports whichever ONE writable
     * backend (SD or internal) is currently active, not the fallback
     * across both storage_read_open() actually does. Confirmed live: every
     * clip (which by design always lives on internal flash, specifically
     * so it survives an SD card being inserted/removed -- see storage.c)
     * became unplayable via this check the moment an SD card was mounted,
     * even though storage_read_open() could still read it fine. */
    storage_reader_t probe = {0};
    if (storage_read_open(storage_name, &probe) != ESP_OK) return ESP_ERR_NOT_FOUND;
    storage_read_close(&probe);

    play_args_t *a = malloc(sizeof(*a));
    if (!a) return ESP_ERR_NO_MEM;
    a->line = line;
    strncpy(a->name, storage_name, sizeof(a->name) - 1);
    a->name[sizeof(a->name) - 1] = 0;
    a->done_cb = done_cb;
    a->ctx = ctx;

    s_line[line].active = true;
    s_line[line].stop_requested = false;

    /* Whichever clip this turns out to be isn't known until play_task
     * actually peeks at it -- so with MP3 playback compiled in, every
     * play_task gets the bigger stack minimp3 needs (~16KB internal decode
     * locals, measured -- see player/Kconfig), not just MP3 ones. Paid only
     * per active playback task and freed when it ends, unlike the
     * permanent tax CONFIG_AUDIO_CONVERT_MP3's httpd-worker path pays. */
#if defined(CONFIG_PLAYER_MP3)
    const uint32_t stack_bytes = 28672;
#else
    const uint32_t stack_bytes = 4096;
#endif
    if (xTaskCreate(play_task, "player", stack_bytes, a, tskIDLE_PRIORITY + 3, NULL) != pdPASS) {
        s_line[line].active = false;
        free(a);
        return ESP_FAIL;
    }
    return ESP_OK;
}

void player_stop(int line)
{
    if (line < 0 || line >= AUDIO_NUM_LINES) return;
    s_line[line].stop_requested = true;
}

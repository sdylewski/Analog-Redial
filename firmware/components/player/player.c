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
        }
        audio_tx_silence(line);
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

    storage_entry_t entries[32];
    size_t n = storage_list(entries, 32);
    bool found = false;
    for (size_t i = 0; i < n; i++) {
        if (strcmp(entries[i].name, storage_name) == 0) { found = true; break; }
    }
    if (!found) return ESP_ERR_NOT_FOUND;

    play_args_t *a = malloc(sizeof(*a));
    if (!a) return ESP_ERR_NO_MEM;
    a->line = line;
    strncpy(a->name, storage_name, sizeof(a->name) - 1);
    a->name[sizeof(a->name) - 1] = 0;
    a->done_cb = done_cb;
    a->ctx = ctx;

    s_line[line].active = true;
    s_line[line].stop_requested = false;

    if (xTaskCreate(play_task, "player", 4096, a, tskIDLE_PRIORITY + 3, NULL) != pdPASS) {
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

bool player_is_playing(int line)
{
    if (line < 0 || line >= AUDIO_NUM_LINES) return false;
    return s_line[line].active;
}

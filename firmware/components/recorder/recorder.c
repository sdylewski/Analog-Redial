#include "recorder.h"
#include "audio.h"
#include "storage.h"
#include "activity.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

static const char *TAG = "recorder";

typedef struct {
    bool          active;
    volatile bool stop;
} rec_state_t;

static rec_state_t s_rec[AUDIO_NUM_LINES];

typedef struct {
    int      line;
    char     name[STORAGE_MAX_NAME];
    uint32_t max_ms;
    uint32_t silence_ms;
    bool     downlink;
    recorder_done_cb_t cb;
    void    *ctx;
} rec_args_t;

/* 48 kHz input, read a whole number of decimation groups per pull (256 groups
 * of 6 = 1536 samples). Kept modest so the RX ring buffer never overflows. */
#define IN_CHUNK_SAMPLES 1536
#define SILENCE_RMS      500  /* ~ -36 dBFS; below this counts as "quiet" */

/* Downlink capture (see recorder_start_downlink()): audio_add_extwrite_tap()
 * is a push callback, not the pull-style audio_rx_read() the uplink path
 * uses (that seam is already owned by DTMF detection's permanent rx_tap
 * registration -- see telephony.c -- so recording can't share it the way it
 * shares the always-filled uplink stream buffer). Deliberately NOT
 * audio_add_downlink_tap() -- that observes the line's muxed TX *output*,
 * which goes silent the moment the answering machine silences its own TX
 * during the actual recording phase (so the caller doesn't hear a repeating
 * beep); audio_add_extwrite_tap() observes audio_ext_write()'s input
 * directly, before muxing, so it keeps seeing the caller's decoded RTP
 * audio regardless of what this line is currently transmitting. Bridged
 * into the same pull shape rec_task() already expects via a small local
 * stream buffer, same pattern as audio.c's own echo_probe_tap() -- just
 * bounded to a few chunks of slack instead of the whole capture, since a
 * voicemail can run up to 2 minutes. */
#define DOWNLINK_SB_SAMPLES (IN_CHUNK_SAMPLES * 4)

typedef struct {
    StreamBufferHandle_t sb;
} downlink_ctx_t;

static void downlink_push(int line, const int16_t *mono, size_t n, void *ctx)
{
    (void)line;
    downlink_ctx_t *dc = ctx;
    /* Non-blocking, like echo_probe_tap() -- the audio task can never block.
     * A full buffer (rec_task briefly stalled on a storage write) drops the
     * overrun rather than corrupting timing; DOWNLINK_SB_SAMPLES gives real
     * slack before that happens. */
    xStreamBufferSend(dc->sb, mono, n * sizeof(int16_t), 0);
}

/* AGC for recorded output only -- confirmed live (2026-09-03) that 211
 * messages were coming out around -46 dBFS RMS / 20% of full-scale peak
 * even for normal speaking volume, quiet enough several people assumed
 * playback itself was broken. That level is inherent to this hardware's
 * mic gain budget (locked to 0dB PGA -- raising it hits the ES8388's
 * gain-dependent input impedance bug documented in docs/signal-levels.md
 * -- combined with the divider added to keep DTMF from clipping), not
 * something to fix by turning the mic up. Applied here to the decimated
 * 8kHz samples right before they're written, so it only ever touches what
 * lands in the WAV file -- zero effect on live call audio, DTMF detection,
 * or anything else sharing the same RX stream upstream of this.
 *
 * Two-layer design, not just dtmf.c's plain one-block-lag AGC: a smoothed
 * gain estimate (same shape as dtmf.c's, see that file's own tuning
 * comment for the rationale) handles the steady-state level, PLUS a
 * per-block hard ceiling computed from that block's own raw peak, applied
 * on top before writing. The smoothed estimate alone isn't enough for
 * recorded voice the way it is for DTMF: DTMF tones hold a stable level
 * for the whole press, so a one-block lag never really overshoots.
 * Real audio doesn't -- confirmed live: the gain smoothed to near max
 * during a quiet stretch, then a single louder transient (the tail of the
 * pre-record beep bleeding into the first ~0.6s of a recording, see
 * REC_SETTLE_MS below) got hit with that stale high gain and clipped
 * solid at full scale. The per-block ceiling means no single block can
 * ever clip regardless of how far behind the smoothed estimate is. */
#define AGC_TARGET_PEAK  20000 /* ~61% of int16 full scale -- clearly audible, real headroom below clipping */
#define AGC_CEILING_PEAK 30000 /* ~92% -- hard per-block cap, independent of the smoothed estimate */
#define AGC_GATE_PEAK     1000 /* below this, hold the smoothed estimate rather than chase noise floor */
#define AGC_MIN_GAIN_Q8     64 /* 0.25x -- still allows taming an already-loud line */
#define AGC_MAX_GAIN_Q8   2048 /* 8x -- the per-block ceiling is what actually bounds worst case, not this */
#define AGC_SMOOTH_DIV       4

/* Recording starts the instant the pre-record beep's timer expires
 * (telephony.c), but the beep's own acoustic decay -- audible through the
 * earpiece, picked up by the mic -- doesn't stop that fast. Confirmed
 * live: the first ~0.6s of every recording captured that tail, loud
 * enough to clip. Discarding (not writing) this short window fixes it at
 * the source instead of just papering over it with the AGC ceiling above.
 * Doesn't change the caller-facing "starts right after the beep" behavior
 * -- recording still begins on the same event, this is well under a
 * quarter second of silence they'd talk through anyway. */
#define REC_SETTLE_MS 250

static void wr_u16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void wr_u32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

static void build_wav_header(uint8_t *h, uint32_t rate, uint32_t data_bytes)
{
    memcpy(h, "RIFF", 4);      wr_u32(h + 4, 36 + data_bytes); memcpy(h + 8, "WAVE", 4);
    memcpy(h + 12, "fmt ", 4); wr_u32(h + 16, 16);
    wr_u16(h + 20, 1);         /* PCM */
    wr_u16(h + 22, 1);         /* mono */
    wr_u32(h + 24, rate);
    wr_u32(h + 28, rate * 2);  /* byte rate = rate * channels * bytes/sample */
    wr_u16(h + 32, 2);         /* block align */
    wr_u16(h + 34, 16);        /* bits/sample */
    memcpy(h + 36, "data", 4); wr_u32(h + 40, data_bytes);
}

static void rec_task(void *arg)
{
    rec_args_t *a = (rec_args_t *)arg;
    int line = a->line;
    bool ok = false;

    uint32_t in_rate = audio_sample_rate();
    uint32_t decim = in_rate / 8000;
    if (decim < 1) decim = 1;
    uint32_t out_rate = in_rate / decim; /* ~8000 */

    int16_t *in = malloc(IN_CHUNK_SAMPLES * sizeof(int16_t));
    int16_t *out = malloc((IN_CHUNK_SAMPLES / decim + 1) * sizeof(int16_t));
    storage_writer_t w = { 0 };
    bool writer_open = false;
    downlink_ctx_t dlctx = { 0 };
    bool downlink_tap_added = false;
    if (!in || !out) { ESP_LOGE(TAG, "line %d: alloc failed", line); goto out; }

    /* Reserve room for a full max_ms clip so mid-record eviction can't clip us. */
    size_t reserve = 44 + (size_t)((uint64_t)out_rate * 2 * a->max_ms / 1000);
    if (storage_write_begin(a->name, reserve, &w) != ESP_OK) {
        ESP_LOGW(TAG, "line %d: storage_write_begin('%s') failed", line, a->name);
        goto out;
    }
    writer_open = true;

    uint8_t hdr[44];
    build_wav_header(hdr, out_rate, 0); /* sizes patched at the end */
    if (storage_write_chunk(&w, hdr, sizeof(hdr)) != ESP_OK) goto out;

    if (a->downlink) {
        dlctx.sb = xStreamBufferCreate(DOWNLINK_SB_SAMPLES * sizeof(int16_t), 1);
        if (!dlctx.sb) { ESP_LOGE(TAG, "line %d: downlink stream buffer alloc failed", line); goto out; }
        audio_add_extwrite_tap(line, downlink_push, &dlctx);
        downlink_tap_added = true;
    }

    ESP_LOGI(TAG, "line %d: recording '%s' (<=%u ms, %u Hz mono, %s)",
             line, a->name, (unsigned)a->max_ms, (unsigned)out_rate,
             a->downlink ? "downlink" : "uplink");
    activity_log("Line %d: recording started", line);

    uint32_t data_bytes = 0;
    int32_t  acc = 0;      /* decimation accumulator, carried across reads */
    uint32_t acc_n = 0;
    int64_t  t0 = esp_timer_get_time();
    int64_t  quiet_since = 0; /* esp_timer_get_time() when quiet began; 0 = not quiet */
    int32_t  agc_gain_q8 = 256; /* unity -- see AGC_* above */

    while (!s_rec[line].stop) {
        int64_t elapsed_ms = (esp_timer_get_time() - t0) / 1000;
        if (elapsed_ms >= (int64_t)a->max_ms) break;

        size_t n = a->downlink
            ? xStreamBufferReceive(dlctx.sb, in, IN_CHUNK_SAMPLES * sizeof(int16_t),
                                    pdMS_TO_TICKS(500)) / sizeof(int16_t)
            : audio_rx_read(line, in, IN_CHUNK_SAMPLES, 500);
        if (n == 0) continue; /* RX starved momentarily; keep waiting up to max_ms */

        size_t on = 0;
        for (size_t i = 0; i < n; i++) {
            acc += in[i];
            if (++acc_n == decim) {
                out[on++] = (int16_t)(acc / (int32_t)decim);
                acc = 0;
                acc_n = 0;
            }
        }
        if (on == 0) continue;

        /* Settle gap: decimate (keeps the accumulator in sync) but discard
         * -- don't write, don't count elapsed time against silence
         * detection below -- see REC_SETTLE_MS above. */
        if (elapsed_ms < REC_SETTLE_MS) continue;

        /* This block's own raw peak, before any gain -- drives both the
         * per-block ceiling and the smoothed estimate's next update (see
         * AGC_* comment above). */
        int32_t raw_peak = 0;
        for (size_t i = 0; i < on; i++) {
            int32_t a_ = out[i] < 0 ? -out[i] : out[i];
            if (a_ > raw_peak) raw_peak = a_;
        }

        /* Gain actually applied to THIS block: the smoothed estimate,
         * capped so this specific block's own peak can't clear
         * AGC_CEILING_PEAK no matter how stale that estimate is. */
        int32_t block_gain_q8 = agc_gain_q8;
        if (raw_peak > 0) {
            int32_t ceiling_gain = (int32_t)(((int64_t)AGC_CEILING_PEAK << 8) / raw_peak);
            if (block_gain_q8 > ceiling_gain) block_gain_q8 = ceiling_gain;
        }
        for (size_t i = 0; i < on; i++) {
            int32_t v = ((int32_t)out[i] * block_gain_q8) >> 8;
            if (v > INT16_MAX) v = INT16_MAX;
            else if (v < INT16_MIN) v = INT16_MIN;
            out[i] = (int16_t)v;
        }
        /* Re-estimate the smoothed gain for the *next* block from this
         * block's raw (pre-gain) peak -- one-block lag, same shape as
         * dtmf.c's AGC, but no longer the only thing standing between a
         * transient and clipping (the ceiling above handles that now). */
        int32_t chunk_peak = raw_peak;
        if (chunk_peak >= AGC_GATE_PEAK) {
            int32_t target = (int32_t)(((int64_t)AGC_TARGET_PEAK << 8) / chunk_peak);
            if (target < AGC_MIN_GAIN_Q8) target = AGC_MIN_GAIN_Q8;
            if (target > AGC_MAX_GAIN_Q8) target = AGC_MAX_GAIN_Q8;
            agc_gain_q8 += (target - agc_gain_q8) / AGC_SMOOTH_DIV;
        }

        if (storage_write_chunk(&w, out, on * sizeof(int16_t)) != ESP_OK) goto out;
        data_bytes += on * sizeof(int16_t);

        /* Trailing-silence auto-stop, after a min duration so a slow start to
         * speaking doesn't cut it off. */
        if (a->silence_ms && elapsed_ms > 1000) {
            double sum = 0;
            for (size_t i = 0; i < on; i++) { double s = out[i]; sum += s * s; }
            double rms = sqrt(sum / on);
            int64_t now = esp_timer_get_time();
            if (rms < SILENCE_RMS) {
                if (!quiet_since) quiet_since = now;
                else if ((now - quiet_since) / 1000 >= (int64_t)a->silence_ms) break;
            } else {
                quiet_since = 0;
            }
        }
    }

    storage_write_end(&w);
    writer_open = false;

    /* Patch the header's RIFF/data sizes now that the length is known. */
    uint8_t sz[4];
    wr_u32(sz, 36 + data_bytes); storage_patch(a->name, 4, sz, 4);
    wr_u32(sz, data_bytes);      storage_patch(a->name, 40, sz, 4);

    ok = (data_bytes > 0);
    ESP_LOGI(TAG, "line %d: recorded '%s' (%u bytes PCM, %.1f s)",
             line, a->name, (unsigned)data_bytes,
             (double)data_bytes / 2.0 / (double)out_rate);
    activity_log("Line %d: recording saved (%.1fs)",
                  line, (double)data_bytes / 2.0 / (double)out_rate);

out:
    if (downlink_tap_added) {
        audio_remove_extwrite_tap(line);
        if (dlctx.sb) vStreamBufferDelete(dlctx.sb);
    }
    if (writer_open) storage_write_abort(&w);
    free(in);
    free(out);
    s_rec[line].active = false;
    if (a->cb) a->cb(line, ok, a->ctx);
    free(a);
    vTaskDelete(NULL);
}

static esp_err_t recorder_start_ex(int line, const char *storage_name, uint32_t max_ms,
                                    uint32_t silence_ms, bool downlink,
                                    recorder_done_cb_t done_cb, void *ctx)
{
    if (line < 0 || line >= AUDIO_NUM_LINES) return ESP_ERR_INVALID_ARG;
    if (s_rec[line].active) return ESP_ERR_INVALID_STATE;

    rec_args_t *a = malloc(sizeof(*a));
    if (!a) return ESP_ERR_NO_MEM;
    a->line = line;
    strncpy(a->name, storage_name, sizeof(a->name) - 1);
    a->name[sizeof(a->name) - 1] = 0;
    a->max_ms = max_ms;
    a->silence_ms = silence_ms;
    a->downlink = downlink;
    a->cb = done_cb;
    a->ctx = ctx;

    s_rec[line].active = true;
    s_rec[line].stop = false;

    if (xTaskCreate(rec_task, "recorder", 4096, a, tskIDLE_PRIORITY + 3, NULL) != pdPASS) {
        s_rec[line].active = false;
        free(a);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t recorder_start(int line, const char *storage_name, uint32_t max_ms,
                         uint32_t silence_ms, recorder_done_cb_t done_cb, void *ctx)
{
    return recorder_start_ex(line, storage_name, max_ms, silence_ms, false, done_cb, ctx);
}

esp_err_t recorder_start_downlink(int line, const char *storage_name, uint32_t max_ms,
                                  uint32_t silence_ms, recorder_done_cb_t done_cb, void *ctx)
{
    return recorder_start_ex(line, storage_name, max_ms, silence_ms, true, done_cb, ctx);
}

void recorder_stop(int line)
{
    if (line < 0 || line >= AUDIO_NUM_LINES) return;
    s_rec[line].stop = true;
}

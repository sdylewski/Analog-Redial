#include "recorder.h"
#include "audio.h"
#include "storage.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

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
    recorder_done_cb_t cb;
    void    *ctx;
} rec_args_t;

/* 48 kHz input, read a whole number of decimation groups per pull (256 groups
 * of 6 = 1536 samples). Kept modest so the RX ring buffer never overflows. */
#define IN_CHUNK_SAMPLES 1536
#define SILENCE_RMS      500  /* ~ -36 dBFS; below this counts as "quiet" */

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

    ESP_LOGI(TAG, "line %d: recording '%s' (<=%u ms, %u Hz mono)",
             line, a->name, (unsigned)a->max_ms, (unsigned)out_rate);

    uint32_t data_bytes = 0;
    int32_t  acc = 0;      /* decimation accumulator, carried across reads */
    uint32_t acc_n = 0;
    int64_t  t0 = esp_timer_get_time();
    int64_t  quiet_since = 0; /* esp_timer_get_time() when quiet began; 0 = not quiet */

    while (!s_rec[line].stop) {
        int64_t elapsed_ms = (esp_timer_get_time() - t0) / 1000;
        if (elapsed_ms >= (int64_t)a->max_ms) break;

        size_t n = audio_rx_read(line, in, IN_CHUNK_SAMPLES, 500);
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

out:
    if (writer_open) storage_write_abort(&w);
    free(in);
    free(out);
    s_rec[line].active = false;
    if (a->cb) a->cb(line, ok, a->ctx);
    free(a);
    vTaskDelete(NULL);
}

esp_err_t recorder_start(int line, const char *storage_name, uint32_t max_ms,
                         uint32_t silence_ms, recorder_done_cb_t done_cb, void *ctx)
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

void recorder_stop(int line)
{
    if (line < 0 || line >= AUDIO_NUM_LINES) return;
    s_rec[line].stop = true;
}

bool recorder_is_active(int line)
{
    if (line < 0 || line >= AUDIO_NUM_LINES) return false;
    return s_rec[line].active;
}

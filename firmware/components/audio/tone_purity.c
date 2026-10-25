#include "audio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* The two test tones, then products a non-linear stage would add: the
 * difference/sum (100, 800), harmonics (700, 900, 1050), and third-order
 * intermodulation (250, 550, 1150, 1250, 1350). All multiples of 2 Hz, so
 * half a second of audio puts each exactly on an analysis bin. */
#define N_FREQS 12
static const float FREQS[N_FREQS] = { 350, 450, 100, 250, 550, 700, 800, 900, 1050, 1150, 1250, 1350 };

typedef struct {
    int16_t *buf;
    uint32_t n, target;
    volatile bool done;
} capture_t;

/* Runs in the audio task: copy only. Soft-float DSP here would eat most of
 * each 10 ms frame and cause the glitches being measured. */
static void capture_tap(int line, const int16_t *mono, size_t n, void *ctx)
{
    capture_t *c = (capture_t *)ctx;
    size_t room = c->target - c->n;
    if (n > room) n = room;
    memcpy(c->buf + c->n, mono, n * sizeof(int16_t));
    c->n += n;
    if (c->n >= c->target) c->done = true;
}

/* Per-line noise floor (hum, hiss, SLIC spurs) at each analysis frequency,
 * measured with no tone playing; subtracted in power so that a quiet test
 * tone is not blamed for noise that is there regardless of level. */
static float s_base[AUDIO_NUM_LINES][N_FREQS];
static bool s_base_ok[AUDIO_NUM_LINES];

static float to_db(float v) { return 20.0f * log10f(v + 1e-9f); }

static esp_err_t measure(int line, bool digital, bool tone_on, float amp[N_FREQS], int32_t *peak_out, uint32_t *clipped_out)
{
    uint32_t fs = audio_sample_rate();

    /* 0.5 s is plenty, but a fragmented heap (the Diagnostics page is big) may
     * not have 48 KB in one piece; settle for a shorter capture, down to 0.125 s
     * (still ~8 Hz resolution, far finer than the 100 Hz spacing it must resolve). */
    capture_t cap = { .target = fs / 2 };
    while (!(cap.buf = malloc(cap.target * sizeof(int16_t))) && cap.target > fs / 8) cap.target /= 2;
    if (!cap.buf) return ESP_ERR_NO_MEM;

    /* 2.2 s burst, same level as the dial tone (0.6 of full scale, split
     * across the two tones). The first 0.9 s holds the SLIC's start-up pop and
     * settling, so only the following 0.5 s is captured. */
    if (tone_on) audio_tx_burst(line, FREQS[0], FREQS[1], 0.6f, 2200);
    vTaskDelay(pdMS_TO_TICKS(tone_on ? 900 : 600));   /* also lets a paused audio pipeline restart cleanly */
    if (digital) audio_add_downlink_tap(line, capture_tap, &cap);
    else         audio_add_diag_tap(line, capture_tap, &cap);
    for (int i = 0; i < 30 && !cap.done; i++) vTaskDelay(pdMS_TO_TICKS(100));
    if (digital) audio_remove_downlink_tap(line);
    else         audio_remove_diag_tap(line);
    if (tone_on) audio_tx_silence(line);
    if (!cap.done) { free(cap.buf); return ESP_ERR_TIMEOUT; }

    for (int k = 0; k < N_FREQS; k++) {
        float coeff = 2.0f * cosf(2.0f * (float)M_PI * FREQS[k] / (float)fs), q1 = 0, q2 = 0;
        for (uint32_t i = 0; i < cap.n; i++) {
            float q0 = (float)cap.buf[i] / 32768.0f + coeff * q1 - q2;
            q2 = q1; q1 = q0;
        }
        float p = q1 * q1 + q2 * q2 - coeff * q1 * q2;
        amp[k] = 2.0f * sqrtf(p > 0 ? p : 0) / (float)cap.n;    /* peak amplitude, 1.0 = full scale */
    }
    int32_t peak = 0;
    uint32_t clipped = 0;
    for (uint32_t i = 0; i < cap.n; i++) {
        int32_t a = cap.buf[i] < 0 ? -cap.buf[i] : cap.buf[i];
        if (a > peak) peak = a;
        if (a >= 32767) clipped++;
    }
    free(cap.buf);
    *peak_out = peak;
    *clipped_out = clipped;
    return ESP_OK;
}

esp_err_t audio_tone_purity_baseline(int line)
{
    if (line < 0 || line >= AUDIO_NUM_LINES) return ESP_ERR_INVALID_ARG;
    s_base_ok[line] = false;
    float amp[N_FREQS]; int32_t pk; uint32_t cl;
    esp_err_t e = measure(line, false, false, amp, &pk, &cl);
    if (e != ESP_OK) return e;
    memcpy(s_base[line], amp, sizeof(amp));
    s_base_ok[line] = true;
    return ESP_OK;
}

void audio_tone_purity_baseline_clear(int line)
{
    if (line >= 0 && line < AUDIO_NUM_LINES) s_base_ok[line] = false;
}

esp_err_t audio_tone_purity(int line, bool digital, tone_purity_t *out)
{
    if (!out || line < 0 || line >= AUDIO_NUM_LINES) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    float amp[N_FREQS]; int32_t peak; uint32_t clipped;
    esp_err_t e = measure(line, digital, true, amp, &peak, &clipped);
    if (e != ESP_OK) return e;
    out->clipped = clipped;
    if (!digital && s_base_ok[line]) {
        for (int k = 2; k < N_FREQS; k++) {
            float d = amp[k] * amp[k] - s_base[line][k] * s_base[line][k];
            amp[k] = d > 0 ? sqrtf(d) : 0;
        }
    }

    float tone = sqrtf(amp[0] * amp[0] + amp[1] * amp[1]);
    float dist = 0;
    for (int k = 2; k < N_FREQS; k++) dist += amp[k] * amp[k];
    dist = sqrtf(dist);

    out->peak_dbfs = to_db((float)peak / 32768.0f);
    out->tone_a_dbfs = to_db(amp[0]);
    out->tone_b_dbfs = to_db(amp[1]);
    out->tone_dbfs = to_db(tone / 1.4142f);   /* each tone's level, averaged */
    out->distortion_pct = tone > 0 ? 100.0f * dist / tone : 100.0f;
    for (int pass = 0; pass < 3; pass++) {
        int best = -1;
        for (int k = 2; k < N_FREQS; k++)
            if (amp[k] >= 0 && (best < 0 || amp[k] > amp[best])) best = k;
        out->top_hz[pass] = FREQS[best];
        out->top_dbfs[pass] = to_db(amp[best]);
        amp[best] = -1.0f;
    }
    /* Nothing came back at the test frequencies: the line isn't powered or
     * its Vout path is open. */
    if (tone < 0.003f) return ESP_ERR_NOT_FOUND;
    return ESP_OK;
}

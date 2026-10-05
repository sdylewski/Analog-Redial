/* fx_test.c -- deterministic checks of the voice effects: a pure tone goes in,
 * the strongest spectral lines coming out say what the effect actually did
 * (chipmunk at 1000 Hz in should be 1500 Hz out, and so on).
 *
 *   audio_fx_tone_test():  tone -> effect -> measure, all in software. Proves the
 *                          DSP itself, and times it against the 10 ms frame.
 *   audio_fx_roundtrip():  tone -> the engine's real effect path -> DAC -> SLIC ->
 *                          back in through the hybrid and ADC -> measure. Differences
 *                          from the software run are what the hardware adds.
 *
 * Measurement works on 8 kHz-decimated audio (boxcar average), 0.25 s Hann
 * window, Goertzel on a 10 Hz grid then 1 Hz refinement. */
#include "audio.h"
#include "voice_fx.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define NDEC      2000   /* 0.25 s at 8 kHz */
#define NENV      25     /* 10 ms blocks in the same 0.25 s */

typedef struct {
    int16_t *y;
    uint32_t ny, dec, k;
    int32_t  acc;
    uint32_t blk_len, blk_n, nenv;
    uint64_t blk_sq, sq;
    uint32_t env[NENV];
    uint32_t n, clipped;
    int32_t  peak;
    volatile bool full;
} accum_t;

static void accum_init(accum_t *a, int16_t *y, uint32_t fs)
{
    memset(a, 0, sizeof(*a));
    a->y = y;
    a->dec = fs / 8000;
    a->blk_len = fs / 100;
}

static void accum_feed(accum_t *a, const int16_t *x, size_t n)
{
    for (size_t i = 0; i < n && !a->full; i++) {
        int32_t v = x[i], m = v < 0 ? -v : v;
        if (m > a->peak) a->peak = m;
        if (m >= 32767) a->clipped++;
        a->sq += (uint64_t)(v * v);
        a->n++;
        a->blk_sq += (uint64_t)(v * v);
        if (++a->blk_n == a->blk_len) {
            if (a->nenv < NENV) a->env[a->nenv++] = (uint32_t)sqrtf((float)(a->blk_sq / a->blk_n));
            a->blk_sq = 0;
            a->blk_n = 0;
        }
        a->acc += v;
        if (++a->k == a->dec) {
            if (a->ny < NDEC) a->y[a->ny++] = (int16_t)(a->acc / (int32_t)a->dec);
            a->acc = 0;
            a->k = 0;
        }
        if (a->ny >= NDEC && a->nenv >= NENV) a->full = true;
    }
}

static float to_db(float v) { return 20.0f * log10f(v + 1e-9f); }

static float goertzel_amp(const float *w, float hz)
{
    float coeff = 2.0f * cosf(2.0f * (float)M_PI * hz / 8000.0f), q1 = 0, q2 = 0;
    for (int i = 0; i < NDEC; i++) {
        float q0 = w[i] + coeff * q1 - q2;
        q2 = q1;
        q1 = q0;
    }
    float p = q1 * q1 + q2 * q2 - coeff * q1 * q2;
    return 4.0f * sqrtf(p > 0 ? p : 0) / (float)NDEC;   /* Hann: amplitude = 4|X|/N */
}

static float refine(const float *w, float f0, float *amp)
{
    float best = f0, ba = -1;
    for (float f = f0 - 9; f <= f0 + 9.5f; f += 1.0f) {
        float a = goertzel_amp(w, f);
        if (a > ba) { ba = a; best = f; }
    }
    *amp = ba;
    return best;
}

static esp_err_t analyze(const accum_t *a, fx_tone_result_t *o)
{
    float *w = malloc(NDEC * sizeof(float));
    enum { NG = 390 };
    float *g = malloc(NG * sizeof(float));
    if (!w || !g) { free(w); free(g); return ESP_ERR_NO_MEM; }
    for (int i = 0; i < NDEC; i++) {
        float h = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * i / (NDEC - 1));
        w[i] = (float)a->y[i] / 32768.0f * h;
    }
    int top = 0;
    for (int b = 0; b < NG; b++) {
        g[b] = goertzel_amp(w, 20.0f + 10.0f * b);
        if (g[b] > g[top]) top = b;
    }
    int sec = -1;
    for (int b = 0; b < NG; b++) {
        if (abs(b - top) < 4) continue;
        if (sec < 0 || g[b] > g[sec]) sec = b;
    }
    float a1, a2 = 0;
    o->peak_hz = refine(w, 20.0f + 10.0f * top, &a1);
    o->second_hz = sec >= 0 ? refine(w, 20.0f + 10.0f * sec, &a2) : 0;
    free(w);
    free(g);
    o->peak_dbfs = to_db(a1);
    o->second_dbfs = to_db(a2);
    float rms = sqrtf((float)(a->sq / (a->n ? a->n : 1))) / 32768.0f;
    o->rms_dbfs = to_db(rms);
    o->sample_peak_dbfs = to_db((float)a->peak / 32768.0f);
    float lines = (a1 * a1 + a2 * a2) / 2.0f;
    o->other_pct = rms > 0 ? 100.0f * fmaxf(0.0f, 1.0f - lines / (rms * rms)) : 0;
    uint32_t emin = UINT32_MAX, emax = 0;
    for (int i = 0; i < NENV; i++) {
        if (a->env[i] < emin) emin = a->env[i];
        if (a->env[i] > emax) emax = a->env[i];
    }
    o->env_ratio = emax ? (float)emin / (float)emax : 0;
    o->clipped = a->clipped;
    return ESP_OK;
}

esp_err_t audio_fx_tone_test(voice_fx_t fx, float hz, float amp, fx_tone_result_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    uint32_t fs = audio_sample_rate();
    size_t n = fs / 100;
    int16_t *frame = malloc(n * sizeof(int16_t));
    int16_t *y = malloc(NDEC * sizeof(int16_t));
    voice_fx_state_t *st = fx == VOICE_FX_NONE ? NULL : voice_fx_create(fs);
    accum_t *a = malloc(sizeof(*a));
    if (!frame || !y || !a || (fx != VOICE_FX_NONE && !st)) {
        free(frame); free(y); free(a); voice_fx_destroy(st);
        return ESP_ERR_NO_MEM;
    }
    accum_init(a, y, fs);
    float phase = 0, step = 2.0f * (float)M_PI * hz / (float)fs;
    int64_t tsum = 0, tmax = 0;
    int timed = 0;
    for (int f = 0; f < 400 && !a->full; f++) {
        for (size_t i = 0; i < n; i++) {
            frame[i] = (int16_t)(amp * 32767.0f * sinf(phase));
            phase += step;
            if (phase > 2.0f * (float)M_PI) phase -= 2.0f * (float)M_PI;
        }
        int64_t t0 = esp_timer_get_time();
        if (st) voice_fx_process(st, fx, frame, n);
        int64_t dt = esp_timer_get_time() - t0;
        if (f >= 30) {                 /* 0.3 s lets the pitch window and combs fill */
            tsum += dt; timed++;
            if (dt > tmax) tmax = dt;
            accum_feed(a, frame, n);
        }
        if ((f & 15) == 15) vTaskDelay(1);   /* the console task shares the CPU */
    }
    esp_err_t r = a->full ? analyze(a, out) : ESP_FAIL;
    if (st) {
        voice_fx_stats_t s;
        voice_fx_take_stats(st, &s);
        out->limited = s.limited;
    }
    out->us_avg = timed ? (float)tsum / timed : 0;
    out->us_max = (float)tmax;
    out->frame_us = 10000;
    voice_fx_destroy(st);
    free(frame); free(y); free(a);
    return r;
}

static void rt_tap(int line, const int16_t *mono, size_t n, void *ctx)
{
    (void)line;
    accum_feed((accum_t *)ctx, mono, n);
}

esp_err_t audio_fx_roundtrip(int line, voice_fx_t fx, float hz, float amp, fx_tone_result_t *out)
{
    if (!out || line < 0 || line >= AUDIO_NUM_LINES) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    uint32_t fs = audio_sample_rate();
    size_t n = fs / 100;
    int16_t *frame = malloc(n * sizeof(int16_t));
    int16_t *y = malloc(NDEC * sizeof(int16_t));
    accum_t *a = malloc(sizeof(*a));
    if (!frame || !y || !a) { free(frame); free(y); free(a); return ESP_ERR_NO_MEM; }
    accum_init(a, y, fs);

    esp_err_t r = audio_set_voice_fx(line, fx);
    if (r != ESP_OK) { free(frame); free(y); free(a); return r; }
    audio_tx_external(line, true);
    float phase = 0, step = 2.0f * (float)M_PI * hz / (float)fs;
    bool tapped = false;
    for (int f = 0; f < 300 && !a->full; f++) {
        for (size_t i = 0; i < n; i++) {
            frame[i] = (int16_t)(amp * 32767.0f * sinf(phase));
            phase += step;
            if (phase > 2.0f * (float)M_PI) phase -= 2.0f * (float)M_PI;
        }
        size_t done = 0;
        int spins = 0;
        while (done < n && spins < 500) {
            size_t w = audio_ext_write(line, frame + done, n - done);
            done += w;
            if (!w) { vTaskDelay(2); spins++; }
        }
        if (f == 70 && !tapped) {      /* 0.7 s: tone, effect and line have settled */
            audio_add_diag_tap(line, rt_tap, a);
            tapped = true;
        }
    }
    if (tapped) audio_remove_diag_tap(line);
    audio_tx_silence(line);
    audio_set_voice_fx(line, VOICE_FX_NONE);
    r = a->full ? analyze(a, out) : ESP_ERR_TIMEOUT;
    free(frame); free(y); free(a);
    return r;
}

#include "voice_fx.h"
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define DALEK_HZ          30
#define PITCH_WINDOW_MS   30
#define PITCH_UP_Q16      98304   /* 1.5 */
#define PITCH_DOWN_Q16    45875   /* 0.7 */
#define CRUSH_RATE_HZ     4000
#define CRUSH_MASK        0xF800  /* keep the top 5 bits */

struct voice_fx_state {
    uint32_t   sr;
    voice_fx_t last;

    uint32_t ring_phase, ring_step;      /* Q32 turns */

    /* Pitch shifter: a short delay line read by two taps half a window
     * apart, swept at (1 - ratio) samples per sample and crossfaded with
     * triangular gains, so whichever tap is about to wrap is silent. */
    int16_t *buf;
    uint32_t mask, w;
    int32_t  win_q16, half_q16, d_q16;

    uint32_t hold_n, hold_left;
    int16_t  held;
};

static int16_t s_sine[256];
static bool s_sine_ready;

voice_fx_state_t *voice_fx_create(uint32_t sample_rate)
{
    if (!s_sine_ready) {
        for (int i = 0; i < 256; i++) s_sine[i] = (int16_t)lrintf(32767.0f * sinf(i * 6.2831853f / 256));
        s_sine_ready = true;
    }
    voice_fx_state_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->sr = sample_rate;
    uint32_t win = sample_rate * PITCH_WINDOW_MS / 1000;
    uint32_t size = 1;
    while (size < win + 4) size <<= 1;
    s->buf = calloc(size, sizeof(int16_t));
    if (!s->buf) { free(s); return NULL; }
    s->mask = size - 1;
    s->win_q16 = (int32_t)(win << 16);
    s->half_q16 = s->win_q16 / 2;
    s->ring_step = (uint32_t)(((uint64_t)DALEK_HZ << 32) / sample_rate);
    s->hold_n = sample_rate / CRUSH_RATE_HZ ? sample_rate / CRUSH_RATE_HZ : 1;
    return s;
}

static void reset(voice_fx_state_t *s)
{
    memset(s->buf, 0, (s->mask + 1) * sizeof(int16_t));
    s->w = 0;
    s->d_q16 = 0;
    s->ring_phase = 0;
    s->hold_left = 0;
}

static inline int16_t sat16(int32_t v)
{
    return v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v;
}

/* Reads the delay line `d` (Q16 samples) behind the newest sample, linearly
 * interpolated. +2 samples keeps the newer interpolation point written. */
static inline int32_t tap(const voice_fx_state_t *s, int32_t d)
{
    int64_t pos = ((int64_t)s->w << 16) - d - (2 << 16);
    uint32_t i = (uint32_t)(pos >> 16);
    int32_t frac = (int32_t)(pos & 0xFFFF);
    int32_t a = s->buf[i & s->mask], b = s->buf[(i + 1) & s->mask];
    return a + (int32_t)(((int64_t)(b - a) * frac) >> 16);
}

static inline int32_t tri_gain_q15(const voice_fx_state_t *s, int32_t d)
{
    int32_t dist = d > s->half_q16 ? d - s->half_q16 : s->half_q16 - d;
    return 32768 - (int32_t)(((int64_t)dist << 15) / s->half_q16);
}

static void pitch(voice_fx_state_t *s, int32_t ratio_q16, int16_t *x, size_t n)
{
    int32_t step = 65536 - ratio_q16;
    for (size_t k = 0; k < n; k++) {
        s->buf[s->w & s->mask] = x[k];
        int32_t d1 = s->d_q16;
        int32_t d2 = d1 + s->half_q16;
        if (d2 >= s->win_q16) d2 -= s->win_q16;
        int32_t y = (int32_t)(((int64_t)tap(s, d1) * tri_gain_q15(s, d1) +
                               (int64_t)tap(s, d2) * tri_gain_q15(s, d2)) >> 15);
        x[k] = sat16(y);
        s->d_q16 += step;
        if (s->d_q16 < 0) s->d_q16 += s->win_q16;
        else if (s->d_q16 >= s->win_q16) s->d_q16 -= s->win_q16;
        s->w++;
    }
}

void voice_fx_process(voice_fx_state_t *s, voice_fx_t fx, int16_t *x, size_t n)
{
    if (!s || fx == VOICE_FX_NONE) return;
    if (fx != s->last) {
        reset(s);
        s->last = fx;
    }
    switch (fx) {
    case VOICE_FX_DALEK:
        for (size_t k = 0; k < n; k++) {
            /* x1.5 makes up most of the level a ring modulator costs on
             * average without clipping loud speech. */
            int32_t m = s_sine[s->ring_phase >> 24];
            x[k] = sat16((int32_t)(((int64_t)x[k] * m * 3) >> 16));
            s->ring_phase += s->ring_step;
        }
        break;
    case VOICE_FX_CHIPMUNK:
        pitch(s, PITCH_UP_Q16, x, n);
        break;
    case VOICE_FX_DEEP:
        pitch(s, PITCH_DOWN_Q16, x, n);
        break;
    case VOICE_FX_8BIT:
        for (size_t k = 0; k < n; k++) {
            if (s->hold_left == 0) {
                s->held = (int16_t)(x[k] & CRUSH_MASK);
                s->hold_left = s->hold_n;
            }
            s->hold_left--;
            x[k] = s->held;
        }
        break;
    default:
        break;
    }
}

const char *voice_fx_name(voice_fx_t fx)
{
    switch (fx) {
    case VOICE_FX_DALEK:    return "Dalek";
    case VOICE_FX_CHIPMUNK: return "chipmunk";
    case VOICE_FX_DEEP:     return "deep voice";
    case VOICE_FX_8BIT:     return "8-bit";
    default:                return "normal";
    }
}

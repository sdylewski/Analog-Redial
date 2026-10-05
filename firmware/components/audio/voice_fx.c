#include "voice_fx.h"
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define DALEK_HZ          30
#define PITCH_WINDOW_MS   30
#define PITCH_UP_Q16      98304   /* 1.5 */
#define VADER_PITCH_Q16   52429   /* 0.8 */
#define VADER_MASK_US     1500    /* short comb: the mask's boxy resonance */
#define VADER_MASK_FB_Q15 9830    /* 0.3 */
#define ROBOT_HZ          110     /* comb delay = one period of this */
#define ROBOT_FB_Q15      19661   /* 0.6 */
#define TREMOLO_HZ        6
#define TREMOLO_DEPTH_Q15 22938   /* 0.7: dips to 30% volume */
#define CRUSH_RATE_HZ     4000
#define CRUSH_MASK        0xF800  /* keep the top 5 bits */

struct voice_fx_state {
    uint32_t   sr;
    voice_fx_t last;

    uint32_t ring_phase, ring_step;      /* Q32 turns; ring_phase also drives tremolo */
    uint32_t trem_step;

    /* Pitch shifter: a short delay line read by two taps half a window
     * apart, swept at (1 - ratio) samples per sample and crossfaded with
     * triangular gains, so whichever tap is about to wrap is silent. */
    int16_t *buf;
    uint32_t mask, w;
    int32_t  win_q16, half_q16, d_q16;

    uint32_t hold_n, hold_left;
    int16_t  held;

    /* Diagnostics: window since the last voice_fx_take_stats(). */
    uint32_t st_n, st_limited;
    int32_t  st_in_peak, st_out_peak;
    uint64_t st_in_sq, st_out_sq;

    /* Feedback comb (robot, Vader's mask): y[n] = x[n] + g * y[n - D]. */
    int16_t *comb;
    uint32_t comb_mask, cw, robot_d, mask_d;
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
    uint32_t csize = 1;
    while (csize < sample_rate / 100 + 4) csize <<= 1;   /* >= 10 ms */
    s->comb = calloc(csize, sizeof(int16_t));
    if (!s->comb) { free(s->buf); free(s); return NULL; }
    s->comb_mask = csize - 1;
    s->robot_d = sample_rate / ROBOT_HZ;
    s->mask_d = sample_rate * VADER_MASK_US / 1000000;
    s->win_q16 = (int32_t)(win << 16);
    s->half_q16 = s->win_q16 / 2;
    s->ring_step = (uint32_t)(((uint64_t)DALEK_HZ << 32) / sample_rate);
    s->trem_step = (uint32_t)(((uint64_t)TREMOLO_HZ << 32) / sample_rate);
    s->hold_n = sample_rate / CRUSH_RATE_HZ ? sample_rate / CRUSH_RATE_HZ : 1;
    return s;
}

static void reset(voice_fx_state_t *s)
{
    memset(s->buf, 0, (s->mask + 1) * sizeof(int16_t));
    memset(s->comb, 0, (s->comb_mask + 1) * sizeof(int16_t));
    s->w = 0;
    s->cw = 0;
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

/* Feedback comb. Output at 0.7x: speech mostly falls between the comb's
 * resonant peaks, so this keeps it about as loud as the dry voice. */
static void comb(voice_fx_state_t *s, uint32_t d, int32_t g_q15, int16_t *x, size_t n)
{
    const int32_t out_q15 = 22938;
    for (size_t k = 0; k < n; k++) {
        int32_t back = s->comb[(s->cw - d) & s->comb_mask];
        int32_t y = x[k] + ((back * g_q15) >> 15);
        s->comb[s->cw & s->comb_mask] = sat16(y);
        s->cw++;
        x[k] = sat16((y * out_q15) >> 15);
    }
}

/* Soft clip 1.5v - 0.5v^3 on a `drive`-times signal, then `out_q15` level. */
static void overdrive(int16_t *x, size_t n, int32_t drive, int32_t out_q15)
{
    for (size_t k = 0; k < n; k++) {
        int64_t v = sat16((int32_t)x[k] * drive);
        int64_t y = (3 * v) / 2 - (v * v * v) / (2LL * 32768 * 32768);
        x[k] = sat16((int32_t)((sat16((int32_t)y) * out_q15) >> 15));
    }
}

/* Last stage of every effect: bend anything above 20000 over to a slope of
 * 1/4 (so 32767 comes out at ~23200). The effects are level-matched to plain
 * speech, but the phone line's audio input starts distorting well before the
 * codec's full scale, so a rare loud peak shouldn't get through at full size. */
static volatile int s_gain_pct = 100;

void voice_fx_set_gain_pct(int pct) { s_gain_pct = pct < 10 ? 10 : (pct > 300 ? 300 : pct); }
int voice_fx_gain_pct(void) { return s_gain_pct; }

static uint32_t limit(int16_t *x, size_t n)
{
    uint32_t hit = 0;
    for (size_t k = 0; k < n; k++) {
        int32_t v = x[k], a = v < 0 ? -v : v;
        if (a > 20000) {
            a = 20000 + ((a - 20000) >> 2);
            x[k] = (int16_t)(v < 0 ? -a : a);
            hit++;
        }
    }
    return hit;
}

static void measure(const int16_t *x, size_t n, int32_t *peak, uint64_t *sq)
{
    for (size_t k = 0; k < n; k++) {
        int32_t a = x[k] < 0 ? -x[k] : x[k];
        if (a > *peak) *peak = a;
        *sq += (uint64_t)(a * a);
    }
}

void voice_fx_process(voice_fx_state_t *s, voice_fx_t fx, int16_t *x, size_t n)
{
    if (!s || fx == VOICE_FX_NONE) return;
    if (fx != s->last) {
        reset(s);
        s->last = fx;
    }
    measure(x, n, &s->st_in_peak, &s->st_in_sq);
    switch (fx) {
    case VOICE_FX_DALEK:
        for (size_t k = 0; k < n; k++) {
            /* Multiplying by a sine halves the average power; 1.25x wins
             * back some loudness without clipping full-scale speech. */
            int32_t m = s_sine[s->ring_phase >> 24];
            x[k] = sat16((int32_t)(((int64_t)x[k] * m * 5) >> 17));
            s->ring_phase += s->ring_step;
        }
        break;
    case VOICE_FX_CHIPMUNK:
        pitch(s, PITCH_UP_Q16, x, n);
        break;
    case VOICE_FX_VADER:
        pitch(s, VADER_PITCH_Q16, x, n);
        overdrive(x, n, 2, 22300);   /* 0.68: its drive raises RMS ~3.4 dB over the input */
        comb(s, s->mask_d, VADER_MASK_FB_Q15, x, n);
        break;
    case VOICE_FX_ROBOT:
        comb(s, s->robot_d, ROBOT_FB_Q15, x, n);
        break;
    case VOICE_FX_TREMOLO:
        for (size_t k = 0; k < n; k++) {
            /* gain = 1 - depth * (1 + sin) / 2 */
            int32_t lfo = 32768 + s_sine[s->ring_phase >> 24];          /* 0..65535 */
            int32_t g = 32768 - (int32_t)(((int64_t)TREMOLO_DEPTH_Q15 * lfo) >> 16);
            x[k] = sat16((x[k] * g) >> 15);
            s->ring_phase += s->trem_step;
        }
        break;
    case VOICE_FX_DISTORTION:
        /* 8x into the clipper flattens almost every peak; the output level
         * below brings the square-ish result back to about the voice's RMS. */
        overdrive(x, n, 8, 7500);    /* 0.23: an 8x-clipped voice is ~2.6x the input RMS otherwise */
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
    int g = s_gain_pct;
    if (g != 100) {
        for (size_t k = 0; k < n; k++) x[k] = sat16((x[k] * g) / 100);
    }
    s->st_limited += limit(x, n);
    measure(x, n, &s->st_out_peak, &s->st_out_sq);
    s->st_n += n;
}

void voice_fx_take_stats(voice_fx_state_t *s, voice_fx_stats_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!s) return;
    out->fx = s->last;
    out->samples = s->st_n;
    out->in_peak = s->st_in_peak;
    out->out_peak = s->st_out_peak;
    out->limited = s->st_limited;
    if (s->st_n) {
        out->in_rms = (uint32_t)sqrt((double)s->st_in_sq / s->st_n);
        out->out_rms = (uint32_t)sqrt((double)s->st_out_sq / s->st_n);
    }
    s->st_n = s->st_limited = 0;
    s->st_in_peak = s->st_out_peak = 0;
    s->st_in_sq = s->st_out_sq = 0;
}

const char *voice_fx_name(voice_fx_t fx)
{
    switch (fx) {
    case VOICE_FX_DALEK:    return "Dalek";
    case VOICE_FX_CHIPMUNK: return "chipmunk";
    case VOICE_FX_VADER:    return "Vader";
    case VOICE_FX_8BIT:     return "8-bit";
    case VOICE_FX_ROBOT:    return "robot";
    case VOICE_FX_TREMOLO:  return "tremolo";
    case VOICE_FX_DISTORTION: return "distortion";
    default:                return "normal";
    }
}

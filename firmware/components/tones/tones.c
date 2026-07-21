#include "tones.h"
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Phase-accumulator (DDS) oscillator over a precomputed sine table -- not
 * sinf() per sample. The ESP32-C6 has no hardware FPU (rv32imac, no f/d
 * extension), so a sinf()-per-sample oscillator running continuously for
 * dial tone/ringback/busy is soft-float in the audio hot path -- the same
 * class of bug fixed in the DTMF Goertzel detector (components/dtmf/dtmf.c).
 * It stayed hidden here until the SLIC hook-detect fix made dial tone
 * actually get triggered for real. */

#define SINE_BITS 10
#define SINE_SIZE (1 << SINE_BITS)
static int16_t s_sine[SINE_SIZE];
static bool    s_sine_ready = false;

static void ensure_sine_table(void)
{
    if (s_sine_ready) return;
    for (int i = 0; i < SINE_SIZE; i++) {
        float phase = (2.0f * (float)M_PI * (float)i) / (float)SINE_SIZE;
        s_sine[i] = (int16_t)lroundf(sinf(phase) * 32767.0f);
    }
    s_sine_ready = true;
}

static inline int16_t sine_lookup(uint32_t phase)
{
    return s_sine[phase >> (32 - SINE_BITS)];
}

void tone_osc_init(tone_osc_t *o, uint32_t sample_rate, float f1, float f2, float amp)
{
    ensure_sine_table();
    o->f1 = f1;
    o->f2 = f2;
    o->amp = amp;
    o->sample_rate = sample_rate;
    o->phase1 = 0;
    o->phase2 = 0;

    /* One-time float/double math here is fine -- it's the per-sample loop
     * in tone_osc_fill() that can't afford soft-float, not tone start. */
    o->inc1 = (uint32_t)((double)f1 / (double)sample_rate * 4294967296.0);
    o->inc2 = (f2 > 0.0f)
                  ? (uint32_t)((double)f2 / (double)sample_rate * 4294967296.0)
                  : 0;
    /* Scale so the sum of two tones never clips (Q15). */
    float scale_f = amp * 32768.0f * ((f2 > 0.0f) ? 0.5f : 1.0f);
    o->scale = (int32_t)lroundf(scale_f);
}

void tone_osc_fill(tone_osc_t *o, int16_t *out, size_t n)
{
    const bool two = o->inc2 != 0;

    for (size_t i = 0; i < n; i++) {
        int32_t s = sine_lookup(o->phase1);
        if (two) s += sine_lookup(o->phase2);
        out[i] = (int16_t)((s * o->scale) >> 15);

        o->phase1 += o->inc1;
        if (two) o->phase2 += o->inc2;
    }
}

/* cadence period and "on" window, in samples, for the current position. */
static bool cadence_on(call_progress_t kind, uint32_t sr, uint32_t pos)
{
    switch (kind) {
        case TONE_DIAL:
            return true;
        case TONE_RINGBACK: {
            uint32_t period = 6 * sr;            /* 2 on + 4 off */
            return (pos % period) < (2 * sr);
        }
        case TONE_BUSY: {
            uint32_t period = sr;                /* 0.5 on + 0.5 off */
            return (pos % period) < (sr / 2);
        }
        case TONE_REORDER: {
            uint32_t period = sr / 2;            /* 0.25 on + 0.25 off */
            return (pos % period) < (sr / 4);
        }
        case TONE_CONFIRM: {
            /* three 100 ms beeps, 100 ms gaps, then silence */
            uint32_t beat = sr / 10;             /* 100 ms */
            uint32_t idx = pos / beat;
            if (idx >= 6) return false;          /* done after 600 ms */
            return (idx % 2) == 0;
        }
        default:
            return false;
    }
}

void progress_tone_start(progress_tone_t *p, uint32_t sample_rate, call_progress_t kind)
{
    p->kind = kind;
    p->sample_rate = sample_rate;
    p->pos = 0;
    float f1 = 0, f2 = 0;
    switch (kind) {
        case TONE_DIAL:     f1 = 350; f2 = 450; break;
        case TONE_RINGBACK: f1 = 440; f2 = 480; break;
        case TONE_BUSY:     f1 = 480; f2 = 620; break;
        case TONE_REORDER:  f1 = 480; f2 = 620; break;
        case TONE_CONFIRM:  f1 = 350; f2 = 440; break;
        default:            f1 = 0;   f2 = 0;   break;
    }
    tone_osc_init(&p->osc, sample_rate, f1, f2, 0.6f);
}

void progress_tone_fill(progress_tone_t *p, int16_t *out, size_t n)
{
    if (p->kind == TONE_NONE) {
        memset(out, 0, n * sizeof(int16_t));
        return;
    }
    /* Generate sample-by-sample so on/off transitions land mid-buffer. */
    for (size_t i = 0; i < n; i++) {
        bool on = cadence_on(p->kind, p->sample_rate, p->pos);
        int16_t s = 0;
        tone_osc_fill(&p->osc, &s, 1);  /* advance phase even when muted */
        out[i] = on ? s : 0;
        p->pos++;
    }
}

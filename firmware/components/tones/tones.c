#include "tones.h"
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define TWO_PI (2.0f * (float)M_PI)

void tone_osc_init(tone_osc_t *o, uint32_t sample_rate, float f1, float f2, float amp)
{
    o->f1 = f1;
    o->f2 = f2;
    o->phase1 = 0.0f;
    o->phase2 = 0.0f;
    o->amp = amp;
    o->sample_rate = sample_rate;
}

void tone_osc_fill(tone_osc_t *o, int16_t *out, size_t n)
{
    const float dp1 = TWO_PI * o->f1 / (float)o->sample_rate;
    const float dp2 = TWO_PI * o->f2 / (float)o->sample_rate;
    const int   two = (o->f2 > 0.0f) ? 1 : 0;
    /* Scale so the sum of two tones never clips. */
    const float scale = o->amp * 32767.0f * (two ? 0.5f : 1.0f);

    for (size_t i = 0; i < n; i++) {
        float s = sinf(o->phase1);
        if (two) s += sinf(o->phase2);
        out[i] = (int16_t)(s * scale);

        o->phase1 += dp1;
        if (o->phase1 >= TWO_PI) o->phase1 -= TWO_PI;
        if (two) {
            o->phase2 += dp2;
            if (o->phase2 >= TWO_PI) o->phase2 -= TWO_PI;
        }
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

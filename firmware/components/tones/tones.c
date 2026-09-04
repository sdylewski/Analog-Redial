#include "tones.h"
#include "tone_regions.h"
#include "board.h"
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

void tone_osc_init_multi(tone_osc_t *o, uint32_t sample_rate,
                         const float *freqs, uint8_t n, float amp)
{
    ensure_sine_table();
    if (n < 1) n = 1;
    if (n > TONE_OSC_MAX_F) n = TONE_OSC_MAX_F;

    o->amp = amp;
    o->sample_rate = sample_rate;

    /* One-time float/double math here is fine -- it's the per-sample loop
     * in tone_osc_fill() that can't afford soft-float, not tone start. */
    uint8_t active = 0;
    for (uint8_t i = 0; i < n; i++) {
        float fi = freqs[i];
        o->f[i] = fi;
        o->phase[i] = 0;
        o->inc[i] = (fi > 0.0f)
                        ? (uint32_t)((double)fi / (double)sample_rate * 4294967296.0)
                        : 0;
        if (o->inc[i]) active++;
    }
    for (uint8_t i = n; i < TONE_OSC_MAX_F; i++) {
        o->f[i] = 0; o->phase[i] = 0; o->inc[i] = 0;
    }
    o->n_f = n;

    /* Scale so the summed tones never clip (Q15): divide by however many are
     * actually oscillating, not by `n` -- a caller passing f2 == 0 for a
     * single tone would otherwise get a needlessly halved amplitude. */
    if (active < 1) active = 1;
    float scale_f = amp * 32768.0f / (float)active;
    o->scale = (int32_t)lroundf(scale_f);
}

void tone_osc_init(tone_osc_t *o, uint32_t sample_rate, float f1, float f2, float amp)
{
    const float freqs[2] = { f1, f2 };
    tone_osc_init_multi(o, sample_rate, freqs, 2, amp);
}

void tone_osc_fill(tone_osc_t *o, int16_t *out, size_t n)
{
    const uint8_t nf = o->n_f;

    for (size_t i = 0; i < n; i++) {
        int32_t s = 0;
        for (uint8_t k = 0; k < nf; k++) {
            if (!o->inc[k]) continue;
            s += sine_lookup(o->phase[k]);
            o->phase[k] += o->inc[k];
        }
        out[i] = (int16_t)((s * o->scale) >> 15);
    }
}

static const tone_region_t *lookup_region(void)
{
    char lang[8];
    board_language_get(lang, sizeof(lang));
    for (size_t i = 0; i < TONE_REGIONS_N; i++) {
        if (strcmp(TONE_REGIONS[i].lang, lang) == 0) return &TONE_REGIONS[i];
    }
    return &TONE_REGION_FALLBACK;
}

/* Resolves a cadence's phase lengths from ms to samples once, at
 * progress_tone_start() time, rather than converting on every sample in
 * the hot fill loop below. */
static void resolve_cadence(progress_tone_t *p, const tone_cadence_t *c)
{
    p->n_phases = c->n_phases;
    for (int i = 0; i < c->n_phases; i++) {
        p->phase_samples[i] = (uint32_t)((uint64_t)c->phase_ms[i] * p->sample_rate / 1000);
    }
}

/* True if `pos` (samples since cadence start) falls in an "on" phase.
 * n_phases == 0 means continuous. Phase 0, 2, 4, ... are "on". */
static bool cadence_on(const progress_tone_t *p, uint32_t pos)
{
    if (p->n_phases == 0) return true;
    uint32_t period = 0;
    for (int i = 0; i < p->n_phases; i++) period += p->phase_samples[i];
    if (period == 0) return true; /* degenerate table entry -- fail open, not silent */
    uint32_t rem = pos % period;
    for (int i = 0; i < p->n_phases; i++) {
        if (rem < p->phase_samples[i]) return (i % 2) == 0;
        rem -= p->phase_samples[i];
    }
    return false; /* unreachable */
}

/* REORDER/CONFIRM aren't in the per-language region table (see tones.h) --
 * fixed specs, same shape as a tone_spec_t so they can share the same
 * resolve_cadence()/cadence_on() path. */
static const tone_spec_t TONE_SPEC_REORDER = { 480, 620, { {250, 250}, 2 } };

/* Off-hook (ROH) alert -- 4 frequencies, so it doesn't fit tone_spec_t's
 * f1/f2 pair; the cadence still does. See TONE_OFFHOOK_ALERT in tones.h. */
static const float TONE_ROH_FREQS[4] = { 1400.0f, 2060.0f, 2450.0f, 2600.0f };
static const tone_cadence_t TONE_ROH_CADENCE = { {100, 100}, 2 };

void progress_tone_start(progress_tone_t *p, uint32_t sample_rate, call_progress_t kind)
{
    p->kind = kind;
    p->sample_rate = sample_rate;
    p->pos = 0;
    p->n_phases = 0;

    float f1 = 0, f2 = 0;
    switch (kind) {
        case TONE_DIAL: {
            const tone_spec_t *s = &lookup_region()->dial;
            f1 = s->f1; f2 = s->f2;
            resolve_cadence(p, &s->cadence);
            break;
        }
        case TONE_RINGBACK: {
            const tone_spec_t *s = &lookup_region()->ringback;
            f1 = s->f1; f2 = s->f2;
            resolve_cadence(p, &s->cadence);
            break;
        }
        case TONE_BUSY: {
            const tone_spec_t *s = &lookup_region()->busy;
            f1 = s->f1; f2 = s->f2;
            resolve_cadence(p, &s->cadence);
            break;
        }
        case TONE_REORDER:
            f1 = TONE_SPEC_REORDER.f1; f2 = TONE_SPEC_REORDER.f2;
            resolve_cadence(p, &TONE_SPEC_REORDER.cadence);
            break;
        case TONE_CONFIRM:
            f1 = 350; f2 = 440; /* handled as a one-shot special case below, not a cadence */
            break;
        case TONE_OFFHOOK_ALERT:
            /* Only 4-frequency tone here, and the only one at full amplitude
             * (see tones.h) -- init'd directly and returned early rather than
             * squeezed through the f1/f2 path below. */
            resolve_cadence(p, &TONE_ROH_CADENCE);
            tone_osc_init_multi(&p->osc, sample_rate, TONE_ROH_FREQS, 4, 1.0f);
            return;
        case TONE_RECORD_WARN_1:
        case TONE_RECORD_WARN_2:
        case TONE_RECORD_WARN_3:
            /* Continuous, like TONE_ANSWERING_BEEP below -- telephony.c
             * itself times the brief on-duration (RECORD_WARN_BEEP_MS) and
             * silences the line again afterward, so there's no cadence to
             * bake in here; each of these three is just "some 900 Hz for
             * as long as the caller asks for." */
            f1 = 900; f2 = 0;
            break;
        case TONE_ANSWERING_BEEP:
            /* Continuous for its whole duration (n_phases stays 0, i.e.
             * "always on") -- telephony.c's play_transient() times the
             * ~600 ms via its `ms` argument, same as CONFIRM. */
            f1 = 1000; f2 = 0;
            break;
        default:
            f1 = 0; f2 = 0;
            break;
    }
    float amp = 0.6f;
    if (kind == TONE_RECORD_WARN_1) amp = 0.10f;
    else if (kind == TONE_RECORD_WARN_2) amp = 0.13f;
    else if (kind == TONE_RECORD_WARN_3) amp = 0.16f;
    else if (kind == TONE_ANSWERING_BEEP) amp = 0.8f; /* clearly audible "go" cue */
    tone_osc_init(&p->osc, sample_rate, f1, f2, amp);
}

void progress_tone_fill(progress_tone_t *p, int16_t *out, size_t n)
{
    if (p->kind == TONE_NONE) {
        memset(out, 0, n * sizeof(int16_t));
        return;
    }
    /* Generate sample-by-sample so on/off transitions land mid-buffer. */
    for (size_t i = 0; i < n; i++) {
        bool on;
        if (p->kind == TONE_CONFIRM) {
            /* three 100 ms beeps, 100 ms gaps, then silence -- a one-shot
             * sequence, not a repeating cadence, so it can't use the
             * generic cadence_on() (which always loops via modulo). */
            uint32_t beat = p->sample_rate / 10;
            uint32_t idx = p->pos / beat;
            on = (idx < 6) && (idx % 2) == 0;
        } else {
            on = cadence_on(p, p->pos);
        }
        int16_t s = 0;
        tone_osc_fill(&p->osc, &s, 1);  /* advance phase even when muted */
        out[i] = on ? s : 0;
        p->pos++;
    }
}

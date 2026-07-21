/*
 * tones.h — dual-tone synthesis and North American call-progress tones.
 *
 * Pure DSP: fills mono int16 PCM buffers. No hardware dependency. The `audio`
 * component pulls samples from a tone generator and routes them to a codec
 * channel (= phone line).
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A dual-tone oscillator with phase continuity across buffer fills.
 * Phase-accumulator/sine-table internally (see tones.c) -- no float math in
 * the per-sample path, only at tone_osc_init() (one-time, not hot-loop). */
typedef struct {
    float    f1, f2;        /* Hz (f2 may be 0 for a single tone) */
    float    amp;           /* 0..1 of full scale */
    uint32_t sample_rate;
    uint32_t phase1, phase2; /* Q32 phase accumulators */
    uint32_t inc1, inc2;     /* Q32 phase increment per sample (inc2 == 0 => single tone) */
    int32_t  scale;          /* Q15 amplitude factor, already folds in the two-tone half-scale */
} tone_osc_t;

void tone_osc_init(tone_osc_t *o, uint32_t sample_rate, float f1, float f2, float amp);

/* Fill `n` mono samples. Phase is preserved for the next call. */
void tone_osc_fill(tone_osc_t *o, int16_t *out, size_t n);

/* ---- Call-progress tone generator with cadence (on/off gating) ---- */
typedef enum {
    TONE_NONE = 0,
    TONE_DIAL,       /* 350+450 Hz continuous */
    TONE_RINGBACK,   /* 440+480 Hz, 2 s on / 4 s off */
    TONE_BUSY,       /* 480+620 Hz, 0.5 s on / 0.5 s off */
    TONE_REORDER,    /* 480+620 Hz, 0.25 s on / 0.25 s off (fast busy) */
    TONE_CONFIRM,    /* 350+440 Hz triple beep (stutter/confirmation) */
} call_progress_t;

typedef struct {
    call_progress_t kind;
    tone_osc_t      osc;
    uint32_t        sample_rate;
    uint32_t        pos;     /* samples since cadence start */
} progress_tone_t;

void progress_tone_start(progress_tone_t *p, uint32_t sample_rate, call_progress_t kind);

/* Fill `n` mono samples honoring the cadence (silence during off phase). */
void progress_tone_fill(progress_tone_t *p, int16_t *out, size_t n);

#ifdef __cplusplus
}
#endif

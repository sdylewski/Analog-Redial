/*
 * voice_fx.h -- in-call voice effects, applied per line by audio.c.
 *
 * All fixed-point (the C6 has no FPU). Only the pitch effects (chipmunk,
 * Vader) add delay: about half their 30 ms window, ~15 ms. The rest are
 * sample-by-sample.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    VOICE_FX_NONE = 0,
    VOICE_FX_DALEK,      /* 30 Hz ring modulation */
    VOICE_FX_CHIPMUNK,   /* pitch up 1.5x */
    VOICE_FX_VADER,      /* pitch down 0.8x, overdrive, short "mask" resonance */
    VOICE_FX_8BIT,       /* bit-crush + 4 kHz sample-and-hold */
    VOICE_FX_ROBOT,      /* metallic comb resonance at ~110 Hz, no delay */
    VOICE_FX_TREMOLO,    /* 6 Hz amplitude wobble, no delay */
    VOICE_FX_DISTORTION, /* heavy soft-clipped overdrive, no delay */
} voice_fx_t;

typedef struct voice_fx_state voice_fx_state_t;

/* One state per audio stream (each keeps its own delay line). NULL on OOM. */
voice_fx_state_t *voice_fx_create(uint32_t sample_rate);

/* What one stream has done since the last voice_fx_take_stats(): levels going
 * in and coming out of the effect, and how often the peak limiter bent a
 * sample. For diagnosing "too loud" / "distorted" without a scope. */
typedef struct {
    voice_fx_t fx;               /* effect last applied (NONE if idle) */
    uint32_t   samples;          /* processed since the last take */
    int32_t    in_peak, out_peak;   /* 0..32767 */
    uint32_t   in_rms, out_rms;
    uint32_t   limited;          /* samples the limiter reduced */
} voice_fx_stats_t;
void voice_fx_take_stats(voice_fx_state_t *s, voice_fx_stats_t *out);

/* In place. Switching effect resets the state, so there's no stale tail. */
void voice_fx_process(voice_fx_state_t *s, voice_fx_t fx, int16_t *buf, size_t n);

const char *voice_fx_name(voice_fx_t fx);

#ifdef __cplusplus
}
#endif

/*
 * voice_fx.h -- in-call voice effects, applied per line by audio.c.
 *
 * Fixed-point (the C6 has no FPU), run at 8 kHz (audio.c resamples the
 * 48 kHz streams around them). Chipmunk (a correlation-spliced delay line)
 * adds 5-40 ms; robot (FFT with a fixed phase) adds about 34 ms; Dalek and
 * tremolo are sample-by-sample.
 */
#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The number is the key pressed during a call (3, 4 and 7 were Vader, 8-bit
 * and distortion, since removed; they stay unused so the others keep their
 * keys). */
typedef enum {
    VOICE_FX_NONE     = 0,
    VOICE_FX_DALEK    = 1,   /* ring modulation, 30 Hz by default */
    VOICE_FX_CHIPMUNK = 2,   /* pitch shift, 1.3x by default (below 1x: a deep voice) */
    VOICE_FX_ROBOT    = 5,   /* monotone buzz, 125 Hz by default: STFT with a fixed phase */
    VOICE_FX_TREMOLO  = 6,   /* amplitude wobble, 6 Hz by default */
} voice_fx_t;
#define VOICE_FX_MAX 6

/* True for VOICE_FX_NONE and every effect above (not the unused numbers). */
bool voice_fx_valid(int fx);

typedef struct voice_fx_state voice_fx_state_t;

/* One state per audio stream (each keeps its own delay line). NULL on OOM. */
voice_fx_state_t *voice_fx_create(uint32_t sample_rate);

void voice_fx_destroy(voice_fx_state_t *s);   /* NULL ok */

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

/* Settings, one slider each in Settings -> Phone -> Voice effects (saved by
 * board.c as an array in this order: only ever append). Take effect mid-call;
 * out-of-range values are clamped. On a VoIP call only the board where the
 * key was pressed changes the voices, so its settings are the ones heard. */
typedef enum {
    VFX_CHIP_PCT = 0,    /* chipmunk pitch, % of the voice */
    VFX_ROBOT_MIX,       /* % robot, the rest the plain voice */
    VFX_ROBOT_HZ,        /* robot buzz pitch */
    VFX_DALEK_HZ,        /* Dalek ring-modulator frequency */
    VFX_DALEK_MIX,       /* % Dalek, the rest the plain voice */
    VFX_TREM_HZ,         /* tremolo speed */
    VFX_TREM_DEPTH,      /* tremolo depth, % */
    VFX_NPARAMS
} voice_fx_param_t;

typedef struct {
    voice_fx_t  fx;              /* the effect it belongs to */
    const char *key;             /* form field / i18n key suffix */
    const char *label, *unit;
    int16_t     min, max, step, def;
} voice_fx_param_info_t;

extern const voice_fx_param_info_t VOICE_FX_PARAMS[VFX_NPARAMS];
void voice_fx_set_param(voice_fx_param_t p, int v);
int  voice_fx_param(voice_fx_param_t p);

/* Output gain applied after every effect (before the limiter), percent of the
 * level-matched default, 10-300. Runtime only (resets to 100 on boot): for
 * testing how much headroom the effects have. */
void voice_fx_set_gain_pct(int pct);
int voice_fx_gain_pct(void);

#ifdef __cplusplus
}
#endif

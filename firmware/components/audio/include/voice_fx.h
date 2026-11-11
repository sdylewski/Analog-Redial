/*
 * voice_fx.h -- in-call voice effects, applied per line by audio.c.
 *
 * Fixed-point (the C6 has no FPU), run at 8 kHz (audio.c resamples the
 * 48 kHz streams around them). Chipmunk and Vader (a correlation-spliced
 * delay line) add 5-40 ms; robot (FFT with a fixed phase) adds about 34 ms.
 * The rest are sample-by-sample.
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
    VOICE_FX_CHIPMUNK,   /* pitch up, 1.3x by default (voice_fx_set_tuning) */
    VOICE_FX_VADER,      /* pitch down 0.8x, overdrive, short "mask" resonance */
    VOICE_FX_8BIT,       /* bit-crush + 4 kHz sample-and-hold */
    VOICE_FX_ROBOT,      /* monotone buzz, 125 Hz by default: STFT with a fixed phase */
    VOICE_FX_TREMOLO,    /* 6 Hz amplitude wobble, no delay */
    VOICE_FX_DISTORTION, /* heavy soft-clipped overdrive, no delay */
} voice_fx_t;

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

/* Adjustable from Settings -> Phone -> Voice effects (stored by board.c, set
 * here at boot and on save; takes effect mid-call). Out-of-range values are
 * clamped. On a VoIP call only the board where the key was pressed changes
 * the voices, so its settings are the ones heard. */
#define VOICE_FX_CHIPMUNK_PCT_DEFAULT 130   /* pitch, % of the original voice */
#define VOICE_FX_CHIPMUNK_PCT_MIN     105
#define VOICE_FX_CHIPMUNK_PCT_MAX     200
#define VOICE_FX_ROBOT_MIX_DEFAULT    100   /* % robot; the rest is the plain voice */
#define VOICE_FX_ROBOT_HZ_DEFAULT     125   /* buzz pitch */
#define VOICE_FX_ROBOT_HZ_MIN         70
#define VOICE_FX_ROBOT_HZ_MAX         200
void voice_fx_set_tuning(int chipmunk_pct, int robot_mix_pct, int robot_hz);
int voice_fx_chipmunk_pct(void);
int voice_fx_robot_mix(void);
int voice_fx_robot_hz(void);

/* Output gain applied after every effect (before the limiter), percent of the
 * level-matched default, 10-300. Runtime only (resets to 100 on boot): for
 * testing how much headroom the effects have. */
void voice_fx_set_gain_pct(int pct);
int voice_fx_gain_pct(void);

#ifdef __cplusplus
}
#endif

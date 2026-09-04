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

/* A multi-tone oscillator (up to TONE_OSC_MAX_F simultaneous frequencies)
 * with phase continuity across buffer fills. Phase-accumulator/sine-table
 * internally (see tones.c) -- no float math in the per-sample path, only at
 * tone_osc_init() (one-time, not hot-loop).
 *
 * Two frequencies covers every call-progress tone except the off-hook alert
 * (TONE_OFFHOOK_ALERT), which is a 4-frequency superposition per Telcordia
 * GR-506-CORE -- hence the arrays rather than the f1/f2 pair this started as. */
#define TONE_OSC_MAX_F 4
typedef struct {
    float    f[TONE_OSC_MAX_F];  /* Hz; entries past n_f are unused */
    float    amp;                /* 0..1 of full scale */
    uint32_t sample_rate;
    uint32_t phase[TONE_OSC_MAX_F]; /* Q32 phase accumulators */
    uint32_t inc[TONE_OSC_MAX_F];   /* Q32 phase increment per sample */
    uint8_t  n_f;                /* active frequency count (1..TONE_OSC_MAX_F) */
    int32_t  scale;              /* Q15 amplitude, already folds in the 1/n_f headroom */
} tone_osc_t;

/* Convenience wrapper for the common 1- or 2-tone case (f2 == 0 => single). */
void tone_osc_init(tone_osc_t *o, uint32_t sample_rate, float f1, float f2, float amp);

/* General form: `n` frequencies from `freqs` (1..TONE_OSC_MAX_F). */
void tone_osc_init_multi(tone_osc_t *o, uint32_t sample_rate,
                         const float *freqs, uint8_t n, float amp);

/* Fill `n` mono samples. Phase is preserved for the next call. */
void tone_osc_fill(tone_osc_t *o, int16_t *out, size_t n);

/* ---- Call-progress tone generator with cadence (on/off gating) ----
 * DIAL/RINGBACK/BUSY frequency + cadence come from a per-language region
 * table (see tone_regions.h) so e.g. Italy gets its own dial tone instead
 * of the North American default -- REORDER/CONFIRM stay fixed (not
 * PSTN call-progress tones a caller in another country would expect to
 * differ, just internal feedback beeps). */
typedef enum {
    TONE_NONE = 0,
    TONE_DIAL,       /* region-dependent, see tone_regions.h (NA default: 350+450 Hz continuous) */
    TONE_RINGBACK,   /* region-dependent (NA default: 440+480 Hz, 2 s on / 4 s off) */
    TONE_BUSY,       /* region-dependent (NA default: 480+620 Hz, 0.5 s on / 0.5 s off) */
    TONE_REORDER,    /* 480+620 Hz, 0.25 s on / 0.25 s off (fast busy) -- not regionalized */
    TONE_CONFIRM,    /* 350+440 Hz triple beep (stutter/confirmation) -- not regionalized */
    /* "Receiver off-hook" (ROH) / howler: 1400+2060+2450+2600 Hz, 100 ms on /
     * 100 ms off, per Telcordia GR-506-CORE SS17.2.8 (North American). Played
     * at full amplitude, unlike the 0.6 the others use -- the entire point is
     * to be heard across a room from a handset lying on a table, so it is
     * deliberately loud and dissonant. Not regionalized (other countries use
     * quite different alerts; this is the one this project implements). */
    TONE_OFFHOOK_ALERT,
    /* 211's "time's almost up" cues -- three quiet, brief 900 Hz beeps
     * played individually *during* the recording (see telephony.c's
     * rec_warn_at_us[]/start_recording()), at 10s/5s/1s before the
     * recording's own max-duration cutoff, not stacked back-to-back
     * afterward -- that read as an abrupt alarm, not a gentle reminder
     * (confirmed live). Deliberately quiet throughout (0.10/0.13/0.16 --
     * see tones.c) rather than escalating to full volume; the point is a
     * light nudge repeated three times, not urgency. Not PSTN
     * call-progress tones, so not regionalized -- same reasoning as
     * REORDER/CONFIRM. */
    TONE_RECORD_WARN_1,  /* ~10s left */
    TONE_RECORD_WARN_2,  /* ~5s left */
    TONE_RECORD_WARN_3,  /* ~1s left */
    /* 211's "leave your message after the beep" cue -- a single clean
     * 1000 Hz tone, ~600 ms, matching the classic answering-machine beep
     * (not TONE_CONFIRM's 350+440 Hz triple stutter, which reads as a
     * "confirmation chime" -- confirmed live: a caller who'd specifically
     * asked for "a beep like an answering machine" said the stutter tone
     * "wasn't like an answering machine, very different"). Not
     * regionalized -- same reasoning as REORDER/CONFIRM. */
    TONE_ANSWERING_BEEP,
} call_progress_t;

/* A cadence is an alternating on/off/on/off/... sequence of phase lengths
 * (ms) that repeats; n_phases == 0 means continuous (always on, e.g. most
 * dial tones). Phase 0 (and every even index) is "on". */
#define TONE_CADENCE_MAX_PHASES 6
typedef struct {
    uint16_t phase_ms[TONE_CADENCE_MAX_PHASES];
    uint8_t  n_phases;
} tone_cadence_t;

typedef struct {
    float          f1, f2; /* Hz; f2 == 0 for a single tone */
    tone_cadence_t cadence;
} tone_spec_t;

typedef struct {
    call_progress_t kind;
    tone_osc_t      osc;
    uint32_t        sample_rate;
    uint32_t        pos;     /* samples since cadence start */
    uint32_t        phase_samples[TONE_CADENCE_MAX_PHASES]; /* cadence, in samples, resolved at start */
    uint8_t         n_phases;
} progress_tone_t;

void progress_tone_start(progress_tone_t *p, uint32_t sample_rate, call_progress_t kind);

/* Fill `n` mono samples honoring the cadence (silence during off phase). */
void progress_tone_fill(progress_tone_t *p, int16_t *out, size_t n);

#ifdef __cplusplus
}
#endif

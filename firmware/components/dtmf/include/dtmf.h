/*
 * dtmf.h — DTMF (touch-tone) generation and Goertzel-based detection.
 *
 * Generation: produce the row+col dual tone for a key.
 * Detection : feed blocks of mono int16 PCM; get decoded digits with debounce.
 *
 * Standard frequencies:
 *   rows: 697 770 852 941   cols: 1209 1336 1477 1633
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "tones.h"

#ifdef __cplusplus
extern "C" {
#endif

/* '0'-'9','*','#','A'-'D' supported. Returns false if key invalid. */
bool dtmf_key_freqs(char key, float *row_hz, float *col_hz);

/* Initialize an oscillator to emit `key`. */
bool dtmf_make_osc(tone_osc_t *o, uint32_t sample_rate, char key, float amp);

/* ---- Detector ---- */

/* Four Goertzel windows run staggered by a quarter of their length, so a
 * decision comes every ~6.4 ms instead of every ~25.6 ms. That is what lets
 * the detector follow a fast tone dialer (tones and pauses of 40-50 ms). */
#define DTMF_BANKS 4

/* The limits classify() applies, which a phone that sends out-of-spec tones can
 * have loosened (the touch-tone test, dtmf_test.h). All compare window powers. */
typedef struct {
    uint8_t col_over_row_x10; /* how far the column tone may beat the row tone, x10 */
    uint8_t row_over_col_x10; /* ...and the other way round */
    uint8_t row_other_pct;    /* another row bin, as % of the strongest */
    uint8_t row01_pct;        /* the same, between 697 and 770 Hz: see classify() */
    uint8_t col_other_pct;    /* another column bin, as % of the strongest */
    uint8_t conc_pct;         /* share of the window's energy in the two bins */
} dtmf_tuning_t;

#define DTMF_TUNING_DEFAULT ((dtmf_tuning_t){ .col_over_row_x10 = 30, .row_over_col_x10 = 30, \
    .row_other_pct = 25, .row01_pct = 80, .col_other_pct = 25, .conc_pct = 50 })

/* What the touch-tone test sees of each finished window. */
typedef struct {
    const int64_t *mags;  /* 8 bin powers: rows, then columns */
    int64_t energy;
    int32_t peak;         /* before the AGC */
    size_t  block_n;
    int     bank;
} dtmf_window_t;
typedef void (*dtmf_window_cb_t)(void *ctx, const dtmf_window_t *w);
typedef void (*dtmf_sample_cb_t)(void *ctx, int16_t raw);

typedef struct {
    uint32_t sample_rate;
    uint32_t dec;            /* input samples averaged into one analysis sample (6 at 48 kHz -> 8 kHz) */
    uint32_t dec_n;
    int32_t  dec_acc;
    size_t   block_n;        /* analysis samples per window (205 = 25.6 ms at 8 kHz) */
    int64_t  min_mag;        /* energy gate, scaled to the window length */
    int32_t  coeff[8];       /* Q14 fixed-point: round(2*cos(w) * 16384) */
    int32_t  q1[DTMF_BANKS][8], q2[DTMF_BANKS][8];
    size_t   filled[DTMF_BANKS];
    int32_t  peak[DTMF_BANKS];  /* peak |sample| in each window, for the AGC */
    int64_t  energy[DTMF_BANKS]; /* sum of squares of the (gain-scaled) samples in each window */
    /* debounce -- see dtmf_detector_push()'s comment. All counts are in
     * windows (~6.4 ms each). */
    char     last_emitted;
    int      gap_run;        /* consecutive windows with no tone, up to now */
    int      gap_before;     /* ...as it stood when the current press began */
    char     candidate;
    int      candidate_n;    /* consecutive windows that classified as `candidate` */
    int      silence_n;      /* consecutive windows that classified as nothing */
    bool     in_tone;
    char     press_digit;    /* digit of the *current* press, START to END */
    int64_t  press_start_us; /* for start/end timing logs, see dtmf.c */
    /* AGC (automatic gain control) -- normalizes real signal content
     * toward a consistent level before Goertzel analysis, so one set of
     * amplitude-based thresholds works across different phones' natural
     * DTMF loudness instead of needing per-phone tuning. */
    int32_t  agc_gain_q8;    /* current gain, Q8 (256 = 1.0x) */
    dtmf_tuning_t tuning;
    dtmf_window_cb_t window_cb;
    dtmf_sample_cb_t sample_cb;
    void    *capture_ctx;
} dtmf_detector_t;

void dtmf_detector_init(dtmf_detector_t *d, uint32_t sample_rate);

/*
 * Push mono PCM. Whenever a *new* stable digit is detected, returns it once
 * (0 otherwise). Call repeatedly with consecutive audio.
 */
char dtmf_detector_push(dtmf_detector_t *d, const int16_t *samples, size_t n);

/* Starts on DTMF_TUNING_DEFAULT. Safe to call while audio is running. */
void dtmf_detector_set_tuning(dtmf_detector_t *d, const dtmf_tuning_t *t);

/* Taps the detector's windows and its 8 kHz samples from inside
 * dtmf_detector_push(), for the touch-tone test. NULL callbacks stop it; the
 * last call may still be running for one audio frame after that. */
void dtmf_detector_set_capture(dtmf_detector_t *d, dtmf_window_cb_t window_cb,
                               dtmf_sample_cb_t sample_cb, void *ctx);

#ifdef __cplusplus
}
#endif

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
    int      since_emit;     /* windows since the last emission */
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
} dtmf_detector_t;

void dtmf_detector_init(dtmf_detector_t *d, uint32_t sample_rate);

/*
 * Push mono PCM. Whenever a *new* stable digit is detected, returns it once
 * (0 otherwise). Call repeatedly with consecutive audio.
 */
char dtmf_detector_push(dtmf_detector_t *d, const int16_t *samples, size_t n);

#ifdef __cplusplus
}
#endif

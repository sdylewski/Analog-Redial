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
typedef struct {
    uint32_t sample_rate;
    size_t   block_n;          /* samples per Goertzel block (e.g. fs/100 = 10 ms) */
    size_t   filled;
    int32_t  coeff[8];  /* Q14 fixed-point: round(2*cos(w) * 16384) */
    int32_t  q1[8], q2[8];
    /* debounce: a digit must persist N consecutive blocks, then a gap. */
    char     last_emitted;
    char     candidate;
    int      candidate_blocks;
    bool     in_tone;
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

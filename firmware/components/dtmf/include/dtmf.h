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
    /* debounce -- see dtmf_detector_push()'s comment for what each of these
     * actually gates: tone-active tracking (in_tone/silence_blocks, short)
     * vs. same-digit re-emission lockout (last_emitted/since_emit_blocks,
     * longer -- rejects contact bounce / keypad auto-repeat that looks like
     * a real release + re-press to the block-level detector). */
    char     last_emitted;
    int      since_emit_blocks;
    char     candidate;
    int      candidate_blocks;
    int      silence_blocks;
    bool     in_tone;
    char     press_digit;    /* digit of the *current* press, START to END */
    int64_t  press_start_us; /* for start/end timing logs, see dtmf.c */
    /* AGC (automatic gain control) -- normalizes real signal content
     * toward a consistent level before Goertzel analysis, so one set of
     * amplitude-based thresholds works across different phones' natural
     * DTMF loudness instead of needing per-phone tuning. See
     * dtmf_detector_push()'s comment for the gating rationale. */
    int32_t  agc_gain_q8;    /* current gain, Q8 (256 = 1.0x) */
    int32_t  agc_block_peak; /* running peak |raw sample| for the block in progress */
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

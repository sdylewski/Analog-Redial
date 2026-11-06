/*
 * coin_tone.h -- detects the coin tones a payphone's totalizer sends down the
 * line: 1700 Hz and 2200 Hz together, one beep for a nickel, two for a dime,
 * five shorter ones for a quarter. No ESP-IDF in here (firmware/test/host runs
 * it against synthetic tones); telephony.c feeds it each line's audio.
 *
 * Timing, as the Bell System specified it: nickel one 66 ms beep, dime two 66 ms
 * beeps 66 ms apart, quarter five 33 ms beeps 33 ms apart. The detector only
 * needs a beep and a gap to last 20 ms or more, and a coin to end with 150 ms
 * of quiet, so real totalizers that drift from that still count.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define COIN_TONE_RATE   8000   /* analysis rate, after averaging the input down */
#define COIN_TONE_WIN    80     /* 10 ms windows: 1700 and 2200 Hz are exact bins */

typedef struct {
    uint32_t dec;            /* input samples averaged into one analysis sample */
    uint32_t dec_n;
    int32_t  dec_acc;
    int32_t  q1[2], q2[2];   /* Goertzel state, 1700 and 2200 Hz */
    int64_t  energy;
    uint32_t filled;
    bool     in_beep;
    uint8_t  on_run, off_run;
    uint8_t  beeps;          /* beeps in the coin being counted */
    uint16_t quiet;          /* windows since the last beep ended */
    uint16_t level;          /* the last tone window's level, 0-32767, for display */
} coin_tone_det_t;

void coin_tone_init(coin_tone_det_t *d, uint32_t sample_rate);

/* Push mono PCM at the rate given to coin_tone_init(). Returns 0 most of the
 * time; once a coin's beeps have ended, its value in cents (5, 10, 25), or
 * minus the beep count for a burst that isn't a known coin. */
int coin_tone_push(coin_tone_det_t *d, const int16_t *samples, size_t n);

/* What a finished burst of `beeps` is worth: 5, 10, 25, or 0 if none. */
int coin_tone_cents(int beeps);

#ifdef __cplusplus
}
#endif

#include "coin_tone.h"
#include <string.h>

/* round(2 * cos(2 pi f / 8000) * 16384), Q14 */
static const int32_t COEFF[2] = {
    7650,    /* 1700 Hz */
    -5126,   /* 2200 Hz */
};

/* A window has the tone when each frequency holds at least this share of
 * what a clean pair of equal tones would give (x100). Lower lets in a weaker
 * or more distorted tone; higher keeps out speech and dial tone. */
#define SHARE_MIN_PCT    25
/* Quietest tone counted: about -40 dBFS. */
#define LEVEL_MIN        300
#define ON_WINDOWS       2    /* 20 ms of tone makes a beep */
#define OFF_WINDOWS      2    /* 20 ms without ends it */
#define END_WINDOWS      15   /* 150 ms of quiet ends the coin */

void coin_tone_init(coin_tone_det_t *d, uint32_t sample_rate)
{
    memset(d, 0, sizeof(*d));
    d->dec = sample_rate > COIN_TONE_RATE ? sample_rate / COIN_TONE_RATE : 1;
}

int coin_tone_cents(int beeps)
{
    switch (beeps) {
        case 1: return 5;
        case 2: return 10;
        case 4: case 5: case 6: return 25;   /* a quarter with a beep lost or split */
        default: return 0;
    }
}

static int64_t power(const coin_tone_det_t *d, int k)
{
    int64_t q1 = d->q1[k], q2 = d->q2[k];
    return q1 * q1 + q2 * q2 - ((COEFF[k] * q1) >> 14) * q2;
}

/* One finished window: is the tone there? */
static bool window_has_tone(coin_tone_det_t *d)
{
    int64_t e = d->energy;
    /* E = N * A^2 for two tones of amplitude A each. */
    if (e < (int64_t)COIN_TONE_WIN * LEVEL_MIN * LEVEL_MIN) return false;
    /* Each bin of a clean pair: (A N / 2)^2 = E * N / 4. */
    int64_t full = e * COIN_TONE_WIN / 4;
    int64_t p0 = power(d, 0), p1 = power(d, 1);
    bool on = p0 * 100 >= full * SHARE_MIN_PCT && p1 * 100 >= full * SHARE_MIN_PCT;
    if (on) {
        /* RMS of the window, for the web UI's level readout. */
        int64_t ms = e / COIN_TONE_WIN, r = 0;
        while ((r + 1) * (r + 1) <= ms && r < 32767) r++;
        d->level = (uint16_t)r;
    }
    return on;
}

static int end_window(coin_tone_det_t *d, bool on)
{
    int result = 0;
    if (on) {
        d->off_run = 0;
        if (d->on_run < 255) d->on_run++;
        if (!d->in_beep && d->on_run >= ON_WINDOWS) {
            d->in_beep = true;
            if (d->beeps < 255) d->beeps++;
        }
        d->quiet = 0;
    } else {
        d->on_run = 0;
        if (d->off_run < 255) d->off_run++;
        if (d->in_beep && d->off_run >= OFF_WINDOWS) d->in_beep = false;
        if (!d->in_beep && d->beeps) {
            if (++d->quiet >= END_WINDOWS) {
                int c = coin_tone_cents(d->beeps);
                result = c ? c : -(int)d->beeps;
                d->beeps = 0;
                d->quiet = 0;
            }
        }
    }
    return result;
}

int coin_tone_push(coin_tone_det_t *d, const int16_t *samples, size_t n)
{
    int result = 0;
    for (size_t i = 0; i < n; i++) {
        d->dec_acc += samples[i];
        if (++d->dec_n < d->dec) continue;
        int32_t x = d->dec_acc / (int32_t)d->dec;
        d->dec_acc = 0;
        d->dec_n = 0;
        for (int k = 0; k < 2; k++) {
            int32_t q0 = (int32_t)(((int64_t)COEFF[k] * d->q1[k]) >> 14) - d->q2[k] + x;
            d->q2[k] = d->q1[k];
            d->q1[k] = q0;
        }
        d->energy += (int64_t)x * x;
        if (++d->filled == COIN_TONE_WIN) {
            int r = end_window(d, window_has_tone(d));
            if (r) result = r;
            d->q1[0] = d->q1[1] = d->q2[0] = d->q2[1] = 0;
            d->energy = 0;
            d->filled = 0;
        }
    }
    return result;
}

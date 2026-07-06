#include "dtmf.h"
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const float ROWS[4] = { 697.0f, 770.0f, 852.0f, 941.0f };
static const float COLS[4] = { 1209.0f, 1336.0f, 1477.0f, 1633.0f };

/* keypad[row][col] */
static const char KEYPAD[4][4] = {
    { '1', '2', '3', 'A' },
    { '4', '5', '6', 'B' },
    { '7', '8', '9', 'C' },
    { '*', '0', '#', 'D' },
};

bool dtmf_key_freqs(char key, float *row_hz, float *col_hz)
{
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            if (KEYPAD[r][c] == key) {
                if (row_hz) *row_hz = ROWS[r];
                if (col_hz) *col_hz = COLS[c];
                return true;
            }
        }
    }
    return false;
}

bool dtmf_make_osc(tone_osc_t *o, uint32_t sample_rate, char key, float amp)
{
    float r, c;
    if (!dtmf_key_freqs(key, &r, &c)) return false;
    tone_osc_init(o, sample_rate, r, c, amp);
    return true;
}

/* ---- Goertzel detector ---- */

static char classify(const float mags[8])
{
    /* strongest row and strongest col */
    int rb = 0, cb = 0;
    for (int i = 1; i < 4; i++) if (mags[i]     > mags[rb])     rb = i;
    for (int i = 1; i < 4; i++) if (mags[4 + i] > mags[4 + cb]) cb = i;

    float rmag = mags[rb], cmag = mags[4 + cb];

    /* energy gate: ignore near-silence / noise */
    const float MIN_MAG = 5.0e5f;
    if (rmag < MIN_MAG || cmag < MIN_MAG) return 0;

    /* twist: row/col should be within a reasonable ratio */
    float ratio = rmag / cmag;
    if (ratio > 8.0f || ratio < (1.0f / 8.0f)) return 0;

    /* second-harmonic / spurious rejection: the chosen row & col must
     * clearly dominate the other three in their group. */
    for (int i = 0; i < 4; i++) {
        if (i != rb && mags[i]     * 4.0f > rmag) return 0;
        if (i != cb && mags[4 + i] * 4.0f > cmag) return 0;
    }
    return KEYPAD[rb][cb];
}

void dtmf_detector_init(dtmf_detector_t *d, uint32_t sample_rate)
{
    memset(d, 0, sizeof(*d));
    d->sample_rate = sample_rate;
    d->block_n = sample_rate / 100; /* 10 ms blocks */
    if (d->block_n < 64) d->block_n = 64;

    for (int i = 0; i < 8; i++) {
        float f = (i < 4) ? ROWS[i] : COLS[i - 4];
        int   k = (int)(0.5f + ((float)d->block_n * f) / (float)sample_rate);
        float w = (2.0f * (float)M_PI / (float)d->block_n) * (float)k;
        d->coeff[i] = 2.0f * cosf(w);
        d->q1[i] = d->q2[i] = 0.0f;
    }
}

static void block_reset(dtmf_detector_t *d)
{
    for (int i = 0; i < 8; i++) { d->q1[i] = 0.0f; d->q2[i] = 0.0f; }
    d->filled = 0;
}

char dtmf_detector_push(dtmf_detector_t *d, const int16_t *samples, size_t n)
{
    char emitted = 0;

    for (size_t s = 0; s < n; s++) {
        float in = (float)samples[s];
        for (int i = 0; i < 8; i++) {
            float q0 = d->coeff[i] * d->q1[i] - d->q2[i] + in;
            d->q2[i] = d->q1[i];
            d->q1[i] = q0;
        }
        if (++d->filled < d->block_n) {
            continue;
        }

        /* block complete: compute magnitudes */
        float mags[8];
        for (int i = 0; i < 8; i++) {
            mags[i] = d->q1[i] * d->q1[i] + d->q2[i] * d->q2[i]
                      - d->coeff[i] * d->q1[i] * d->q2[i];
        }
        char digit = classify(mags);
        block_reset(d);

        /* debounce + edge detection: emit once per key press */
        if (digit != 0) {
            if (digit == d->candidate) {
                d->candidate_blocks++;
            } else {
                d->candidate = digit;
                d->candidate_blocks = 1;
            }
            if (!d->in_tone && d->candidate_blocks >= 2 && digit != d->last_emitted) {
                d->in_tone = true;
                d->last_emitted = digit;
                emitted = digit;
            }
        } else {
            /* silence block: end of tone, allow same key again next time */
            d->candidate = 0;
            d->candidate_blocks = 0;
            d->in_tone = false;
            d->last_emitted = 0;
        }
    }
    return emitted;
}

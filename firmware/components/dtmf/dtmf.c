#include "dtmf.h"
#include <math.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "dtmf";

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

/* Minimum gap (in ~25.6ms Goertzel blocks -- see dtmf_detector_init()) before
 * the *same* digit can be emitted again -- see dtmf_detector_push()'s
 * comment. 8 blocks =~ 205ms. */
#define SAME_DIGIT_LOCKOUT_BLOCKS 8

/* ---- Goertzel detector ----
 *
 * Fixed-point (Q14 coefficients, int32 state). The ESP32-C6's RISC-V core
 * has no hardware FPU (rv32imac -- no f/d extension), so a float version of
 * this recursion running per-sample on both lines' RX audio, continuously,
 * is soft-float in the hot path -- easily enough to blow the 10ms audio
 * frame budget and starve the watchdog once both lines are actually live.
 * Integer multiply (the M extension) is real hardware here. q1/q2 stay in
 * the same raw-sample-amplitude units as the old float version (only the
 * coeff multiply is scaled, then shifted back down), so MIN_MAG and the
 * ratio thresholds below are unchanged from the float version. */

static char classify(const int64_t mags[8])
{
    /* strongest row and strongest col */
    int rb = 0, cb = 0;
    for (int i = 1; i < 4; i++) if (mags[i]     > mags[rb])     rb = i;
    for (int i = 1; i < 4; i++) if (mags[4 + i] > mags[4 + cb]) cb = i;

    int64_t rmag = mags[rb], cmag = mags[4 + cb];

    /* energy gate: ignore near-silence / noise */
    const int64_t MIN_MAG = 500000;
    if (rmag < MIN_MAG || cmag < MIN_MAG) return 0;

    /* twist: row/col should be within a reasonable ratio (cross-multiplied
     * to avoid division). Real DTMF twist (ITU-T Q.24/Bellcore GR-506) is a
     * few dB at most either direction -- nowhere near 8x (~18dB). 8x was
     * loose enough to also accept a real, reproducible false positive:
     * measured live (see telephony.c's dtmf_tap comment -- some line/board
     * noise sits at ~770Hz, the row-1 frequency), every captured instance
     * had the row 4.1-7.7x the column, comfortably below the old limit and
     * comfortably above this one. 3x has margin on both sides: it clears
     * every measured false trigger by >35% and is still far looser than any
     * real phone/generator's twist should ever need. */
    if (rmag > 3 * cmag || cmag > 3 * rmag) return 0;

    /* second-harmonic / spurious rejection: the chosen row & col must
     * clearly dominate the other three in their group. */
    for (int i = 0; i < 4; i++) {
        if (i != rb && mags[i]     * 4 > rmag) return 0;
        if (i != cb && mags[4 + i] * 4 > cmag) return 0;
    }
    return KEYPAD[rb][cb];
}

void dtmf_detector_init(dtmf_detector_t *d, uint32_t sample_rate)
{
    memset(d, 0, sizeof(*d));
    d->sample_rate = sample_rate;
    /* 205 samples @ 8kHz (~25.6ms) is the standard Bellcore/ITU-T DTMF
     * Goertzel block size -- ~39Hz bin spacing. A 10ms block (100Hz bins,
     * what this used to be) can't resolve 852Hz and 941Hz: they're only
     * 89Hz apart, so both round to the *same* bin, tie exactly, and the
     * dominance check in classify() then rejects the ambiguous result as
     * noise. That silently broke every row-3/row-4 key against each
     * other -- '7'/'8'/'9' vs '*'/'0'/'#' -- not just '0'/'*'/'#' alone. */
    d->block_n = (size_t)(((uint64_t)sample_rate * 205 + 4000) / 8000);
    if (d->block_n < 64) d->block_n = 64;

    /* One-time setup -- float/cosf here is fine, this isn't the hot loop. */
    for (int i = 0; i < 8; i++) {
        float f = (i < 4) ? ROWS[i] : COLS[i - 4];
        int   k = (int)(0.5f + ((float)d->block_n * f) / (float)sample_rate);
        float w = (2.0f * (float)M_PI / (float)d->block_n) * (float)k;
        d->coeff[i] = (int32_t)lroundf(2.0f * cosf(w) * 16384.0f); /* Q14 */
        d->q1[i] = d->q2[i] = 0;
    }
}

static void block_reset(dtmf_detector_t *d)
{
    for (int i = 0; i < 8; i++) { d->q1[i] = 0; d->q2[i] = 0; }
    d->filled = 0;
}

char dtmf_detector_push(dtmf_detector_t *d, const int16_t *samples, size_t n)
{
    char emitted = 0;

    for (size_t s = 0; s < n; s++) {
        int32_t in = samples[s];
        for (int i = 0; i < 8; i++) {
            int32_t q0 = (int32_t)(((int64_t)d->coeff[i] * d->q1[i]) >> 14) - d->q2[i] + in;
            d->q2[i] = d->q1[i];
            d->q1[i] = q0;
        }
        if (++d->filled < d->block_n) {
            continue;
        }

        /* block complete: compute magnitudes */
        int64_t mags[8];
        for (int i = 0; i < 8; i++) {
            int64_t q1 = d->q1[i], q2 = d->q2[i];
            int64_t cross = (((int64_t)d->coeff[i] * q1) >> 14) * q2;
            mags[i] = q1 * q1 + q2 * q2 - cross;
        }
        char digit = classify(mags);
        block_reset(d);

        /* debounce + edge detection: emit once per key press.
         * Two separate, deliberately-decoupled timers:
         *  - in_tone/silence_blocks: is a tone (any digit) active right
         *    now? Clears after a short (50ms) real quiet gap, so a
         *    *different* next digit is still recognized promptly.
         *  - since_emit_blocks: how long since we last emitted THIS
         *    digit. A real analog phone/keypad can chatter -- contact
         *    bounce, or a cheap keypad's own auto-repeat -- and drop the
         *    tone entirely for tens of ms mid-press, which looks exactly
         *    like "released, pressed again" to the block-level detector.
         *    Gating same-digit re-emission on a longer, dialing-speed
         *    window (not just "was there a brief quiet gap") is what
         *    actually rejects that, without slowing down a genuinely
         *    different next digit. */
        if (d->since_emit_blocks < SAME_DIGIT_LOCKOUT_BLOCKS) d->since_emit_blocks++;

        if (digit != 0) {
            d->silence_blocks = 0;
            if (digit == d->candidate) {
                d->candidate_blocks++;
            } else {
                d->candidate = digit;
                d->candidate_blocks = 1;
            }
            if (!d->in_tone && d->candidate_blocks >= 2) {
                d->in_tone = true; /* track this occurrence either way */
                d->press_digit = digit;
                d->press_start_us = esp_timer_get_time();
                bool too_soon = (digit == d->last_emitted) &&
                                 (d->since_emit_blocks < SAME_DIGIT_LOCKOUT_BLOCKS);
                if (!too_soon) {
                    d->last_emitted = digit;
                    d->since_emit_blocks = 0;
                    emitted = digit;
                    ESP_LOGD(TAG, "'%c' START", digit);
                } else {
                    ESP_LOGD(TAG, "'%c' START (suppressed, repeat too soon)", digit);
                }
            }
        } else {
            /* non-classifying block: only declare the tone over after a
             * couple of these in a row (~51ms) -- a single one is as likely
             * to be a momentary dip mid-tone (line noise/echo) as a real
             * release. */
            d->candidate = 0;
            d->candidate_blocks = 0;
            if (d->in_tone && ++d->silence_blocks >= 2) {
                d->in_tone = false;
                d->silence_blocks = 0;
                int64_t held_ms = (esp_timer_get_time() - d->press_start_us) / 1000;
                ESP_LOGD(TAG, "'%c' END (held %lld ms)", d->press_digit, (long long)held_ms);
            }
        }
    }
    return emitted;
}

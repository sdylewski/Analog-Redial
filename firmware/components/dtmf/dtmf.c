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

/* Minimum time before the *same* digit can be emitted again -- see
 * dtmf_detector_push()'s comment. Counted in windows (see DTMF_BANKS):
 * 32 windows =~ 205 ms. */
#define SAME_DIGIT_LOCKOUT_WINDOWS 32

/* A key counts as pressed once this many windows in a row classify as the
 * same digit: 3 windows span ~38 ms of unbroken tone, the ITU minimum tone
 * length. (The old, non-overlapping detector needed two whole blocks, ~51 ms
 * of tone plus whatever alignment it had.) */
#ifndef START_WINDOWS
#define START_WINDOWS 3
#endif
/* A real DTMF window holds nearly all of its energy in two bins; speech and
 * music spread theirs. Percent of the ideal (all of it in the strongest row
 * and column bin) that a window must reach to count as a key. 50 passes
 * tones with 4 kHz-wide noise 14 dB under them, +-5 dB twist, and tones
 * clipped flat by a 1.2x overdrive, and rejects the speech, music and modem
 * clips this was tried on (tests in firmware/test/host). */
#ifndef DTMF_MIN_CONC_PCT
#define DTMF_MIN_CONC_PCT 50
#endif
/* ...and ends after this many in a row that classify as nothing. A pause of
 * 40 ms always contains at least two windows with no tone in them at all. */
#define END_WINDOWS 2

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

static char classify(const int64_t mags[8], int64_t min_mag, int64_t energy, size_t block_n)
{
    /* strongest row and strongest col */
    int rb = 0, cb = 0;
    for (int i = 1; i < 4; i++) if (mags[i]     > mags[rb])     rb = i;
    for (int i = 1; i < 4; i++) if (mags[4 + i] > mags[4 + cb]) cb = i;

    int64_t rmag = mags[rb], cmag = mags[4 + cb];

    /* energy gate: ignore near-silence / noise */
    if (rmag < min_mag || cmag < min_mag) return 0;

    /* tone-ness: two bins should hold most of the window's energy. For a pure
     * pair, |Xr|^2 + |Xc|^2 = N * sum(x^2) / 2. */
    if (DTMF_MIN_CONC_PCT > 0 &&
        (rmag + cmag) * 200 < (int64_t)DTMF_MIN_CONC_PCT * (int64_t)block_n * energy) return 0;

    /* twist: row/col should be within a reasonable ratio (cross-multiplied
     * to avoid division). A single tight limit, not tiered by loudness --
     * this used to need an 8x "loud" tier specifically to tolerate ADC
     * clipping (a real handset's DTMF was hitting int16 full scale on
     * this board, distorting twist unpredictably block-to-block, up to
     * 245x on the noisy attack/decay edges around a press). Clipping is
     * now fixed at the analog level (voltage divider between the SLIC and
     * codec, see docs/signal-levels.md), and the AGC below keeps
     * captured levels in a consistent, predictable range on top of that
     * regardless of a given phone's natural loudness -- so nothing should
     * be arriving distorted anymore, and one limit close to real DTMF
     * spec (ITU-T Q.24's normal twist tolerance is roughly 1.6-2.5x; 3x
     * keeps a bit of margin) should cover real presses on any properly-
     * functioning phone. The original false-positive this also guards
     * against (~770Hz line/board noise, see telephony.c's dtmf_tap
     * comment) measured twist 4.1-7.7x, comfortably rejected by 3x. */
    const int64_t twist_limit = 3;
    if (rmag > twist_limit * cmag || cmag > twist_limit * rmag) return 0;

    /* second-harmonic / spurious rejection: the chosen row & col must
     * clearly dominate the other three in their group. Row0/row1
     * (697/770Hz) get a looser cross-check than the rest -- measured live
     * on a real handset, its '3'/'6' keys produce real, simultaneous
     * energy at BOTH 697 and 770Hz (confirmed: synthetic dtmf_gen tones
     * for the same digits are perfectly clean via digital loopback, which
     * bypasses codec+SLIC entirely -- so it's the handset, not the
     * board). The real tone is still reliably 20-90% louder than the
     * companion across most of a press's blocks (only ever a handful of
     * blocks near-tied or briefly reversed) -- just never near the
     * 4x/75% separation this check demands elsewhere. 80% (~1.25x)
     * clears every observed multi-block run with margin while staying
     * well short of the ~99-100% ties -- candidate_blocks>=2 (below) is
     * the second layer against a single ambiguous block slipping through. */
    char result = KEYPAD[rb][cb];
    for (int i = 0; i < 4; i++) {
        if (i != rb) {
            bool row01_pair = (rb == 0 && i == 1) || (rb == 1 && i == 0);
            if (row01_pair ? (mags[i] * 5 > rmag * 4) : (mags[i] * 4 > rmag)) return 0;
        }
        if (i != cb && mags[4 + i] * 4 > cmag) return 0;
    }
    return result;
}

void dtmf_detector_init(dtmf_detector_t *d, uint32_t sample_rate)
{
    memset(d, 0, sizeof(*d));
    d->sample_rate = sample_rate;
    /* Analyse at 8 kHz: average groups of input samples (48 kHz -> 6 at a
     * time). The windows are the standard 205 samples @ 8 kHz (~25.6 ms,
     * ~39 Hz bins -- the Bellcore/ITU-T DTMF Goertzel block): a shorter one
     * can't resolve 852 and 941 Hz, which round to the same bin and tie, and
     * the dominance check in classify() then rejects the result as noise,
     * silently breaking every row-3/row-4 key. Running at 8 kHz also keeps
     * four staggered windows cheaper than the single 48 kHz one was. A rate
     * that doesn't divide into 8 kHz is analysed as it comes. */
    d->dec = (sample_rate >= 8000 && sample_rate % 8000 == 0) ? sample_rate / 8000 : 1;
    uint32_t arate = sample_rate / d->dec;
    d->block_n = (size_t)(((uint64_t)arate * 205 + 4000) / 8000);
    if (d->block_n < 64) d->block_n = 64;
    /* The energy gate was 500000 for the old 1230-sample window; magnitudes
     * grow with the square of the window length. */
    d->min_mag = (int64_t)((double)500000 * (double)d->block_n * (double)d->block_n / (1230.0 * 1230.0));
    if (d->min_mag < 1000) d->min_mag = 1000;

    /* One-time setup -- float/cosf here is fine, this isn't the hot loop. */
    for (int i = 0; i < 8; i++) {
        float f = (i < 4) ? ROWS[i] : COLS[i - 4];
        int   k = (int)(0.5f + ((float)d->block_n * f) / (float)arate);
        float w = (2.0f * (float)M_PI / (float)d->block_n) * (float)k;
        d->coeff[i] = (int32_t)lroundf(2.0f * cosf(w) * 16384.0f); /* Q14 */
    }
    /* Stagger the windows: window b starts b/DTMF_BANKS of a window late. */
    for (int b = 0; b < DTMF_BANKS; b++) d->filled[b] = (size_t)b * d->block_n / DTMF_BANKS;

    d->agc_gain_q8 = 256; /* unity -- see dtmf_detector_push() */
}

/* AGC tuning. Target/gate are raw int16 sample-amplitude units (peak, not
 * Goertzel magnitude); gain is Q8 fixed point (256 = 1.0x).
 *   - AGC_TARGET_PEAK: ~49% of int16 full scale -- comfortably matches what
 *     a real press looks like post-divider on this hardware (measured
 *     9000-17000), with headroom below clipping for a phone that runs a
 *     bit hotter.
 *   - AGC_GATE_PEAK: only adapt the gain estimate from blocks that clear
 *     this -- gates on "plausibly real signal, not silence/background",
 *     distinct from (and higher than) MIN_MAG's much lower noise-floor
 *     gate in classify(). Below it, HOLD the current gain rather than
 *     decay toward unity, so a quiet gap mid-dialing doesn't erase what
 *     was just learned about this call's line/handset. Doesn't reopen the
 *     original false-positive: that noise was always rejected by twist/
 *     dominance (ratio checks, scale-invariant -- unaffected by a uniform
 *     gain), never by MIN_MAG, so amplifying it doesn't help it pass.
 *   - MIN/MAX_GAIN: 0.25x-8x -- wide enough for a genuinely quiet old
 *     phone or a hot one, bounded so a stray transient can't send gain to
 *     an extreme in one step.
 *   - SMOOTH_DIV: converges over ~4 blocks (~100ms) -- fast enough to
 *     settle within a single press (presses run 6-11 blocks in the data
 *     this was tuned against), slow enough not to chase every block. */
#define AGC_TARGET_PEAK 16000
#define AGC_GATE_PEAK    4000
#define AGC_MIN_GAIN_Q8    64
#define AGC_MAX_GAIN_Q8  2048
#define AGC_SMOOTH_DIV     16

char dtmf_detector_push(dtmf_detector_t *d, const int16_t *samples, size_t n)
{
    char emitted = 0;

    for (size_t s = 0; s < n; s++) {
        d->dec_acc += samples[s];
        if (++d->dec_n < d->dec) continue;
        int32_t raw = d->dec_acc / (int32_t)d->dec;
        d->dec_acc = 0;
        d->dec_n = 0;

        int32_t a = raw < 0 ? -raw : raw;
        int32_t in = (raw * d->agc_gain_q8) >> 8;
        for (int b = 0; b < DTMF_BANKS; b++) {
            if (a > d->peak[b]) d->peak[b] = a;
            d->energy[b] += (int64_t)in * in;
            for (int i = 0; i < 8; i++) {
                int32_t q0 = (int32_t)(((int64_t)d->coeff[i] * d->q1[b][i]) >> 14) - d->q2[b][i] + in;
                d->q2[b][i] = d->q1[b][i];
                d->q1[b][i] = q0;
            }
            if (++d->filled[b] < d->block_n) continue;

            /* window complete: compute magnitudes */
            int64_t mags[8];
            for (int i = 0; i < 8; i++) {
                int64_t q1 = d->q1[b][i], q2 = d->q2[b][i];
                int64_t cross = (((int64_t)d->coeff[i] * q1) >> 14) * q2;
                mags[i] = q1 * q1 + q2 * q2 - cross;
            }
            char digit = classify(mags, d->min_mag, d->energy[b], d->block_n);

            /* AGC: re-estimate gain for the *next* window from this one's
             * peak. Gated per the comment above AGC_TARGET_PEAK. */
            if (d->peak[b] >= AGC_GATE_PEAK) {
                int32_t target = (int32_t)(((int64_t)AGC_TARGET_PEAK << 8) / d->peak[b]);
                if (target < AGC_MIN_GAIN_Q8) target = AGC_MIN_GAIN_Q8;
                if (target > AGC_MAX_GAIN_Q8) target = AGC_MAX_GAIN_Q8;
                d->agc_gain_q8 += (target - d->agc_gain_q8) / AGC_SMOOTH_DIV;
            }
            for (int i = 0; i < 8; i++) { d->q1[b][i] = 0; d->q2[b][i] = 0; }
            d->filled[b] = 0;
            d->peak[b] = 0;
            d->energy[b] = 0;

            /* debounce + edge detection: emit once per key press.
             * Two separate, deliberately-decoupled timers:
             *  - in_tone/silence_n: is a tone (any digit) active right
             *    now? Clears after END_WINDOWS windows with no tone in
             *    them, so the next key is recognised promptly. A
             *    *different* digit that differs from the current one in
             *    both its row and its column may start without waiting for
             *    that: some handsets dial with pauses too short to leave
             *    two clean windows. (Not for a digit sharing a row or
             *    column: a handset whose '3' and '6' both ring at 697 and
             *    770 Hz -- measured on a real one -- flips between them
             *    inside one press.)
             *  - since_emit: how long since we last emitted THIS digit. A
             *    real analog phone/keypad can chatter -- contact bounce, or
             *    a cheap keypad's own auto-repeat -- and drop the tone for
             *    tens of ms mid-press, which looks exactly like "released,
             *    pressed again". Gating same-digit re-emission on a longer,
             *    dialing-speed window is what rejects that, without slowing
             *    down a genuinely different next digit. */
            if (d->since_emit < SAME_DIGIT_LOCKOUT_WINDOWS) d->since_emit++;

            if (digit != 0) {
                d->silence_n = 0;
                if (digit == d->candidate) {
                    d->candidate_n++;
                } else {
                    d->candidate = digit;
                    d->candidate_n = 1;
                }
                bool unrelated_key = false;
                if (d->in_tone && digit != d->press_digit) {
                    float r0, c0, r1, c1;
                    unrelated_key = dtmf_key_freqs(d->press_digit, &r0, &c0) &&
                                    dtmf_key_freqs(digit, &r1, &c1) && r0 != r1 && c0 != c1;
                }
                if (d->candidate_n >= START_WINDOWS && (!d->in_tone || unrelated_key)) {
                    d->in_tone = true; /* track this occurrence either way */
                    d->press_digit = digit;
                    d->press_start_us = esp_timer_get_time();
                    bool too_soon = (digit == d->last_emitted) &&
                                     (d->since_emit < SAME_DIGIT_LOCKOUT_WINDOWS);
                    if (!too_soon) {
                        d->last_emitted = digit;
                        d->since_emit = 0;
                        if (!emitted) emitted = digit;
                        ESP_LOGD(TAG, "'%c' START", digit);
                    } else {
                        ESP_LOGD(TAG, "'%c' START (suppressed, repeat too soon)", digit);
                    }
                }
            } else {
                /* a window with no valid key in it: only declare the tone
                 * over after a couple in a row -- a single one is as likely
                 * to be a momentary dip mid-tone (line noise/echo) as a
                 * real release. */
                d->candidate = 0;
                d->candidate_n = 0;
                if (d->in_tone && ++d->silence_n >= END_WINDOWS) {
                    d->in_tone = false;
                    d->silence_n = 0;
                    int64_t held_ms = (esp_timer_get_time() - d->press_start_us) / 1000;
                    ESP_LOGD(TAG, "'%c' END (held %lld ms)", d->press_digit, (long long)held_ms);
                }
            }
        }
    }
    return emitted;
}

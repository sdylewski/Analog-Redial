#include "aec.h"
#include "board.h"
#include "esp_log.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "aec";

#define NLINES 2

/* Reference history ring. Must hold the bulk delay plus the tap span; sized
 * to a power of two so indexing is a mask instead of a modulo (this is the
 * hot loop -- AEC_TAPS multiply-accumulates per sample). */
#define REF_RING      1024
#define REF_MASK      (REF_RING - 1)

/*
 * Fixed-point NLMS. The ESP32-C6 is rv32imac -- no FPU -- so anything
 * per-sample has to be integer, the same constraint that shaped tones.c's
 * oscillator and the DTMF Goertzel detector.
 *
 * Weights are Q15. The textbook update is
 *     w[i] += mu * e * x[i] / P      (P = energy in the tap window)
 * which as written needs a 64-bit multiply and divide PER TAP. Instead the
 * sample-dependent part is folded into one scalar computed once per sample:
 *     g = (e << STEP_SHIFT) / P            <- one 64-bit divide per sample
 *     w[i] += (g * x[i]) >> DELTA_SHIFT    <- pure 32-bit in the tap loop
 * g is clamped to int16 range so `g * x[i]` cannot overflow int32.
 *
 * DELTA_SHIFT = STEP_SHIFT - 15 + MU_SHIFT reconciles the Q15 weight scaling
 * with the step size mu = 2^-MU_SHIFT.
 */
#define STEP_SHIFT   31
#define MU_SHIFT      2            /* mu = 1/4 -- converges in well under a second
                                    * at 8 kHz while staying stable on speech */
#define DELTA_SHIFT  (STEP_SHIFT - 15 + MU_SHIFT)

/* Below this the far end is effectively silent: there is no echo to learn
 * from, and normalizing by a near-zero energy would make the step explode. */
#define XPOW_FLOOR   ((int64_t)AEC_TAPS * 64)

/* Weight magnitude ceiling (Q15). The true path is ~-20 dB, so coefficients
 * well past unity mean the filter is diverging, not modelling. */
#define W_LIMIT      (1 << 17)

/* Double-talk: the measured coupling is about -20 dB, so echo power is
 * roughly ref/100. Near-end power beyond 4x that (ref/25) is energy the echo
 * cannot account for -- someone is talking on this end, and adapting on it
 * would drag the filter away from the real path. */
#define DT_RATIO     25

/* The big per-line buffers (3.3 KB each), allocated the first time a line's
 * AEC is turned on and kept until reboot: aec_process() runs on the audio
 * task, so freeing them on "off" could pull them out from under it. Most
 * boards never turn AEC on and never pay for them. */
typedef struct {
    int16_t  ring[REF_RING];
    int32_t  wts[AEC_TAPS];  /* Q15 adaptive weights */
} aec_bufs_t;

typedef struct {
    bool     enabled;        /* only ever true once b is allocated */
    uint32_t delay_ms;
    uint32_t delay_samp;

    aec_bufs_t *b;
    uint32_t w_idx;          /* next ring write position */
    int64_t  xpow;           /* running energy of the tap window */

    /* rolling stats, reset when read */
    int64_t  e_near;
    int64_t  e_out;
    uint32_t blocks;
    uint32_t blocks_frozen;
    bool     converged;
} aec_line_t;

static aec_line_t s_l[NLINES];

static inline bool line_ok(int line) { return line >= 0 && line < NLINES; }

static void reset_locked(aec_line_t *a)
{
    if (a->b) memset(a->b, 0, sizeof(*a->b));
    a->w_idx = 0;
    a->xpow = 0;
    a->e_near = a->e_out = 0;
    a->blocks = a->blocks_frozen = 0;
    a->converged = false;
}

esp_err_t aec_init(void)
{
    for (int i = 0; i < NLINES; i++) {
        s_l[i].enabled = false;
        /* Bulk delay is a per-device property (each board's own I2S/codec/
         * acoustic path timing, measured via `echoprobe`) -- one firmware
         * image serves every board of a type, so it can't be a single
         * compile-time constant. board_aec_delay_get() returns whatever was
         * persisted via `aec delay <ms> [line]` or a completed `aec sweep`,
         * falling back to AEC_DEFAULT_DELAY_MS if nothing's been set yet. */
        uint32_t ms = board_aec_delay_get(i, AEC_DEFAULT_DELAY_MS);
        if (ms > AEC_MAX_DELAY_MS) ms = AEC_DEFAULT_DELAY_MS; /* guard a corrupt/stale NVS value */
        s_l[i].delay_ms = ms;
        s_l[i].delay_samp = ms * AEC_RATE_HZ / 1000;
        reset_locked(&s_l[i]);
    }
    return ESP_OK;
}

void aec_reset(int line)
{
    if (!line_ok(line)) return;
    reset_locked(&s_l[line]);
}

void aec_set_enabled(int line, bool on)
{
    if (!line_ok(line)) return;
    if (s_l[line].enabled == on) return;
    if (on && !s_l[line].b) {
        s_l[line].b = calloc(1, sizeof(aec_bufs_t));
        if (!s_l[line].b) {
            ESP_LOGW(TAG, "line %d: AEC not enabled: no memory for its buffers", line);
            return;
        }
    }
    /* Always start from zero weights: state adapted during a previous call
     * (different handset position, different far end) is worse than nothing. */
    reset_locked(&s_l[line]);
    s_l[line].enabled = on;
    ESP_LOGI(TAG, "line %d: AEC %s (delay %u ms, %d taps)",
             line, on ? "on" : "off", (unsigned)s_l[line].delay_ms, AEC_TAPS);
}

bool aec_is_enabled(int line) { return line_ok(line) && s_l[line].enabled; }

esp_err_t aec_set_delay_ms(int line, uint32_t ms)
{
    if (!line_ok(line) || ms > AEC_MAX_DELAY_MS) return ESP_ERR_INVALID_ARG;
    uint32_t samp = ms * AEC_RATE_HZ / 1000;
    if (samp + AEC_TAPS + 1 >= REF_RING) return ESP_ERR_INVALID_ARG;
    s_l[line].delay_ms = ms;
    s_l[line].delay_samp = samp;
    reset_locked(&s_l[line]); /* weights were fitted to the old alignment */
    ESP_LOGI(TAG, "line %d: AEC bulk delay %u ms (%u samples)",
             line, (unsigned)ms, (unsigned)samp);
    return ESP_OK;
}

void aec_process(int line, const int16_t *ref, const int16_t *near,
                 int16_t *out, size_t n)
{
    if (!line_ok(line)) return;
    aec_line_t *a = &s_l[line];
    if (!a->enabled || !a->b) {
        if (out != near) memcpy(out, near, n * sizeof(int16_t));
        return;
    }
    int16_t *ring = a->b->ring;
    int32_t *wts = a->b->wts;

    const uint32_t D = a->delay_samp;
    int64_t blk_near = 0, blk_out = 0, blk_ref = 0;

    for (size_t k = 0; k < n; k++) {
        /* Push this playout sample, then advance. */
        ring[a->w_idx & REF_MASK] = ref[k];
        a->w_idx++;

        /* Sample aligned with near[k] sits D back from the newest. */
        const uint32_t base = a->w_idx - 1 - D;

        /* Slide the tap-window energy: one sample in, one out. O(1) rather
         * than re-summing AEC_TAPS values every sample. */
        int32_t x_in  = ring[base & REF_MASK];
        int32_t x_out = ring[(base - AEC_TAPS) & REF_MASK];
        a->xpow += (int64_t)x_in * x_in - (int64_t)x_out * x_out;
        if (a->xpow < 0) a->xpow = 0; /* guard against rounding drift */

        /* y = w . x  (Q15 weights) */
        int64_t acc = 0;
        for (int i = 0; i < AEC_TAPS; i++) {
            acc += (int64_t)wts[i] * ring[(base - i) & REF_MASK];
        }
        int32_t y = (int32_t)(acc >> 15);

        int32_t d = near[k];
        int32_t e = d - y;
        if (e > 32767) e = 32767;
        else if (e < -32768) e = -32768;

        blk_near += (int64_t)d * d;
        blk_out  += (int64_t)e * e;
        blk_ref  += (int64_t)x_in * x_in;

        out[k] = (int16_t)e;

        /* Adapt. Skipped when the far end is silent (nothing to learn) --
         * double-talk freezing is decided per block, below, and applied via
         * a->blocks_frozen on the NEXT block; within a block we only gate on
         * reference energy, which is the cheap per-sample check. */
        if (a->xpow > XPOW_FLOOR) {
            int64_t g64 = (((int64_t)e) << STEP_SHIFT) / a->xpow;
            if (g64 > 32767) g64 = 32767;
            else if (g64 < -32767) g64 = -32767;
            int32_t g = (int32_t)g64;
            if (g) {
                for (int i = 0; i < AEC_TAPS; i++) {
                    int32_t xi = ring[(base - i) & REF_MASK];
                    wts[i] += (g * xi) >> DELTA_SHIFT;
                    if (wts[i] > W_LIMIT) wts[i] = W_LIMIT;
                    else if (wts[i] < -W_LIMIT) wts[i] = -W_LIMIT;
                }
            }
        }
    }

    /* Per-block bookkeeping: double-talk detection and the ERLE/freeze stats
     * the console and web UI report. Doing this per block rather than per
     * sample keeps it off the hot path. */
    a->blocks++;
    bool double_talk = (blk_near * DT_RATIO > blk_ref) && (blk_ref > 0);
    if (double_talk) {
        a->blocks_frozen++;
        /* Roll the weights back toward where they were by damping the last
         * block's worth of adaptation: cheaper and steadier than maintaining
         * a full shadow copy, and enough to stop a burst of near-end speech
         * from dragging the filter far off. */
        for (int i = 0; i < AEC_TAPS; i++) wts[i] -= wts[i] >> 6;
    }
    a->e_near += blk_near;
    a->e_out  += blk_out;
    if (blk_near > 0 && blk_out * 2 < blk_near) a->converged = true;
}

void aec_get_status(int line, aec_status_t *st)
{
    if (!st) return;
    memset(st, 0, sizeof(*st));
    if (!line_ok(line)) return;
    aec_line_t *a = &s_l[line];

    st->enabled = a->enabled;
    st->delay_ms = a->delay_ms;
    st->converged = a->converged;
    st->freeze_pct = a->blocks ? (int)((a->blocks_frozen * 100) / a->blocks) : 0;

    /* ERLE = 10*log10(near/out), integer-only: count halvings, then refine.
     * Not called per sample, but there is still no FPU, so no log10f(). */
    st->erle_db = 0;
    if (a->e_out > 0 && a->e_near > a->e_out) {
        int64_t ratio = a->e_near / a->e_out;
        int db = 0;
        while (ratio >= 10 && db < 60) { ratio /= 10; db += 10; }
        /* ratio now 1..9 -- approximate the remainder (10*log10 of 1..9) */
        static const int frac[10] = { 0, 0, 3, 4, 6, 6, 7, 8, 9, 9 };
        db += frac[ratio < 10 ? (int)ratio : 9];
        st->erle_db = db;
    }

    /* Stats are windowed, not lifetime: reset on read so a fresh `aec status`
     * reflects the last stretch of the call rather than being dominated by
     * whatever happened during initial convergence. */
    a->e_near = a->e_out = 0;
    a->blocks = a->blocks_frozen = 0;
}

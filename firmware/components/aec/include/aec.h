/*
 * aec.h — acoustic echo canceller (NLMS), fixed point, 8 kHz.
 *
 * Cancels this board's OWN earpiece -> mic leak: whatever the far end sends
 * us gets played out the handset earpiece, leaks back into the handset mic,
 * and would otherwise be sent straight back to them as an echo of their own
 * voice. The canceller predicts that leaked copy from the signal we played
 * (the "reference") and subtracts it from the mic signal before it goes out.
 *
 * WHY 8 kHz AND NOT THE ENGINE'S 48 kHz: cost is 2 MACs per tap per sample,
 * and the filter has to span the echo path in TIME -- so the same physical
 * path costs 6x as many taps at 48 kHz. Measured tail here is ~9 ms, which
 * is ~430 taps at 48 kHz (~41M MAC/s, roughly half of a 160 MHz core with no
 * FPU) versus ~72 taps at 8 kHz. The uplink is decimated to 8 kHz for G.711
 * anyway, so cancelling at 8 kHz costs nothing in quality.
 *
 * WHY A BULK DELAY IS COMPENSATED SEPARATELY: `echoprobe` (bringup.c) on real
 * hardware measured the leak arriving ~36-41 ms after playout, with only ~9 ms
 * of actual ringing after that -- but that measurement used a standalone burst
 * outside a call. Aligning the reference by an offset first means the
 * adaptive part doesn't have to spend taps re-learning a delay that's mostly
 * just fixed I2S DMA buffering.
 *
 * THE FILTER IS WIDER THAN THAT TAIL SUGGESTS, on purpose: `aec sweep`
 * (bringup.c) run live against a real call found TWO separate ERLE peaks,
 * roughly 20 ms and 36 ms, both real and reproducible run to run -- not
 * measurement noise, but (most likely) two distinct leak paths landing at
 * different delays (plausibly an electrical leak arriving sooner, and the
 * acoustic earpiece->mic leak echoprobe characterized arriving later, though
 * that's inferred from the timing pattern rather than confirmed directly).
 * A filter narrower than the gap between them can only capture one at a
 * time -- whichever the anchor happens to favor -- which is exactly what
 * capped ERLE around 10-16 dB instead of cancelling both together. AEC_TAPS
 * and AEC_DEFAULT_DELAY_MS are sized so one anchor brackets both.
 *
 * NOT modelled: nonlinearity (speaker/SLIC distortion) and any path change
 * faster than the filter adapts. A moving handset re-converges in well under
 * a second; a genuinely nonlinear leak leaves a residual this can't remove.
 */
#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AEC_RATE_HZ   8000
/* 320 taps = 40 ms at 8 kHz. Anchored at AEC_DEFAULT_DELAY_MS=12, the window
 * spans 12-52 ms -- comfortably bracketing both observed ERLE peaks (~20 ms
 * and ~36 ms, see above) with margin on either side for a different board
 * or handset landing a bit differently. Cost: 320 taps * 8000 Hz * 2 MACs
 * = 5.1M MAC/s, ~32% of a 160 MHz core with no FPU -- more than the original
 * 192-tap estimate (~19%) but still far under the ~half-a-core cost 48 kHz
 * cancellation would have been for the same time span. */
#define AEC_TAPS      320
#define AEC_DEFAULT_DELAY_MS 12
#define AEC_MAX_DELAY_MS     60

typedef struct {
    bool     enabled;
    uint32_t delay_ms;
    /* Echo return loss enhancement: how much quieter the outgoing signal got.
     * 0 means "no measurable improvement" -- either not converged yet, or
     * there is no linear echo on this line to remove. Meaningful only while
     * the far end is actually talking (there's nothing to cancel in silence). */
    int      erle_db;
    /* Percent of recent blocks where adaptation was frozen because near-end
     * speech was detected. Sustained ~100% while only the far end is talking
     * means the double-talk detector is mis-firing and the filter can never
     * learn; sustained 0% during genuine double-talk risks divergence. */
    int      freeze_pct;
    bool     converged; /* has produced real cancellation at least once */
} aec_status_t;

esp_err_t aec_init(void);

/* Runtime toggle. Disabling resets the filter, so re-enabling starts clean
 * rather than resuming from weights adapted to a since-ended call. */
void aec_set_enabled(int line, bool on);
bool aec_is_enabled(int line);

esp_err_t aec_set_delay_ms(int line, uint32_t ms);

/* Drop adapted state (call teardown, delay change, handset moved). */
void aec_reset(int line);

/*
 * One block, all at AEC_RATE_HZ. `ref` is what was played toward the earpiece,
 * `near` is what the mic captured, `out` receives near minus the predicted
 * echo. `out` may alias `near`. Passthrough (out == near) when disabled.
 *
 * `ref` and `near` MUST be sampled at the same point in the pipeline -- the
 * fixed offset between them is exactly what AEC_DELAY_MS compensates. Taking
 * the reference from where RTP packets arrive instead would add the (variable)
 * playout buffer depth on top, which no fixed delay can correct for.
 */
void aec_process(int line, const int16_t *ref, const int16_t *near,
                 int16_t *out, size_t n);

void aec_get_status(int line, aec_status_t *st);

#ifdef __cplusplus
}
#endif

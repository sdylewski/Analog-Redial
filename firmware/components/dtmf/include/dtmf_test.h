/*
 * dtmf_test.h -- the guided touch-tone test: capture what a phone really sends
 * for each key, work out which detector limits would accept it, and write a
 * report. No ESP-IDF in here (firmware/test/host drives it with synthetic
 * phones); telephony.c owns the session, webui the card.
 *
 * Only one detector window in four is kept (bank 0, so they don't overlap):
 * 40 of them is about a second of tone per key.
 */
#pragma once

#include "dtmf.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DTMF_TEST_KEYS     12
#define DTMF_TEST_WINDOWS  40
#define DTMF_TEST_SNIPPET  410  /* samples of raw 8 kHz tone kept per key (~51 ms), for offline analysis */

typedef struct {
    uint8_t bins[8];  /* 20*log10(magnitude): 0.5 dB steps. Rows 0-3, then columns 0-3 */
    uint8_t energy;   /* the window's total energy, same scale */
    uint8_t peak;     /* peak |sample| / 256, before the AGC */
} dtmf_test_win_t;

typedef enum {
    DTMF_TEST_NOISE = 0, /* measuring the quiet line, before the first key */
    DTMF_TEST_RELEASE,   /* waiting for quiet before the next key */
    DTMF_TEST_PRESS,     /* waiting for the key */
    DTMF_TEST_HOLD,      /* recording it */
    DTMF_TEST_FINISHED,
} dtmf_test_phase_t;

typedef struct {
    volatile dtmf_test_phase_t phase;
    volatile int  idx;      /* which key, into dtmf_test_key() */
    volatile bool skip;     /* set by another task: give up on this key */
    int     noise_peak, tone_peak_min;
    int     noise_n, quiet_run, wait_n;
    int     n[DTMF_TEST_KEYS];
    int     snip_n[DTMF_TEST_KEYS];
    char    heard[DTMF_TEST_KEYS];      /* first digit the detector itself reported */
    uint8_t heard_n[DTMF_TEST_KEYS];
    dtmf_test_win_t win[DTMF_TEST_KEYS][DTMF_TEST_WINDOWS];
    int16_t snip[DTMF_TEST_KEYS][DTMF_TEST_SNIPPET];
} dtmf_test_capture_t;

typedef enum {
    DTMF_VERDICT_GOOD = 0, /* every key tested is fine on the standard limits */
    DTMF_VERDICT_TUNED,    /* every key tested is fine on the recommended limits */
    DTMF_VERDICT_PARTIAL,  /* the recommended limits help, but some keys are still poor */
    DTMF_VERDICT_NONE,     /* no key could be measured */
} dtmf_verdict_t;

typedef struct {
    char    key;
    char    heard;      /* 0 = the detector reported nothing */
    char    sent;       /* the key the tones actually belong to, 0 = unclear */
    uint8_t heard_n;
    uint8_t n;          /* windows recorded, 0 = key skipped or never heard */
    uint8_t pass_std, pass_cur, pass_rec; /* % of windows accepted */
    uint8_t clip_pct;   /* % of windows at or near full scale */
    uint8_t peak;       /* as dtmf_test_win_t.peak: typical for this key */
    int16_t twist_db10; /* column over row, 0.1 dB: typical for this key */
} dtmf_test_key_result_t;

typedef struct {
    dtmf_verdict_t verdict;
    bool           clipped;
    int            tested, poor_rec;
    int            noise_peak;
    dtmf_tuning_t  current, recommended;
    dtmf_test_key_result_t keys[DTMF_TEST_KEYS];
} dtmf_test_result_t;

/* The key at each step: 1-9, *, 0, #. */
char dtmf_test_key(int idx);

void dtmf_test_init(dtmf_test_capture_t *c);
/* dtmf_detector_set_capture() callbacks: ctx is the capture. */
void dtmf_test_on_window(void *ctx, const dtmf_window_t *w);
void dtmf_test_on_sample(void *ctx, int16_t raw);
/* For each digit dtmf_detector_push() returns during the test. */
void dtmf_test_on_digit(dtmf_test_capture_t *c, char digit);

/* Works out the recommendation. Slow (soft float): not for the audio task. */
void dtmf_test_analyze(const dtmf_test_capture_t *c, const dtmf_tuning_t *current, dtmf_test_result_t *r);

/* Writes the text report through `out`, a piece at a time. */
typedef void (*dtmf_test_out_t)(void *ctx, const char *text, size_t len);
void dtmf_test_report(const dtmf_test_capture_t *c, const dtmf_test_result_t *r, const char *header,
                      dtmf_test_out_t out, void *ctx);

#ifdef __cplusplus
}
#endif

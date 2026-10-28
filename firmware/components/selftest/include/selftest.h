/* selftest.h — guided hardware bring-up check for a freshly-built board.
 *
 * Exists because of a real multi-hour debugging session (2026-09-26) where
 * two different boards each had a *different* single fault, and both looked
 * identical from the user's chair ("no dial tone"):
 *
 *   - One board's SLIC had a dead 2-wire/4-wire audio hybrid: hook detection
 *     and DC feed were perfect, but no audio ever reached tip/ring.
 *   - The other board had a bad solder joint on the J5 SHK jumper: audio was
 *     perfect, but hook detection never fired.
 *
 * The lesson that shapes this whole component: **the hook path and the audio
 * path fail independently, so they must be tested independently.** A single
 * "does line 0 work?" check conflates them and misdiagnoses both. Each line
 * therefore gets a hook check that never depends on audio, and an audio
 * check that never depends on hook detection.
 *
 * A third failure that day was pure misconfiguration (a 2-line board set to
 * "1 line + microSD", so line 1 was never initialized at all while the SD
 * driver drove its SLIC control pins as SPI) -- which is why the config-vs-
 * physical-evidence check runs first and why board variant has to be settled
 * before anything else means much.
 *
 * Results are stored on the board itself (NVS), not on whoever ran the test,
 * so "this passed on 2026-09-26 and fails now" is answerable from the board
 * alone months later. */
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SELFTEST_PENDING = 0, /* not judged yet (a human step nobody answered) */
    SELFTEST_PASS,
    SELFTEST_FAIL,
    SELFTEST_SKIP,        /* not applicable to this board variant */
} selftest_result_t;

typedef enum {
    /* Firmware decides alone -- no human involvement. */
    SELFTEST_AUTO,
    /* Human does something physical; **firmware verifies the result**, so the
     * outcome is objective rather than a self-report. Lifting a handset and
     * having firmware watch SHK actually change is the motivating case. */
    SELFTEST_ACTION,
    /* Only a human can judge it (did you hear dial tone / did the bell
     * ring). Firmware produces the stimulus where one is needed, but the
     * verdict is the person's. */
    SELFTEST_CONFIRM,
} selftest_kind_t;

#define SELFTEST_ID_MAX     16
#define SELFTEST_DETAIL_MAX 72
#define SELFTEST_MAX_CHECKS 16

typedef struct {
    char              id[SELFTEST_ID_MAX];   /* stable key, e.g. "hook0" */
    const char       *title;                 /* short human label */
    const char       *instruction;           /* what to do, for ACTION/CONFIRM */
    selftest_kind_t   kind;
    selftest_result_t result;
    char              detail[SELFTEST_DETAIL_MAX]; /* measured value, or why it failed */
} selftest_check_t;

typedef struct {
    int             n;
    selftest_check_t checks[SELFTEST_MAX_CHECKS];
} selftest_report_t;

/* Builds the check list for THIS board (line-1 entries come back SKIP on a
 * 1-line board) and runs every SELFTEST_AUTO check immediately. ACTION and
 * CONFIRM checks come back PENDING for the caller to walk through.
 *
 * Safe to call repeatedly; it resets the in-progress report each time. */
void selftest_begin(void);

/* Borrows the in-progress report, including any human answers recorded so
 * far. Deliberately a pointer to internal state rather than a by-value copy:
 * selftest_report_t is ~1.2 KB, and copying it onto the console REPL task's
 * 4 KB stack overflowed it outright (confirmed on hardware 2026-09-26 --
 * "Stack protection fault" in task console_repl). Valid until the next
 * selftest_begin(); not thread-safe against a concurrent run, which is fine
 * for a human-driven bring-up walkthrough. */
const selftest_report_t *selftest_report(void);

/* Runs one SELFTEST_ACTION check: waits up to timeout_ms for the physical
 * event it describes (currently: a hook-state change on that line, read
 * straight from the SLIC's SHK pin, deliberately independent of the audio
 * path and of telephony's own state machine). Records PASS/FAIL in the
 * report and returns the same.
 *
 * Blocks for up to timeout_ms -- callers on the httpd worker should keep
 * that within the socket timeout. */
selftest_result_t selftest_run_action(const char *id, int timeout_ms);

/* Produces the stimulus a SELFTEST_CONFIRM check needs a human to judge --
 * e.g. ringing the line for a few seconds. No-op for checks whose stimulus
 * is the human's own action (lifting a handset to hear dial tone). */
esp_err_t selftest_stimulus(const char *id);

/* Same two steps on a background task, returning immediately -- the web UI
 * has a single httpd worker, so blocking it for 15s hung every page.
 * ESP_ERR_INVALID_STATE if one is already running. */
esp_err_t selftest_run_action_async(const char *id, int timeout_ms);
esp_err_t selftest_stimulus_async(const char *id);
bool selftest_busy(void);

/* Everything the audio-level check measured on one line, newest run: one point
 * per level it tried (the current level first, then the search), for the
 * Diagnostics page's detail table. */
#define SELFTEST_LEVEL_MAX_POINTS 14
typedef struct {
    uint8_t level;
    float   distortion_pct, tone_dbfs, peak_dbfs, worst_hz, worst_dbfs;
    uint32_t clipped;
} selftest_level_point_t;
typedef struct {
    bool    valid;
    uint8_t saved_level;     /* level in use when the check started */
    uint8_t recommended;     /* level to suggest (0 = no recommendation) */
    uint8_t clean_max;       /* highest clean level found, 0 = none */
    int     n;
    selftest_level_point_t pt[SELFTEST_LEVEL_MAX_POINTS];
} selftest_level_report_t;
const selftest_level_report_t *selftest_level_report(int line);

/* Records a human verdict for an ACTION or CONFIRM check. */
esp_err_t selftest_answer(const char *id, bool ok);

/* ---- persisted run history -------------------------------------------- */

#define SELFTEST_HISTORY_MAX 8

typedef struct {
    int64_t unix_time;  /* 0 if the clock had never synced when it ran */
    uint8_t n_pass, n_fail, n_skip, n_pending;
} selftest_run_t;

/* Appends the current report to this board's stored history (oldest entry
 * drops once SELFTEST_HISTORY_MAX is reached). Call when a run is finished
 * -- the point is being able to say "this board passed before" later. */
esp_err_t selftest_history_append(void);

/* Newest first. Returns how many entries were written. */
size_t selftest_history_get(selftest_run_t *out, size_t max);

/* This board's unique identity (ESP32 base MAC from eFuse), formatted as
 * "AABBCCDDEEFF". Stable across reflashes and NVS erases, unlike the board
 * name -- the right key for "which physical board is this". */
void selftest_board_id(char *out, size_t len);

#ifdef __cplusplus
}
#endif

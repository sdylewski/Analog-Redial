/*
 * telephony.h — per-line call state machine.
 *
 * Consumes AG1170 hook/pulse events and DTMF digits, drives call-progress
 * tones, ringing, local inter-line bridging, and (via the voip seam) SIP calls.
 *
 * Start it after audio_init(), wm8731_init(), ag1170_init(), dialplan_init(),
 * voip_init(). telephony_start() registers the AG1170 callback and DTMF taps
 * and spawns the FSM task.
 */
#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TEL_IDLE = 0,   /* on-hook */
    TEL_DIALTONE,   /* off-hook, dial tone */
    TEL_DIALING,    /* collecting digits */
    TEL_RINGBACK,   /* we placed a call, hearing ringback */
    TEL_RINGING,    /* our phone is being rung (incoming) */
    TEL_INCALL,     /* connected (local bridge or VoIP) */
    TEL_BUSY,       /* busy tone */
    TEL_REORDER,    /* reorder / wrong-number tone */
    TEL_PLAYING,    /* playing a prompt/clip (operator help, fun clips, ...) */
    TEL_RECORDING,  /* capturing a message (211 archive, 212-215 callback) */
} tel_state_t;

esp_err_t telephony_start(void);

/* Inspection (for web UI / bring-up). */
tel_state_t telephony_state(int line);
const char *telephony_state_name(tel_state_t s);
const char *telephony_last_number(int line);
/* Most recently detected DTMF key on `line` ('0'-'9','*','#','A'-'D'), 0 if
 * none yet. *at_us is the esp_timer_get_time() timestamp of that detection
 * (undefined if the return value is 0). The detector runs continuously
 * (independent of dial state), so this reflects live touch-tone dialing --
 * see bringup's `tonemon`. */
char telephony_last_dtmf(int line, int64_t *at_us);

/* Bring-up escape hatch: cancel any pending transient-tone auto-return and
 * immediately drop the line to dial tone (if off-hook) or idle (if
 * on-hook), instead of waiting out a reorder/confirm tone's timer. */
void telephony_force_recover(int line);

/* Bring-up: while true, completed digits (pulse or DTMF) are dropped before
 * they reach the dial plan -- no dialplan_feed(), no tone/state change. For
 * pulsemon/tonemon, which read the raw pulse counter / DTMF detector
 * directly and are otherwise "non-invasive": without this, the phone still
 * treats what you dial as a real number underneath the monitor (wrong
 * number, hangup, etc.), which is confusing during a timing test. Off by
 * default; bringup toggles it around a monitor command's run. */
void telephony_set_monitor_suppress(bool on);

/* ---- Test/verification hooks (host-driven DVT harness) ----
 * Injection is gated behind test mode so it can never fire in normal
 * operation. Lets an automated harness simulate a handset: off-hook,
 * hang-up, and rotary/DTMF digits, without physically dialing. In test mode
 * the injected hook state also shadows the real SLIC, so the FSM behaves
 * exactly as it would with a real handset (e.g. returning to dial tone
 * after a clip finishes only while "off-hook"). */
void      telephony_test_set_enabled(bool on);
bool      telephony_test_enabled(void);
/* ESP_ERR_INVALID_STATE if test mode is off; ESP_ERR_INVALID_ARG for a bad
 * line or (for digit) a non-'0'-'9' character. */
esp_err_t telephony_test_hook(int line, bool offhook);
esp_err_t telephony_test_digit(int line, char digit);

#ifdef __cplusplus
}
#endif

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

#ifdef __cplusplus
}
#endif

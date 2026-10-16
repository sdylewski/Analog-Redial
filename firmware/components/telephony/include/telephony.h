/*
 * telephony.h — per-line call state machine.
 *
 * Consumes AG1170 hook/pulse events and DTMF digits, drives call-progress
 * tones, ringing, local inter-line bridging, and (via the voip seam) SIP calls.
 *
 * Start it after audio_init(), es8388_init(), ag1170_init(), dialplan_init(),
 * voip_init(). telephony_start() registers the AG1170 callback and DTMF taps
 * and spawns the FSM task.
 */
#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

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
    TEL_RECORDING,  /* capturing audio -- 211 archive, a 212 greeting
                     * recording, or an answering machine's caller message;
                     * see telephony.c's rec_purpose_t (private) for which */
    TEL_OFFHOOK_ALERT, /* left off-hook on dial tone too long -- loud ROH tone
                        * until the handset goes back on-hook (board.h's
                        * board_offhook_alert_get() configures the delay) */
    TEL_PLAYING_RECORD_PROMPT, /* playing a "leave a message"-style clip
                                * before the beep -- 211's RECORD_PROMPT_NAME,
                                * or an answering machine's own greeting -- a
                                * distinct state from TEL_PLAYING so
                                * IN_CLIP_DONE knows to continue into the beep
                                * rather than just returning to dial tone */
    TEL_RECORD_BEEP,           /* beep tone playing, recording starts
                                * when it finishes */
    TEL_BT_PASSTHROUGH,        /* off-hook while the Bluetooth companion
                                * (bt_companion.c) owns this line's audio --
                                * silent, no dial tone, no digit collection,
                                * no inter-digit timeout. See on_offhook()'s
                                * bt_companion_enabled() check: this line's
                                * Vin/Vout are physically shared with the
                                * companion's GPIO25/34, so normal telephony
                                * tones would electrically fight the
                                * Bluetooth audio on the same node. */
} tel_state_t;

/* 211's optional spoken prompt ("record your message after the beep...").
 * Well-known name, like operator.wav -- prompts.h tracks the desired text
 * for it under this exact filename, gen_prompt.py --sync generates and
 * uploads it here, and telephony.c plays it (if present) before the beep.
 * No prompt configured is a normal, silent-but-working state: falls
 * straight through to the beep. */
#define RECORD_PROMPT_NAME "record_prompt.wav"

/* Answering machine's own greeting for `line`, recorded via dial code 999
 * (fixed filename -- re-recording overwrites it directly, reviewed via the
 * web UI's playback, not a phone-side confirm loop). Not present is a
 * normal, working state: auto_answer() (telephony.c) falls straight through
 * to the beep, same fallback as 211's own missing-prompt case. */
/* Shared default answering-machine greeting, used by any line without its
 * own recording. Built into the firmware (firmware/assets/, "Leave a
 * message after the beep.") and overridden by a same-named upload -- e.g.
 * text-to-speech generated from the web UI's greeting text. */
#define AM_DEFAULT_GREETING_NAME "am_greeting.wav"

void telephony_am_greeting_name(char *out, size_t len, int line);

esp_err_t telephony_start(void);
/* Warns (log) about any dial code whose audio file is missing. Call once storage is up. */
void telephony_check_clip_files(void);
/* Warns (log) about any dial code whose audio file is missing. Call once storage is up. */
void telephony_check_clip_files(void);

/* Inspection (for web UI / bring-up). */
tel_state_t telephony_state(int line);
const char *telephony_state_name(tel_state_t s);
const char *telephony_last_number(int line);
/* Caller ID of the most recent inbound VoIP call on `line` (kept after that
 * call ends, so "who last called" survives hangup). Name is "" when the
 * caller sent only a bare SIP URI -- display the number in that case. */
const char *telephony_caller_name(int line);
const char *telephony_caller_number(int line);
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

/* Rings a line's bell for `secs` (1-15) seconds, like a doorbell: for the
 * web UI's test ring and Home Assistant. Refused (ESP_ERR_INVALID_STATE) if
 * the line isn't idle and on-hook, and, when respect_quiet is set, during
 * quiet hours (ESP_ERR_NOT_ALLOWED). Each line has its own timer, and the
 * timer leaves the bell alone if a real call has started ringing meanwhile.
 * Lifting the handset stops the bell at once (the SLIC's ring trip). */
esp_err_t telephony_bell(int line, int secs, bool respect_quiet);

/* Messages (dial 211 and answering-machine recordings) saved since someone
 * last looked at the Recordings page or called telephony_messages_seen().
 * Kept across restarts. */
int  telephony_new_messages(void);
void telephony_messages_seen(void);

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

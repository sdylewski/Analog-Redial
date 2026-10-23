/* bt401.h — DFRobot DFR0781 / BT401 Bluetooth module backend for bt_companion.
 *
 * The BT401 sits on the same 8-pin J2 header as the BM64 (power, UART on
 * GPIO16/17, and the analog mic/speaker pair). bt_companion.c dispatches to
 * this file when Settings -> Hardware says a BT401 is fitted. The protocol
 * (plain text, 115200 baud) is in bt401_proto.h; wiring and what still needs
 * checking on a real module: archive/hardware/bt401.md.
 *
 * Differences from the BM64 backend that callers can see:
 *   - no EEPROM configuration step and no MFB handshake (the module powers
 *     itself up; GPIO15 is left alone);
 *   - it doesn't report the connected phone's name.
 *
 * Not used from application code directly -- go through bt_companion.h. The
 * bring-up functions at the bottom are for the console commands in bringup.c.
 */
#pragma once

#include "bt_companion.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Called from the module's task when the call state, or the caller ID of a
 * ringing call, changes. Also with BT_CALL_IDLE if the link drops mid-call. */
typedef void (*bt401_notify_cb)(bt_call_state_t state);

esp_err_t bt401_start(bt401_notify_cb cb);
/* disconnect = ask the module to drop the phone first (feature switched off,
 * so the module stops driving the audio path). */
void      bt401_stop(bool disconnect);
bool      bt401_running(void);

void      bt401_get_status(bt_companion_status_t *out);

esp_err_t bt401_accept_call(void);
esp_err_t bt401_reject_call(void);
esp_err_t bt401_end_call(void);
esp_err_t bt401_start_pairing(void);
/* Places a call on the connected phone. ESP_ERR_INVALID_STATE if there is no
 * phone or a call is already under way; ESP_ERR_INVALID_ARG for a bad number. */
esp_err_t bt401_dial(const char *number);
bool      bt401_call_live(void);
bool      bt401_music_playing(void);

/* ---- bring-up (console commands) ---------------------------------------- */

/* Sends one text command ("BA04", "TS", ...) as AT+<cmd>\r\n. With the task
 * running the reply shows up in its log and in bt401_print_state(); without
 * it, the UART is owned for listen_ms and every received line is printed. */
esp_err_t bt401_send_at(const char *cmd, int listen_ms);

/* Is anything there? Listens for the power-up chatter (QA+/QM+), then asks for
 * the status, name and address, and says what came back. Needs the task
 * stopped (ESP_ERR_INVALID_STATE otherwise). */
esp_err_t bt401_probe(void);

/* Prints what the task has decoded so far, plus parser counters. */
void      bt401_print_state(void);

#ifdef __cplusplus
}
#endif

/* bk3266.h — BK3266SR Bluetooth module backend for bt_companion.
 *
 * The BK3266 is the cheap alternative to the BM64 on the same 8-pin J2
 * header (power, UART, WAKEUP on GPIO15, and the analog mic/speaker pair).
 * bt_companion.c dispatches to this file when Settings -> Hardware says a
 * BK3266 is fitted. Protocol details live in bk3266_proto.h; wiring, what's
 * verified and what isn't: archive/hardware/bk3266.md.
 *
 * Differences from the BM64 backend that callers can see:
 *   - no dial-out (the module has no "dial this number" command), so
 *     bt_companion_can_dial() is false;
 *   - no EEPROM configuration step;
 *   - 9600 baud, and no MFB handshake: WAKEUP is left alone in normal use.
 *
 * Not used from application code directly -- go through bt_companion.h.
 * The bring-up functions at the bottom are for the console commands in
 * bringup.c and refuse to run while the background task owns the UART.
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
typedef void (*bk3266_notify_cb)(bt_call_state_t state);

esp_err_t bk3266_start(bk3266_notify_cb cb);
/* disconnect = ask the module to drop the phone first (feature switched off,
 * so the module stops driving the audio path). */
void      bk3266_stop(bool disconnect);
bool      bk3266_running(void);

void      bk3266_get_status(bt_companion_status_t *out);

esp_err_t bk3266_accept_call(void);
esp_err_t bk3266_reject_call(void);
esp_err_t bk3266_end_call(void);
esp_err_t bk3266_start_pairing(void);
bool      bk3266_call_live(void);

/* ---- bring-up (console commands) ---------------------------------------- */

/* Sends one MCU->module command code (BK_CMD_*) in a normal frame. Works
 * with the task running -- the reply shows up in its log and in
 * bk3266_print_state(). */
esp_err_t bk3266_send_code(uint8_t code);

/* Prints what the task has decoded so far, plus parser counters. */
void      bk3266_print_state(void);

/* The rest need the task NOT running (ESP_ERR_INVALID_STATE otherwise),
 * except the two send functions, which just transmit when it is running:
 * they own the UART for their duration and print every byte they see. */

/* Listens for listen_ms, printing raw bytes and decoded frames. */
esp_err_t bk3266_listen(int listen_ms);
/* Version query (binary and text) at every common baud rate: what answers? */
esp_err_t bk3266_baudscan(void);
/* Sends raw bytes (any framing, or text), then listens. With the driver
 * running it only sends: the reply shows up in its log and bk_state. */
esp_err_t bk3266_send_raw(const uint8_t *tx, size_t n, int listen_ms);
/* Sends a well-formed frame with the given outer CMD and DATA, then listens. */
esp_err_t bk3266_send_framed(uint8_t cmd, const uint8_t *data, size_t n, int listen_ms);
/* Is anything there? Listens for boot chatter, then sends a harmless version
 * query in the framed forms the spreadsheet could mean, and says what came
 * back (nothing / text / binary frames). */
esp_err_t bk3266_probe(void);

/* Drives WAKEUP (GPIO15) high for ms, then releases it. Only on boards where
 * that pin is dedicated (SMD5). ESP_ERR_NOT_SUPPORTED elsewhere. */
esp_err_t bk3266_wake_pulse(int ms);

#ifdef __cplusplus
}
#endif

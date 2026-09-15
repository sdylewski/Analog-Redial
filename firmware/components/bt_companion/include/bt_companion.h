/* bt_companion.h — link to an optional Bluetooth Classic audio companion
 * board (BM64, or a second ESP32 running Bluedroid) over UART on
 * BOARD_UART_TX/BOARD_UART_RX (GPIO16/17 — reserved in every board's pin
 * map, never otherwise used). See archive/BLUETOOTH_AUDIO_UPGRADE.md for
 * why this exists: the ESP32-C6 on this board has BLE only, no Classic BT,
 * so A2DP/HFP need a separate chip; this component is the C6 side of that
 * link. Audio itself doesn't come through here at all — it's wired in the
 * analog domain directly between the companion and the existing codec/
 * handset paths (also per that doc), so this is control/status only:
 * pairing, connection state, and (eventually) ring/answer/hangup events.
 *
 * No hardware exists yet as of this writing — this defines the NVS-backed
 * "do we have this daughter board" switch and the protocol/plumbing it'll
 * use once one is wired in, so the web UI has something real to show
 * (honestly reporting "not connected") rather than nothing at all. */
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BT_COMPANION_NAME_MAX 32

typedef struct {
    bool    enabled;                          /* "I have this daughter board wired in" */
    bool    link_up;                          /* UART link to the companion itself alive */
    bool    bt_connected;                     /* companion reports a paired phone/laptop */
    char    paired_name[BT_COMPANION_NAME_MAX]; /* "" if bt_connected is false */
} bt_companion_status_t;

/* Starts the UART + background poll task if (and only if) enabled -- see
 * bt_companion_set_enabled(). Call once at boot, after NVS is up (same
 * ordering as board_timezone_init()). Safe to call even if never enabled;
 * does nothing in that case. */
void bt_companion_init(void);

/* Defaults to disabled -- there's no companion board on any board that's
 * shipped so far, so this must be an opt-in, not a default-on guess. */
bool        bt_companion_enabled(void);
/* Starts or stops the UART link immediately (no reboot needed, unlike most
 * of this project's other feature toggles) -- turning this on/off doesn't
 * touch telephony or VoIP, so there's no shared state that needs a clean
 * restart to reset. */
esp_err_t   bt_companion_set_enabled(bool enabled);

/* Current link/pairing status. link_up/bt_connected/paired_name are all
 * runtime-only (not persisted) -- they reflect whatever the companion last
 * reported, or all false/"" if disabled or nothing's answered yet. */
bt_companion_status_t bt_companion_get_status(void);

/* Sends the companion a "start pairing" command. ESP_ERR_INVALID_STATE if
 * not enabled or the UART link isn't up (nothing to send it to). */
esp_err_t bt_companion_start_pairing(void);

/* Diagnostic: asks the companion to synthesize a tone and drive it straight
 * out its own DAC (downlink), bypassing Bluetooth/the phone call entirely
 * -- see bt_audio_io_test_tone() on that side. Useful for isolating whether
 * the analog bridge itself is clean, independent of live call audio.
 * ESP_ERR_INVALID_STATE if not enabled or the UART link isn't up. */
esp_err_t bt_companion_test_tone(int hz, int ms);

/* Diagnostic: asks the companion to drive a constant DC level out its DAC
 * (bypassing Bluetooth), instead of a tone -- see bt_audio_io_test_level()
 * on that side. Easier to confirm with a multimeter/scope than a moving
 * waveform. ESP_ERR_INVALID_STATE if not enabled or the UART link isn't up. */
esp_err_t bt_companion_test_level(bool high, int ms);

#ifdef __cplusplus
}
#endif

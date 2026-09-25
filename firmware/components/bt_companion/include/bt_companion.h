/* bt_companion.h — link to a BM64 Bluetooth Classic audio module over UART
 * on BOARD_UART_TX/BOARD_UART_RX (GPIO16/17 — reserved in every board's pin
 * map, never otherwise used). See archive/BLUETOOTH_AUDIO_UPGRADE.md for why
 * this exists: the ESP32-C6 on this board has BLE only, no Classic BT, so
 * A2DP/HFP need a separate chip. Audio itself doesn't come through here at
 * all — it's wired in the analog domain directly between BM64 and the
 * existing codec/handset paths (also per that doc), so this is
 * control/status only: pairing and connection state.
 *
 * Speaks BM64's real serial command protocol — a binary framed format
 * (sync byte, length, opcode/event code, parameters, checksum), NOT a
 * custom text protocol — see bt_companion.c's own doc comment for the full
 * frame layout. Confirmed against Microchip's actual "Audio UART Command
 * Set" document and BM62/64 datasheet (datasheets/ in this repo) as of
 * 2026-09-22 — cross-checked against a working open-source Arduino library
 * too, konikoni428/BM64_arduino, vendored locally at reference/BM64_arduino.
 *
 * REWRITTEN 2026-09-21, confirmed working end-to-end 2026-09-22: an earlier
 * version of this file spoke an invented ASCII line protocol ("STATUS?\n",
 * "PAIR\n") designed for a since-abandoned plan (a second ESP32 running our
 * own Bluedroid code as the companion, see
 * archive/BLUETOOTH_AUDIO_UPGRADE.md's "Option B") — a real BM64 module
 * never understood any of that. This version does, checksums and all, and
 * pairing has been confirmed with a real phone (see that doc's 2026-09-22
 * entry for the full story, including the POWERON_BUTTON_PRESS/RELEASE
 * handshake the module turned out to need first). */
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
    bool    link_up;                          /* we've seen at least one valid framed
                                                * reply/event from the module recently --
                                                * proves the UART link itself is alive */
    bool    bt_connected;                     /* true once an HF or A2DP link is up
                                                * (BM64_EVENT_DEVICE_STATE, confirmed
                                                * against Microchip's real command-set
                                                * doc 2026-09-22 -- see bt_companion.c) */
    char    paired_name[BT_COMPANION_NAME_MAX]; /* "" if bt_connected is false or unknown */
    char    bd_addr[18];                      /* this module's own BT MAC, "" until the
                                                * background poller's one-time startup query
                                                * gets a reply -- shown in the web UI so you
                                                * know which device to pick on a phone */
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
 * runtime-only (not persisted) -- they reflect whatever the module last
 * reported, or all false/"" if disabled or nothing's answered yet. */
bt_companion_status_t bt_companion_get_status(void);

/* Sends the module a "start pairing" MMI command (BM64_MMI_ANY_MODE_ENTERING_
 * PAIRING). ESP_ERR_INVALID_STATE if not enabled or the UART link isn't up
 * (nothing to send it to). */
esp_err_t bt_companion_start_pairing(void);

/* Bring-up/diagnostic only (see bringup.c's 'bm64_mmi' console command):
 * sends BM64_MMI_CMD with an arbitrary MMI action code, same
 * link-must-be-up precondition as bt_companion_start_pairing() (which is
 * really just this, hardcoded to the pairing action). For testing MMI
 * codes the reference library sends as part of its own powerOn() sequence
 * (0x51 POWERON_BUTTON_PRESS, 0x52 POWERON_BUTTON_RELEASE) that this
 * component doesn't send anywhere yet -- see reference/BM64_arduino. */
esp_err_t bt_companion_send_mmi(uint8_t action);

/* Bring-up/diagnostic only, same link-must-be-up precondition: sends any
 * simple "opcode + one reserved byte" command (see bt_companion.c's own
 * doc comment -- this covers Read_Paired_Device_Record (0x0E),
 * Read_Local_Device_Name (0x10), etc., anything not already wrapped by a
 * dedicated function above). Reply shows up via the console log at DEBUG
 * level (handle_event()'s hex-dumped "unhandled event" case) -- this
 * deliberately doesn't parse the specific reply itself. */
esp_err_t bt_companion_send_raw_cmd(uint8_t opcode, uint8_t reserved_byte);

/* Bring-up/diagnostic only (see bringup.c's 'bm64_test' console command),
 * NOT wired to the web UI: sends GET_UART_VERSION (a harmless, always-
 * supported query -- every real BM64 answers it, paired or not, which is
 * exactly why it's the right thing to send when the only question is "is
 * anything even listening on this UART") and waits up to timeout_ms for
 * any single valid framed reply, dumping it via ESP_LOGI as it decodes.
 * Returns ESP_OK the instant a valid frame arrives (regardless of its
 * content -- this only proves the link itself is alive), ESP_ERR_TIMEOUT
 * if nothing valid arrived in time, ESP_ERR_INVALID_STATE if the background
 * poll task is already running (bt_companion_set_enabled(true) already
 * called) -- the two would fight over the same UART RX bytes, so this is
 * deliberately a "test it before you turn the real feature on" tool, not
 * something safe to run alongside it. */
esp_err_t bt_companion_raw_ping(int timeout_ms);

/* Same as bt_companion_raw_ping(), but overrides the UART baud rate for just
 * this one probe (baud <= 0 means "use the normal 115200 default", identical
 * to plain bt_companion_raw_ping()) -- restores the default baud rate before
 * returning either way. For bring-up only: sweeping common BM64 baud rates
 * (9600/19200/38400/57600/115200) is the fastest way to tell a wrong-baud
 * module apart from a real wiring/power problem without re-flashing. */
esp_err_t bt_companion_raw_ping_at_baud(int timeout_ms, int baud);

/* Bring-up/diagnostic only, same "can't share the UART with the background
 * poller" rule as bt_companion_raw_ping(): reads the module's own Bluetooth
 * MAC/BD address and formats it into out ("XX:XX:XX:XX:XX:XX", out_len
 * should be at least 18). ESP_ERR_TIMEOUT if no address reply arrived in
 * time, ESP_ERR_INVALID_STATE if the background poll task is running. */
esp_err_t bt_companion_read_bd_addr(int timeout_ms, char *out, size_t out_len);

/* Bring-up/diagnostic only, same UART-exclusivity rule as the above: prints
 * every raw RX byte live, in hex, the instant it arrives, for duration_ms --
 * no framing/checksum validation at all (unlike every other command here).
 * For watching what happens on the wire in real time while physically
 * poking hardware (e.g. touching MFB) -- read_one_frame()-based commands
 * only report a fully valid frame after the fact, which hides exactly the
 * "did anything at all happen right when I touched that pin" moment this
 * is for. */
esp_err_t bt_companion_raw_snoop(int duration_ms);

/* Sends tx_len arbitrary raw bytes -- no AA00 framing, no checksum -- then
 * prints whatever comes back for listen_ms, live, same style as
 * bt_companion_raw_snoop(). For probing BM64's undocumented IBDK/test-mode
 * protocol (EEPROM read/write), which is NOT the normal AA00 command set.
 * Unlike every other raw_* function here, this does NOT touch MFB/RST_N/
 * P2_0 at all (see bt_companion.c's comment) -- IBDK mode needs P2_0 held
 * low, so set that up yourself first (bringup.c's bm64_pin command) before
 * calling this. */
esp_err_t bt_companion_raw_send(const uint8_t *tx, size_t tx_len, int listen_ms);

/* NOT implemented yet, deliberately: BM64_MMI_POWEROFF_BT (0x5E) exists in
 * bt_companion.c's opcode enum but nothing sends it. Don't wire this up
 * until MFB is on a controllable GPIO, not a fixed pull-up resistor --
 * powering the module off over UART with MFB hard-tied high leaves no way
 * to power it back on again short of physically disturbing the wiring. */

#ifdef __cplusplus
}
#endif

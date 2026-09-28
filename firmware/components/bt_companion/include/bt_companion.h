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
#define BT_COMPANION_CALLER_ID_MAX 24

/* HF call state as the module reports it (Call_Status event 0x02, Microchip
 * Audio UART Command Set 7.3). Values are the module's own. */
typedef enum {
    BT_CALL_IDLE           = 0,
    BT_CALL_VOICE_DIAL     = 1,
    BT_CALL_INCOMING       = 2,
    BT_CALL_OUTGOING       = 3,
    BT_CALL_ACTIVE         = 4,
    BT_CALL_ACTIVE_WAITING = 5, /* a call active with another waiting */
    BT_CALL_ACTIVE_HOLD    = 6, /* a call active with another on hold */
} bt_call_state_t;

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
    bt_call_state_t call_state;                  /* the paired phone's HF call state */
    char    caller_id[BT_COMPANION_CALLER_ID_MAX]; /* number from the Caller_ID event,
                                                * "" if none reported (yet) */
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

/* True only when the feature is switched on AND a module is actually fitted
 * per board_hw_config_get().has_bt_module. This is what behavior should gate
 * on -- bt_companion_enabled() alone is just the user's toggle, and acting on
 * it by itself is what let a stray enabled flag claim a phone line on a board
 * with no hardware attached, leaving no dial tone and no way to tell why
 * (2026-09-26). "Fitted" is a fact about the board, "enabled" is a choice. */
bool bt_companion_active(void);

/* Current link/pairing status. link_up/bt_connected/paired_name are all
 * runtime-only (not persisted) -- they reflect whatever the module last
 * reported, or all false/"" if disabled or nothing's answered yet. */
bt_companion_status_t bt_companion_get_status(void);

/* Notified from the Bluetooth task whenever the paired phone's call state
 * changes, or a caller ID arrives for a call already ringing (state
 * unchanged). Also fires with BT_CALL_IDLE when the link drops or the feature
 * is switched off mid-call, so a ringing bell can't be left ringing. Runs in
 * bt_companion's own task -- the listener should just queue the event. One
 * listener; a second registration replaces the first. */
typedef void (*bt_companion_call_cb)(bt_call_state_t state, void *ctx);
void bt_companion_set_call_cb(bt_companion_call_cb cb, void *ctx);

/* HF call control (MMI actions 0x04/0x05/0x06). With two devices connected
 * (e.g. a phone and a laptop), status.call_state is merged across both --
 * a ringing call wins, then a live one. ESP_ERR_INVALID_STATE if the
 * feature isn't on or the module isn't responding; whether the phone actually
 * acts on it comes back as a later call-state change, not as a return value.
 * End uses 0x06 ("end call / transfer audio to phone"), not 0x02 -- the
 * latter is marked deprecated in the v2.06 command set. */
/* Each goes to the right connected device: accept/reject to the one that's
 * ringing, end to the one with a live call. ESP_ERR_NOT_FOUND if no device
 * is in that state. */
esp_err_t bt_companion_accept_call(void);
esp_err_t bt_companion_reject_call(void);
esp_err_t bt_companion_end_call(void);

/* A call is outgoing or in progress on any connected device. Use this, not
 * the merged status.call_state, for "should hanging up end a call": with one
 * device mid-call and another ringing, the merged state reports the ringing
 * one. */
bool bt_companion_call_live(void);

/* Places an outgoing call on the paired phone (BM64 Make_Call): `number` is
 * ASCII digits, max 19. Picks the connected device with its HF profile up.
 * ESP_ERR_INVALID_STATE if disabled or no phone is connected. Progress comes
 * back through the call callback (OUTGOING, then ACTIVE or IDLE). */
esp_err_t bt_companion_make_call(const char *number);

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

/* Bring-up/diagnostic only, same link-must-be-up precondition: sends any
 * real command-set command (AA00-framed, checksummed -- NOT the raw IBDK
 * probing bt_companion_raw_send() does) with an arbitrary multi-byte
 * parameter list, for commands bt_companion_send_raw_cmd()'s fixed
 * "opcode + one byte" shape can't cover -- e.g. Set_Overall_Gain (0x23),
 * which takes 6 parameter bytes (Data_Base_Index, Mask, Type, Gain1-3; see
 * Microchip's Audio UART Command Set doc, BM64/Docs/, section 5.1.30).
 * Same reply-surfaces-via-the-log behavior as bt_companion_send_raw_cmd(). */
esp_err_t bt_companion_send_cmd(uint8_t opcode, const uint8_t *params, uint8_t param_count);

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

/* Like bt_companion_raw_send() but returns response bytes in a caller buffer
 * instead of printing them. Returns the number of bytes received (may exceed
 * rx_buf_len if the buffer was too small — only rx_buf_len bytes are stored).
 * Returns -1 on UART conflict (background poller running) or init failure. */
int bt_companion_raw_xfer(const uint8_t *tx, size_t tx_len,
                          uint8_t *rx_buf, size_t rx_buf_len,
                          int listen_ms);

/* ---- IBDK mode + EEPROM access ------------------------------------------
 *
 * IBDK ("in-board development kit") is the BM64's test/programming mode,
 * entered by holding P2_0 low across a reset -- so it needs real GPIO
 * control of MFB/RST_N/P2_0, and is only available on boards that define
 * BOARD_BT_MFB_PIN. In this mode the module speaks HCI, not the normal AA00
 * framed command set, and EEPROM access travels as HCI ACL Data packets to
 * connection handle 0x0FFF (reverse-engineered from a DMS capture of
 * Microchip's own isUpdate.exe -- see BM64/eeprom_write/eeprom_write.md).
 *
 * All of these stop the module talking Bluetooth for as long as IBDK mode
 * is held, and they can't share the UART with the background poll task, so
 * bt_companion must be disabled first (bt_companion_set_enabled(false)) --
 * they return ESP_ERR_INVALID_STATE otherwise. bt_companion_config_check()
 * below handles that sequencing itself; these raw primitives don't. */

/* toggle_pins=false skips the P2_0/RST_N/MFB dance and only re-runs the HCI
 * init handshake, for when the module is already sitting in IBDK mode (or
 * the pins are being driven by hand). */
esp_err_t bt_companion_ibdk_enter(bool toggle_pins);
/* Resets the module back into normal Flash Application mode. Leaves
 * bt_companion itself disabled -- re-enable separately. */
esp_err_t bt_companion_ibdk_exit(void);
bool      bt_companion_ibdk_ready(void);

/* len 1-256. Requires bt_companion_ibdk_enter() to have succeeded. */
esp_err_t bt_companion_eeprom_read(uint16_t addr, uint8_t *out, size_t len);
esp_err_t bt_companion_eeprom_write(uint16_t addr, uint8_t value);

/* ---- Known-good EEPROM configuration check ------------------------------
 *
 * A stock BM64 ships configured for the EVB it was characterized on, which
 * routes audio out over I2S to an external DAC -- not to the analog
 * AOHPL/AOHPR pins this board taps. Getting analog audio needs a handful of
 * EEPROM bytes changed (see BM64/BM64_AUDIO_DEBUG.md for how each one was
 * identified, and what the wrong value does).
 *
 * This deliberately checks a short curated list of bytes whose meaning is
 * actually understood, rather than diffing the whole EEPROM against
 * Microchip's reference table: the module's own BD address and device name
 * live in that same EEPROM, so bulk-writing it to match a reference image
 * would destroy the module's identity. */
#define BM64_CONFIG_ITEM_MAX 8

typedef struct {
    uint16_t    addr;
    uint8_t     expected;
    uint8_t     found;
    bool        matched;   /* found == expected, as read before any write */
    bool        written;   /* this run wrote it (apply mode, and it differed) */
    const char *what;      /* short human label, not translated */
} bm64_config_item_t;

typedef struct {
    bool ok;             /* every item matches now (after writes, in apply mode) */
    int  n_items;
    int  n_mismatched;   /* items that differed when read */
    int  n_written;      /* items actually written (0 unless apply) */
    char bd_addr[18];    /* module's BD address, "" if it couldn't be read */
    char error[96];      /* "" unless something failed outright */
    bm64_config_item_t items[BM64_CONFIG_ITEM_MAX];
} bm64_config_report_t;

/* Reads every byte in the table, compares, and (if apply) writes just the
 * ones that differ, then verifies each write by reading it back.
 *
 * Handles the whole disruptive sequence itself: stops the poll task, reads
 * the module's BD address, enters IBDK mode, does the work, returns the
 * module to normal mode, and restores bt_companion to whatever enabled
 * state it started in. Takes roughly 10-15 seconds, during which Bluetooth
 * is down and any paired phone disconnects.
 *
 * On a successful apply where everything ends up matching, records this
 * module's BD address in NVS so bt_companion_config_is_verified() can skip
 * re-checking it -- keyed by BD address specifically so swapping the BM64
 * module, or moving this ESP32 to another board, re-triggers the check
 * instead of trusting a stale "already done" flag.
 *
 * `out` may be NULL. ESP_ERR_NOT_SUPPORTED on boards without
 * BOARD_BT_MFB_PIN (no GPIO control of the mode pins -> no IBDK). */
esp_err_t bt_companion_config_check(bool apply, bm64_config_report_t *out);

/* True if the currently-connected module's BD address matches the one
 * recorded by a successful bt_companion_config_check(true, ...). False if
 * nothing is recorded, the addresses differ, or the module's own address
 * isn't known yet (link still coming up). */
bool bt_companion_config_is_verified(void);

/* Most recent report from bt_companion_config_check(), for rendering after
 * a redirect. False if none has run since boot. */
bool bt_companion_config_last_report(bm64_config_report_t *out);

/* NOT implemented yet, deliberately: BM64_MMI_POWEROFF_BT (0x5E) exists in
 * bt_companion.c's opcode enum but nothing sends it. Don't wire this up
 * until MFB is on a controllable GPIO, not a fixed pull-up resistor --
 * powering the module off over UART with MFB hard-tied high leaves no way
 * to power it back on again short of physically disturbing the wiring. */

#ifdef __cplusplus
}
#endif

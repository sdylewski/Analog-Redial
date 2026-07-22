/* board.h — board-level init helpers shared across components. */
#pragma once

#include "esp_err.h"
#include "driver/i2c_master.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize the shared I2C master bus (codec control). Idempotent. */
esp_err_t board_i2c_init(void);

/* Handle to the shared I2C master bus, or NULL if not initialized. */
i2c_master_bus_handle_t board_i2c_bus(void);

/* Probes the I2C bus for both known codec addresses (ES8388 @ 0x10, WM8731 @
 * 0x1A) and logs a warning if neither ACKs, or if the codec that answered
 * doesn't match the board type this firmware was built for (CONFIG_BOARD_TYPE_*).
 * This is a sanity check, not a substitute for selecting the right Kconfig
 * board type at build time — GPIO pin routing is fixed per PCB and can't be
 * switched at runtime. Safe to call before or after board_i2c_init(). */
void board_detect_codec(void);

typedef enum {
    BOARD_LED_BOOTING,         /* fast blink -- subsystems still coming up */
    BOARD_LED_OK,               /* solid off -- everything initialized cleanly */
    BOARD_LED_ERROR,            /* slow blink -- a subsystem failed to init but
                                  * the device kept running (see app_main.c) */
    BOARD_LED_WIFI_CONNECTING,  /* slow PWM breathe -- WiFi connect in progress */
} board_led_state_t;

/* Configures the status LED (SMD only, GPIO11 -- no-op on THT, which has no
 * LED) and starts it blinking fast (BOARD_LED_BOOTING). Call once, early in
 * app_main(), before other subsystem init so there's visible boot feedback
 * even if something later hangs. */
void board_led_init(void);

/* Change the blink pattern. Safe to call from any task. */
void board_led_set_state(board_led_state_t state);

/* Starts a background task that watches the BOOT strap (GPIO9). Held low for
 * ~3 s *during normal runtime* (NOT at power-on -- low at reset just enters
 * the serial bootloader), it calls on_hold() once, meant for a WiFi factory
 * reset (erase creds + reboot). Reading the strap as an input after boot is
 * safe. Pass NULL to disable. */
void board_factory_reset_start(void (*on_hold)(void));

/* ---- Board identity ----
 * Lets several boards on one LAN be told apart in the web UI (see the
 * Settings page). Three layers:
 *   - serial: the factory-unique ESP32-C6 MAC, never changes, not settable.
 *   - hardware: chip + codec + PCB variant, detected, not settable.
 *   - name: a human label you set ("Scott's Room"); persists in NVS and,
 *     sanitized, becomes the mDNS <name>.local hostname. */

typedef enum {
    BOARD_CODEC_NONE = 0,  /* nothing ACKed on the I2C bus yet / at all */
    BOARD_CODEC_ES8388,    /* 0x10  -> SMD board */
    BOARD_CODEC_WM8731,    /* 0x1A  -> through-hole board */
    BOARD_CODEC_CONFLICT,  /* both answered -- bus contention (see detect log) */
} board_codec_t;

/* Result of the last board_detect_codec() probe (BOARD_CODEC_NONE before it
 * has run). Reflects what is physically on the bus, independent of the
 * CONFIG_BOARD_TYPE_* this firmware was built for. */
board_codec_t board_detected_codec(void);
const char   *board_codec_name(board_codec_t c);

/* Factory-unique chip id (the WiFi-station MAC) as "AABBCCDDEEFF". Stable
 * across reflashes and NVS erases -- the closest thing to a serial number. */
void board_serial_str(char *out, size_t len);

/* One-line hardware description, e.g. "ESP32-C6 rev1.0 · ES8388 (SMD2)".
 * Combines chip model/revision, the detected codec, and the built-for PCB. */
void board_hardware_str(char *out, size_t len);

/* Persisted, user-facing board name (NVS). Defaults to CONFIG_MDNS_HOSTNAME
 * on first boot / if unset. board_name_set() rejects empty names. */
void      board_name_get(char *out, size_t len);
esp_err_t board_name_set(const char *name);

/* True only if a name has actually been stored (user- or auto-assigned),
 * false when board_name_get() is still falling back to the default. Lets the
 * mDNS collision logic auto-rename only an un-personalized board. */
bool board_name_is_set(void);

/* mDNS label derived from the board name: lowercased, apostrophes dropped,
 * other non [a-z0-9] folded to a single '-', so "Scott's Room" ->
 * "scotts-room" (reachable as scotts-room.local). wifi_apply_hostname()
 * feeds this to mDNS. */
void board_hostname(char *out, size_t len);

/* ---- Phone language ----
 * One language for the whole phone, set once during configuration -- NOT
 * switchable in use (that was the Pi's 901/902 behavior; dropped here as
 * more complexity than it's worth). Whatever this is set to is what
 * setup-time prompt/number-audio generation (gen_prompt.py et al.) should
 * target; nothing in the dial plan changes it. A short code, e.g. "en",
 * "it" -- not validated against a fixed list, so new languages don't need a
 * firmware change, just matching audio assets. */
void      board_language_get(char *out, size_t len); /* defaults to "en" */
esp_err_t board_language_set(const char *lang);

#ifdef __cplusplus
}
#endif

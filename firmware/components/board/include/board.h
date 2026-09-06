/* board.h — board-level init helpers shared across components. */
#pragma once

#include "esp_err.h"
#include "driver/i2c_master.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

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
 * other non [a-z0-9] folded to a single '-', so "Living Room" ->
 * "living-room" (reachable as living-room.local). wifi_apply_hostname()
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

/* ---- Off-hook ("receiver off-hook" / ROH) alert ----
 * A handset left off the hook sits on dial tone forever otherwise: the line
 * is unreachable (callers get busy) with nothing to draw attention to it.
 * Real switches escalate to a deliberately loud, dissonant alert tone after
 * a timeout; this reproduces that (TONE_OFFHOOK_ALERT, tones.h), triggered
 * from the dial-tone state only -- the other idle-ish states either already
 * play something attention-getting (reorder) or represent a real call.
 * Hanging up clears it, same as any other state. */
#define BOARD_OFFHOOK_ALERT_MIN_S 10
#define BOARD_OFFHOOK_ALERT_MAX_S 3600
typedef struct {
    bool     enabled;
    uint32_t seconds; /* dial-tone dwell before the alert starts */
} board_offhook_alert_t;

/* Defaults to enabled / 120 s until board_offhook_alert_set() says otherwise. */
board_offhook_alert_t board_offhook_alert_get(void);
/* ESP_ERR_INVALID_ARG outside BOARD_OFFHOOK_ALERT_MIN_S..MAX_S. Takes effect
 * on the next dial tone -- no reboot (the FSM reads this when it starts the
 * dwell timer). */
esp_err_t board_offhook_alert_set(board_offhook_alert_t cfg);

/* ---- AEC bulk delay (per-line, per-device) ----
 * Each physical board's own I2S/codec/acoustic path has a slightly different
 * bulk delay (measured via the bringup `echoprobe` command, applied by hand
 * or via `aec sweep`) -- one firmware image serves every board of a given
 * type, so this can't be a compile-time default the way most of aec.h's
 * tuning constants are. Persisted per line so a value set by hand or found
 * by `aec sweep` survives a reboot instead of reverting to the generic
 * compiled default every time. */
/* Returns the persisted value for `line`, or `fallback_ms` if nothing has
 * been persisted yet -- callers pass their own compiled default so this
 * component doesn't need to know AEC's tuning constants. */
uint32_t board_aec_delay_get(int line, uint32_t fallback_ms);
esp_err_t board_aec_delay_set(int line, uint32_t ms);

/* ---- 211 "record a message" max duration ----
 * Board-wide (not per-line): recording is a local feature, not something
 * that varies by which line placed the call. Returns `fallback_ms` if
 * never set. */
#define BOARD_RECORD_MAX_MS_MIN  5000   /* 5 s -- below this isn't a useful message */
#define BOARD_RECORD_MAX_MS_MAX 120000  /* 2 min -- generous ceiling, storage is the real limit */
uint32_t board_record_max_ms_get(uint32_t fallback_ms);
/* ESP_ERR_INVALID_ARG outside BOARD_RECORD_MAX_MS_MIN..MAX. */
esp_err_t board_record_max_ms_set(uint32_t ms);

/* ---- Speaker (earpiece) drive level, per line ----
 * ES8388 LOUT1VOL/ROUT1VOL convention (see es8388.h / the bring-up `spkamp`
 * command): 0 = mute, 33 = 0 dB, 1 dB/step, 63 = max. Persisted so a level
 * tuned by ear (console `spkamp`, or the webui volume control) survives a
 * reboot instead of reverting to the compiled boot default every time. */
uint8_t board_speaker_volume_get(int line, uint8_t fallback);
esp_err_t board_speaker_volume_set(int line, uint8_t vol);

/* ---- Line/microSD hardware configuration (SMD2 only) ----
 * The SMD2 PCB's GPIO 18-21 are wired to EITHER a second AG1171 SLIC (line
 * 2) OR a microSD socket, never both -- a BOM/jumper choice made when the
 * board was assembled (see ROTARY_PHONE_SMD.md's "Build options": populate
 * line 2's 0R jumpers, or the microSD's 0R jumpers, one set only). This used
 * to be a build-time choice (CONFIG_BOARD_NUM_LINES/CONFIG_BOARD_MICROSD),
 * meaning a separate firmware image per board variant. Now runtime,
 * NVS-backed, so one firmware image works on either board -- but that means
 * something has to decide which peripheral those 4 pins actually are, and a
 * wrong guess doesn't just misbehave in software, it drives real GPIO pins
 * as the wrong peripheral (the line-2 side of that pair carries SLIC
 * control signals). So: defaults to the SAFEST option (1 line, no SD) until
 * a human explicitly confirms otherwise -- see board_hw_config_set(). */
typedef struct {
    int  num_lines;    /* 1 or 2 */
    bool has_microsd;  /* only meaningful combined with num_lines == 1 */
} board_hw_config_t;

/* Current config. (1, false) -- the safe default -- until
 * board_hw_config_set() has been called at least once; see
 * board_hw_config_is_set() to tell "explicitly configured to 1-line" apart
 * from "never configured, still on the default". */
board_hw_config_t board_hw_config_get(void);

/* True once a human has explicitly confirmed a configuration. The web UI
 * (and, on the SMD2 THT-style first-boot flow, WiFi setup) should prompt
 * for this if false, rather than silently trusting the untouched default
 * forever. */
bool board_hw_config_is_set(void);

/* Rejects num_lines outside 1-2, and num_lines==2 combined with
 * has_microsd==true (the GPIO conflict this whole API exists to prevent),
 * with ESP_ERR_INVALID_ARG. Takes effect on reboot -- like every other
 * hardware config in this project, peripheral GPIO/driver init only runs
 * once at boot. */
esp_err_t board_hw_config_set(board_hw_config_t cfg);

/* Best-effort, purely advisory evidence for the config UI to show a human
 * before they confirm -- NOT authoritative, and deliberately not used to
 * drive behavior automatically (see the risk this whole file's comment
 * above describes). Just a raw, passive GPIO18-21 level read before any
 * peripheral has claimed them -- safe (input-only, drives nothing) but not
 * proof: a floating/undriven pin reads as whatever noise it picks up. Only
 * meaningful if called before ag1170's line-2 init or sdcard_init(), i.e.
 * very early in app_main() -- after boot, whichever driver is active owns
 * those pins and this will just reflect that instead. */
typedef struct {
    bool valid;               /* false if board type isn't SMD2 -- no GPIO18-21 pair to read */
    int  raw_levels[4];        /* GPIO 18,19,20,21 as read, 0/1 each */
    bool looks_like_line2;     /* SHK-equivalent (GPIO19) reads low at rest, as a bare/idle AG1171 SHK divider would */
} board_hw_evidence_t;
board_hw_evidence_t board_hw_detect_evidence(void);

#ifdef __cplusplus
}
#endif

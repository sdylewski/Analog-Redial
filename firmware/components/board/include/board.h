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

/* ---- Answering machine (per line) ----
 * After `rings` unanswered rings, auto-answers and plays this line's own
 * greeting (recorded via dial code 212, see telephony.c) then records the
 * caller after a beep. `other_line` gates whether an unanswered call FROM
 * this board's other line (not VoIP) also gets auto-answered -- off by
 * default, since routing a family member's unanswered intercom call to
 * voicemail isn't obviously wanted. Meaningless/hidden in the UI on a
 * 1-line board. See archive/ANSWERING_MACHINE.md. */
#define BOARD_AM_RINGS_MIN    1
#define BOARD_AM_RINGS_MAX   15
#define BOARD_AM_MAX_MS_MIN 5000    /* 5 s */
#define BOARD_AM_MAX_MS_MAX 120000  /* 2 min -- matches BOARD_RECORD_MAX_MS_MAX's ceiling */
typedef struct {
    bool     enabled;
    uint8_t  rings;      /* rings before auto-answer */
    uint32_t max_ms;     /* max caller message length */
    bool     other_line;
} board_am_cfg_t;

/* Defaults to disabled / 4 rings / 30000 ms / other_line off. */
board_am_cfg_t board_am_get(int line);
/* ESP_ERR_INVALID_ARG if rings or max_ms are outside their MIN..MAX above. */
esp_err_t board_am_set(int line, board_am_cfg_t cfg);

/* ---- Timezone (for Quiet hours below) ----
 * Nothing else in this firmware cares about local time -- recording
 * timestamps and everything else deliberately use UTC (gmtime_r()), and
 * stay that way regardless of this setting. A POSIX TZ string (e.g.
 * "PST8PDT,M3.2.0,M11.1.0" for US Pacific with DST, "EST5" for fixed-offset
 * US Eastern with no DST rule) -- newlib's localtime_r() parses this
 * directly, so no timezone database needs to ship in firmware. See
 * https://github.com/nayarsystems/posix_tz_db for a lookup table of common
 * strings. "" (default) means UTC. */
#define BOARD_TZ_MAX 64
void      board_timezone_get(char *out, size_t len); /* "" = UTC (default) */
/* Applies immediately (setenv("TZ", ...) + tzset()) -- no reboot needed. */
esp_err_t board_timezone_set(const char *tz);
/* Applies whatever's saved in NVS to the process's C library timezone.
 * Call once at boot, before anything might call board_quiet_hours_active(). */
void      board_timezone_init(void);

/* ---- Quiet hours ----
 * While enabled and the current local time (board_timezone_get(), UTC if
 * unset) falls in [start, end), incoming calls on either line go straight
 * to the answering machine instead of ringing -- old mechanical ringers are
 * loud, and not everyone wants one going off at 2 a.m. One setting for both
 * lines (not per-line -- if you're asleep, you're asleep regardless of
 * which line rings), and it works independently of the per-line Answering
 * Machine config above: a call still gets routed to voicemail during quiet
 * hours even on a line whose own AM is disabled/unconfigured, since
 * "silently drop the call" isn't a real option and there's nowhere else to
 * send it -- auto_answer() (telephony.c) already degrades gracefully with
 * no greeting recorded (straight to a beep, then record). */
typedef struct {
    bool    enabled;
    uint8_t start_hour, start_min; /* 0-23, 0-59 */
    uint8_t end_hour, end_min;
} board_quiet_hours_t;

/* Defaults to disabled, 21:00-07:00 (9 pm-7 am) once enabled. `start` later
 * than `end` (the common case) means a window that wraps past midnight --
 * see board_quiet_hours_active(). */
board_quiet_hours_t board_quiet_hours_get(void);
/* ESP_ERR_INVALID_ARG if any hour/minute is out of range. Takes effect on
 * the next incoming call -- no reboot needed. */
esp_err_t board_quiet_hours_set(board_quiet_hours_t cfg);
/* True if quiet hours are enabled and right now (local time) falls inside
 * the configured window. */
bool board_quiet_hours_active(void);

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

/* Current config: whatever was last written, by a human
 * (board_hw_config_set()) OR by board_hw_config_autodetect()'s best-effort
 * guess -- see board_hw_config_is_set() to tell those apart from each other
 * and from "nothing written yet, on the hardcoded (1, false) fallback". */
board_hw_config_t board_hw_config_get(void);

/* True once a human has explicitly confirmed a configuration via
 * board_hw_config_set() -- NOT true just because board_hw_config_autodetect()
 * has written a guess (that's the whole point: the guess should work
 * out-of-the-box, but still needs a one-time human look, see the first-boot
 * banner in webui.c). The web UI should prompt for this if false, rather
 * than silently trusting an unconfirmed value forever. */
bool board_hw_config_is_set(void);

/* Rejects num_lines outside 1-2, and num_lines==2 combined with
 * has_microsd==true (the GPIO conflict this whole API exists to prevent),
 * with ESP_ERR_INVALID_ARG. Takes effect on reboot -- like every other
 * hardware config in this project, peripheral GPIO/driver init only runs
 * once at boot. Marks the config human-confirmed (board_hw_config_is_set()
 * becomes true) -- this is the one function that does. */
esp_err_t board_hw_config_set(board_hw_config_t cfg);

/* Best-effort auto-guess, run once early in app_main() (before ag1170_init()
 * and any sdcard_init()) so line 2 or microSD actually works out of the box
 * on a fresh board instead of sitting on the conservative (1, false)
 * fallback until someone visits Settings. No-op if a value already exists
 * (a prior guess, or a human's explicit confirm) -- only ever fires once,
 * on the very first boot after flashing/factory-reset. Deliberately does
 * NOT mark the config confirmed (board_hw_config_is_set() stays false) --
 * it's a guess, not a human's word, so the first-boot banner still asks for
 * a one-time review. Evidence used, strongest first: the SD_CD card-detect
 * pin (SMD4 only -- a real physical switch, decisive when a board has one:
 * a card can only be seated on a board actually wired for microSD), then
 * board_hw_detect_evidence()'s weaker SHK-divider heuristic for 2-line vs
 * 1-line. Falls back to (1, false) if neither gives a positive read. */
void board_hw_config_autodetect(void);

/* Best-effort, purely advisory evidence for a human to sanity-check
 * board_hw_config_autodetect()'s guess against -- e.g. Settings ->
 * Hardware's "Advisory evidence" row, or the first-boot banner. NOT
 * authoritative on its own (see that function's doc comment for why a wrong
 * guess matters). The raw_levels/looks_like_line2 GPIO18-21 read is only
 * physically possible before any peripheral has claimed those pins -- input-
 * only, drives nothing, but a floating/undriven pin reads as whatever noise
 * it picks up, so it's cached from whenever that first real read happened
 * (autodetect(), very early in app_main(), before ag1170_init()/
 * sdcard_init() can claim them) and just replayed on every later call in
 * the same boot -- this function does NOT re-read those 4 pins once
 * something owns them, it would glitch a live peripheral. card_present
 * (BOARD_SD_CD, SMD4 only) has no such restriction -- it's a dedicated,
 * always-safe-to-read pin -- so that field is live every call, not cached. */
typedef struct {
    bool valid;               /* false if board type isn't SMD -- no GPIO18-21 pair to read, or no cached reading exists (already had a confirmed/guessed value before the first read this boot) */
    int  raw_levels[4];        /* GPIO 18,19,20,21 as read at cache time, 0/1 each */
    bool looks_like_line2;     /* SHK-equivalent (GPIO19) read low at rest, as a bare/idle AG1171 SHK divider would */
    bool card_present;         /* BOARD_SD_CD read low (card physically seated) -- always false on boards without a CD pin (SMD2) */
} board_hw_evidence_t;
board_hw_evidence_t board_hw_detect_evidence(void);

#ifdef __cplusplus
}
#endif

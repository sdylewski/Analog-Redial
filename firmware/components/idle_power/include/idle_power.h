/*
 * idle_power.h — optional low-power idle mode.
 *
 * After the configured idle time with every line on-hook, duty-cycles each
 * SLIC's PD pin (ag1170_set_power()) instead of leaving it powered
 * continuously, and -- only when no VoIP server is configured -- also drops
 * WiFi into power-save. Any line going off-hook (or starting to dial, ring,
 * record, etc.) immediately restores full power. See
 * archive/POWER_SAVING.md for the datasheet numbers and reasoning behind
 * the duty-cycle timing and the VoIP gating.
 *
 * Disabled by default -- this is an opt-in tradeoff (a small added delay on
 * the first pickup after a long idle stretch), not a change to default
 * behavior.
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IDLE_POWER_MINUTES_MIN 1
#define IDLE_POWER_MINUTES_MAX 60

/* Two SLIC duty-cycle presets, both 100ms on-time (the AG1171 wake +
 * debounce + margin floor -- see idle_power.c, not adjustable, doesn't
 * depend on the period). "Balanced" is this feature's original tuning;
 * "max savings" trades roughly another ~5mA/line of average draw for
 * roughly double the worst-case pickup latency, based on user feedback
 * that a real handset takes ~500ms-1s to reach your ear anyway -- see
 * archive/POWER_SAVING.md's "Timing derivation" for the exact math on
 * both. Exposed as named presets, not a free-form period field, to keep
 * this a two-click choice rather than a tuning knob someone has to
 * understand the datasheet math to set safely. */
#define IDLE_POWER_INTERVAL_MS_BALANCED     500
#define IDLE_POWER_INTERVAL_MS_MAX_SAVINGS 1000

typedef struct {
    bool     enabled;
    uint16_t minutes;     /* idle time (all lines on-hook) before low-power kicks in */
    bool     max_savings; /* false = balanced (default), true = max savings */
} idle_power_cfg_t;

/* Defaults to disabled / 5 minutes until idle_power_set() says otherwise. */
idle_power_cfg_t idle_power_get(void);

/* ESP_ERR_INVALID_ARG outside IDLE_POWER_MINUTES_MIN..MAX. Takes effect
 * immediately -- no reboot needed. */
esp_err_t idle_power_set(idle_power_cfg_t cfg);

/* Starts the background monitor task. Call once at boot, after ag1170,
 * telephony (which also brings up voip) and wifi are all up. */
void idle_power_init(void);

/* True while the system is currently past the idle threshold and
 * duty-cycling (SLIC(s) and/or WiFi actually sleeping right now). For the
 * Settings/status page -- not something normal callers need. */
bool idle_power_is_sleeping(void);

/* Call whenever there's activity that should postpone/undo low-power idle
 * even though every line is on-hook -- currently just the web UI (webui.c
 * calls this from the httpd server's connection-open hook, so simply
 * loading any page counts). Restarts the idle clock and, on this
 * component's own next tick (<= TICK_MS later), restores full SLIC/WiFi
 * power if it had already gone to sleep. Safe to call from any task --
 * actual hardware calls (ag1170_set_power()/wifi_set_power_save()) only
 * ever happen from idle_power_task() itself, this just flags that they
 * should run. */
void idle_power_note_activity(void);

/* Rough estimate, in watts, of what enabling idle_power would save on this
 * board in its *current* configuration (line count, VoIP configured or
 * not, and which duty-cycle preset is selected) -- purely informational,
 * for the Settings page hint text. See archive/POWER_SAVING.md for where
 * these numbers come from. */
float idle_power_estimated_savings_w(void);

/* Rough worst-case latency, in ms, before a genuine pickup is noticed once
 * past the idle threshold, in the *current* duty-cycle preset -- purely
 * informational, for the Settings page hint text. See
 * archive/POWER_SAVING.md's "Timing derivation" for where this comes
 * from. */
int idle_power_worst_case_latency_ms(void);

#ifdef __cplusplus
}
#endif

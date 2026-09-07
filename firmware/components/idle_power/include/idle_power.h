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

typedef struct {
    bool     enabled;
    uint16_t minutes; /* idle time (all lines on-hook) before low-power kicks in */
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

/* Rough estimate, in watts, of what enabling idle_power would save on this
 * board in its *current* configuration (line count, VoIP configured or
 * not) -- purely informational, for the Settings page hint text. See
 * archive/POWER_SAVING.md for where these numbers come from. */
float idle_power_estimated_savings_w(void);

#ifdef __cplusplus
}
#endif

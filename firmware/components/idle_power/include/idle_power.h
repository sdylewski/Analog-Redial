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
 * Enabled by default, with every option on (a small added delay on the first pickup after a long idle
 * stretch is the cost; each can be turned off in Settings -> Hardware). Previously this was an opt-in tradeoff -- (a small added delay on
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
    /* Drop WiFi transmit power while the signal is strong. Independent of
     * `enabled` -- it applies all the time, not just while idle, since a
     * strong link is wasting TX power whether or not the phone is in use. */
    bool     adaptive_tx;
    /* Stop the audio pipeline (I2S + its 10ms processing task) whenever every
     * line is on-hook, resuming on pickup. Also independent of `enabled`:
     * it's cheap to undo (no multi-minute threshold), so it applies after a
     * couple of idle seconds. See audio_allow_pause(). */
    bool     pause_audio;
    /* Let the CPU clock drop from 160 to 80 MHz whenever it's idle (esp_pm
     * dynamic frequency scaling). FreeRTOS switches back to 160 the moment
     * any task runs, so real work -- including the 10ms audio frames --
     * always happens at full speed; only idle time runs slower. Measured on
     * dualphone2 with CONFIG_PM_PROFILING: 89% of time at 80 MHz idle with
     * audio paused, ~75% with audio running. */
    bool     cpu_scale;
} idle_power_cfg_t;

/* Thresholds for adaptive_tx, on the RSSI we *receive* from the AP. The link
 * is roughly symmetric, so a strong downlink means the AP will still hear us
 * at reduced power. Lowering our own TX power doesn't change the RSSI we
 * measure, so there's no feedback loop to oscillate -- the gap between the
 * two thresholds is only there to ride out ordinary RSSI jitter.
 * Off by default: on a mesh network the downlink RSSI says little about whether the
 * router hears us at reduced power. If the link ever drops while reduced, the
 * driver stays at full power until reboot. */
#define IDLE_POWER_TX_LOW_DBM        11   /* ~8 dB below full */
#define IDLE_POWER_TX_FULL_DBM       20
#define IDLE_POWER_TX_REDUCE_RSSI   (-45) /* at or above: reduce */
#define IDLE_POWER_TX_RESTORE_RSSI  (-55) /* below: restore full */

/* Defaults to disabled / 5 minutes until idle_power_set() says otherwise. */
idle_power_cfg_t idle_power_get(void);

/* ESP_ERR_INVALID_ARG outside IDLE_POWER_MINUTES_MIN..MAX. Takes effect
 * immediately -- no reboot needed. */
esp_err_t idle_power_set(idle_power_cfg_t cfg);

/* Starts the background monitor task. Call once at boot, after ag1170,
 * telephony (which also brings up voip) and wifi are all up. */
void idle_power_init(void);

typedef enum {
    IDLE_POWER_DISABLED = 0, /* feature switched off */
    IDLE_POWER_IN_USE,       /* a line isn't idle, so the clock isn't running */
    IDLE_POWER_COUNTING,     /* idle, but not long enough to sleep yet */
    IDLE_POWER_ASLEEP,       /* in low-power mode right now */
} idle_power_state_t;

/* What the low-power logic is doing, for display. Any web request counts as
 * activity and wakes the board (see idle_power_note_activity()), so a web
 * page can essentially never observe IDLE_POWER_ASLEEP -- loading the page
 * ended it. woken_by_web and slept_s say what the board was doing just before
 * that page load, which is the part actually worth showing. The console
 * doesn't count as activity, so the `power` console command can see
 * IDLE_POWER_ASLEEP directly. */
typedef struct {
    idle_power_state_t state;
    uint32_t secs_until_sleep;  /* IDLE_POWER_COUNTING only */
    uint32_t secs_asleep_now;   /* IDLE_POWER_ASLEEP only */
    bool     woken_by_web;      /* the most recent wake came from a web request */
    uint32_t last_sleep_s;      /* how long that most recent sleep lasted */
    uint32_t total_asleep_s;    /* summed over every sleep since boot */
    uint32_t uptime_s;
    uint32_t sleep_count;       /* sleeps entered since boot */
    int      tx_power_dbm;      /* current WiFi TX power ceiling, 0 if WiFi off */
    int      rssi_dbm;          /* 0 if not connected */
    bool     audio_paused;
} idle_power_status_t;

idle_power_status_t idle_power_status(void);

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

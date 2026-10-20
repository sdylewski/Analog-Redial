#include "idle_power.h"
#include "audio.h"
#include "esp_pm.h"
#include "board_pins.h"
#include "board.h"
#include "ag1170.h"
#include "telephony.h"
#include "voip.h"
#include "wifi.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "idle_power";

#define NVS_NS       "idlepwr"
#define NVS_KEY_EN   "enabled"
#define NVS_KEY_MIN  "minutes"
#define NVS_KEY_MAXS "maxsave"
#define NVS_KEY_ADTX "adtx"
#define NVS_KEY_PAUD "paudio"
#define NVS_KEY_CPUS "cpuscale"

/* SLIC duty cycle while past the idle threshold -- see
 * archive/POWER_SAVING.md for the full derivation and for where the two
 * IDLE_POWER_INTERVAL_MS_* presets (idle_power.h) come from. AG1171
 * wake-up time is 50ms and SHK debounce 10ms (datasheet §8.3); 100ms
 * on-time is that plus real margin, independent of which preset's period
 * is active. */
#define SLIC_POLL_ON_MS 100
#define TICK_MS         50

/* AG1171 on-hook vs power-down current, 5V rail (datasheet §8.3): 60mA vs
 * 7mA typ. WiFi: WIFI_PS_NONE (today) sits close to the RX-active figure,
 * ~80mA @ 3.3V; WIFI_PS_MIN_MODEM's typical range is ~14-38mA depending on
 * CPU load, call it ~25mA typical -> ~55mA saved, *3.3V ~= 0.18W. Both are
 * datasheet-derived estimates, not bench-measured on real hardware -- see
 * archive/POWER_SAVING.md. (SLIC savings depend on which duty-cycle preset
 * is active -- computed live in idle_power_estimated_savings_w() below,
 * not a fixed constant here, so both presets stay correct from one
 * formula.) */
#define WIFI_SAVINGS_W 0.18f

static bool all_lines_idle(int num_lines);

/* On by default, every option (the user can switch any of them off in Settings -> Hardware). */
static idle_power_cfg_t s_cfg = { .enabled = true, .minutes = 5, .max_savings = false,
                                  .adaptive_tx = true, .pause_audio = true, .cpu_scale = true };
static bool             s_sleeping;
static bool             s_wifi_eco;
static bool             s_slic_on[BOARD_NUM_LINES];
static int64_t          s_idle_since_us; /* 0 while any line is busy */
/* Set from any task (idle_power_note_activity()), only ever cleared from
 * idle_power_task() itself -- a plain bool read/write race here is
 * harmless (worst case, one activity ping is noticed a tick late, <=
 * TICK_MS), so this doesn't need a lock the way actually touching hardware
 * (wake_fully()) would if it were called from more than one task. */
static volatile bool    s_activity_ping;

/* Bookkeeping for idle_power_status() only -- nothing decides anything off
 * these. Written from idle_power_task() except s_woken_by_web, which
 * wake_fully() sets from the same task based on s_wake_reason_web. */
static int64_t          s_sleep_started_us;
static int64_t          s_total_asleep_us;
static int64_t          s_last_sleep_us;
static uint32_t         s_sleep_count;
static bool             s_woken_by_web;
static bool             s_wake_reason_web; /* set just before a wake_fully() call */

static void load_cfg(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    uint8_t en;
    if (nvs_get_u8(h, NVS_KEY_EN, &en) == ESP_OK) s_cfg.enabled = (en != 0);
    uint32_t min;
    if (nvs_get_u32(h, NVS_KEY_MIN, &min) == ESP_OK &&
        min >= IDLE_POWER_MINUTES_MIN && min <= IDLE_POWER_MINUTES_MAX) {
        s_cfg.minutes = (uint16_t)min;
    }
    uint8_t maxs;
    if (nvs_get_u8(h, NVS_KEY_MAXS, &maxs) == ESP_OK) s_cfg.max_savings = (maxs != 0);
    uint8_t adtx;
    if (nvs_get_u8(h, NVS_KEY_ADTX, &adtx) == ESP_OK) s_cfg.adaptive_tx = (adtx != 0);
    uint8_t paud;
    if (nvs_get_u8(h, NVS_KEY_PAUD, &paud) == ESP_OK) s_cfg.pause_audio = (paud != 0);
    uint8_t cpus;
    if (nvs_get_u8(h, NVS_KEY_CPUS, &cpus) == ESP_OK) s_cfg.cpu_scale = (cpus != 0);
    nvs_close(h);
}

idle_power_cfg_t idle_power_get(void) { return s_cfg; }

static int current_interval_ms(void)
{
    return s_cfg.max_savings ? IDLE_POWER_INTERVAL_MS_MAX_SAVINGS : IDLE_POWER_INTERVAL_MS_BALANCED;
}

/* min == max pins the clock, which is how "off" is expressed -- there's no
 * separate disable call. Light sleep stays off: it isolates GPIOs between
 * wakeups (see CLAUDE.md's bench-debugging notes) and the SLIC hook poll runs
 * every 1ms anyway, so it would never get a chance to sleep. */
static void apply_cpu_scale(void)
{
#if CONFIG_PM_ENABLE
    esp_pm_config_t pm = {
        .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .min_freq_mhz = s_cfg.cpu_scale ? 80 : CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .light_sleep_enable = false,
    };
    esp_err_t err = esp_pm_configure(&pm);
    if (err != ESP_OK) ESP_LOGW(TAG, "esp_pm_configure: %s", esp_err_to_name(err));
    else ESP_LOGI(TAG, "CPU clock %d-%d MHz", pm.min_freq_mhz, pm.max_freq_mhz);
#endif
}

esp_err_t idle_power_set(idle_power_cfg_t cfg)
{
    if (cfg.minutes < IDLE_POWER_MINUTES_MIN || cfg.minutes > IDLE_POWER_MINUTES_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, NVS_KEY_EN, cfg.enabled ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_u32(h, NVS_KEY_MIN, cfg.minutes);
    if (err == ESP_OK) err = nvs_set_u8(h, NVS_KEY_MAXS, cfg.max_savings ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_u8(h, NVS_KEY_ADTX, cfg.adaptive_tx ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_u8(h, NVS_KEY_PAUD, cfg.pause_audio ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_u8(h, NVS_KEY_CPUS, cfg.cpu_scale ? 1 : 0);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) {
        s_cfg = cfg;
        ESP_LOGI(TAG, "idle power %s, %u min, %s, adaptive TX %s",
                 cfg.enabled ? "enabled" : "disabled", (unsigned)cfg.minutes,
                 cfg.max_savings ? "max savings" : "balanced", cfg.adaptive_tx ? "on" : "off");
        ESP_LOGI(TAG, "pause audio when idle: %s", cfg.pause_audio ? "on" : "off");
        apply_cpu_scale();
    }
    return err;
}

void idle_power_note_activity(void) { s_activity_ping = true; }

idle_power_status_t idle_power_status(void)
{
    idle_power_status_t st = { 0 };
    int64_t now = esp_timer_get_time();
    int num_lines = board_hw_config_get().num_lines;

    if (!s_cfg.enabled) {
        st.state = IDLE_POWER_DISABLED;
    } else if (s_sleeping && s_activity_ping) {
        /* A web request has just arrived but idle_power_task() (50ms tick)
         * hasn't acted on it yet -- which is exactly the situation of a web
         * page asking for this status. Report the wake that's already
         * underway rather than "asleep", which would be true for a few more
         * milliseconds and misleading after. */
        st.state = IDLE_POWER_COUNTING;
        st.secs_until_sleep = (uint32_t)s_cfg.minutes * 60;
        st.woken_by_web = true;
        st.last_sleep_s = (uint32_t)((now - s_sleep_started_us) / 1000000);
        st.total_asleep_s = (uint32_t)((s_total_asleep_us + now - s_sleep_started_us) / 1000000);
        st.uptime_s = (uint32_t)(now / 1000000);
        st.sleep_count = s_sleep_count;
        st.tx_power_dbm = wifi_get_tx_power_dbm();
        st.audio_paused = audio_is_paused();
        st.rssi_dbm = wifi_rssi();
        return st;
    } else if (s_sleeping) {
        st.state = IDLE_POWER_ASLEEP;
        st.secs_asleep_now = (uint32_t)((now - s_sleep_started_us) / 1000000);
    } else if (!all_lines_idle(num_lines) || s_idle_since_us == 0) {
        st.state = IDLE_POWER_IN_USE;
    } else {
        st.state = IDLE_POWER_COUNTING;
        int64_t left_us = (int64_t)s_cfg.minutes * 60 * 1000000 - (now - s_idle_since_us);
        st.secs_until_sleep = left_us > 0 ? (uint32_t)(left_us / 1000000) : 0;
    }
    st.woken_by_web   = s_woken_by_web;
    st.last_sleep_s   = (uint32_t)(s_last_sleep_us / 1000000);
    int64_t total = s_total_asleep_us + (s_sleeping ? now - s_sleep_started_us : 0);
    st.total_asleep_s = (uint32_t)(total / 1000000);
    st.uptime_s       = (uint32_t)(now / 1000000);
    st.sleep_count    = s_sleep_count;
    st.tx_power_dbm   = wifi_get_tx_power_dbm();
    st.audio_paused   = audio_is_paused();
    st.rssi_dbm       = wifi_rssi();
    return st;
}

float idle_power_estimated_savings_w(void)
{
    int num_lines = board_hw_config_get().num_lines;
    /* Same derivation as archive/POWER_SAVING.md's "Design" section, just
     * computed live against whichever preset's interval is active instead
     * of a fixed constant, so both presets get a correct number from one
     * formula. */
    float duty = (float)SLIC_POLL_ON_MS / (float)current_interval_ms();
    float avg_ma = duty * 60.0f + (1.0f - duty) * 7.0f;
    float saved_ma = 60.0f - avg_ma;
    float w_per_line = saved_ma * 5.0f / 1000.0f;
    float w = w_per_line * (float)num_lines;
    if (voip_server()[0] == '\0') w += WIFI_SAVINGS_W;
    return w;
}

int idle_power_worst_case_latency_ms(void)
{
    /* (interval - on_ms) waiting for the next window + on_ms settling in
     * it, simplifies to just interval + the system's existing
     * offhook_confirm_ms baseline (80ms) that runs either way -- see
     * archive/POWER_SAVING.md's "Timing derivation" for the full walk
     * through why the on_ms terms cancel. */
    return current_interval_ms() + 80;
}

static void set_line_power(int line, bool on)
{
    if (s_slic_on[line] == on) return;
    ag1170_set_power(line, on);
    s_slic_on[line] = on;
}

static bool all_lines_idle(int num_lines)
{
    for (int line = 0; line < num_lines; line++) {
        if (telephony_state(line) != TEL_IDLE) return false;
    }
    return true;
}

/* Restores full power on every subsystem this component might have touched.
 * Called the instant anything stops being idle, whether that's a real
 * off-hook, the feature being disabled, or VoIP getting configured mid-sleep. */
static void wake_fully(int num_lines)
{
    for (int line = 0; line < num_lines; line++) set_line_power(line, true);
    if (s_wifi_eco) {
        wifi_set_power_save(false);
        s_wifi_eco = false;
    }
    if (s_sleeping) {
        int64_t slept = esp_timer_get_time() - s_sleep_started_us;
        s_last_sleep_us = slept;
        s_total_asleep_us += slept;
        s_woken_by_web = s_wake_reason_web;
        ESP_LOGI(TAG, "leaving low-power idle after %lld s%s", (long long)(slept / 1000000),
                 s_wake_reason_web ? " (web request)" : "");
        s_sleeping = false;
    }
    s_wake_reason_web = false;
}

/* Adjusts WiFi TX power from received signal strength -- see the
 * IDLE_POWER_TX_* thresholds in idle_power.h. Runs every ADAPTIVE_TX_EVERY_MS,
 * not every tick; RSSI is a smoothed figure anyway. Always restores full power
 * when disconnected or when the option is off, so reconnecting never has to
 * happen at reduced power. */
#define ADAPTIVE_TX_EVERY_MS 10000
static int     s_tx_dbm;          /* what we last set; 0 = never set */
static int64_t s_tx_next_check_us;

static void adaptive_tx_step(int64_t now)
{
    if (now < s_tx_next_check_us) return;
    s_tx_next_check_us = now + (int64_t)ADAPTIVE_TX_EVERY_MS * 1000;
    if (!wifi_is_connected()) {
        s_tx_dbm = 0;  /* force a re-apply once connected */
        return;
    }
    int want = IDLE_POWER_TX_FULL_DBM;
    if (s_cfg.adaptive_tx) {
        int rssi = wifi_rssi();
        if (rssi >= IDLE_POWER_TX_REDUCE_RSSI) {
            want = IDLE_POWER_TX_LOW_DBM;
        } else if (rssi >= IDLE_POWER_TX_RESTORE_RSSI) {
            /* in the hysteresis band: hold whatever we have */
            want = s_tx_dbm ? s_tx_dbm : IDLE_POWER_TX_FULL_DBM;
        }
    }
    if (want != s_tx_dbm) {
        if (wifi_set_tx_power_dbm(want) == ESP_OK) {
            ESP_LOGI(TAG, "WiFi TX power -> %d dBm (RSSI %d dBm)", want, wifi_rssi());
            s_tx_dbm = want;
        }
    }
}

static void idle_power_task(void *arg)
{
    const TickType_t period = pdMS_TO_TICKS(TICK_MS);
    TickType_t last = xTaskGetTickCount();
    while (1) {
        int num_lines = board_hw_config_get().num_lines;
        int64_t now = esp_timer_get_time();

        adaptive_tx_step(now);
        audio_allow_pause(s_cfg.pause_audio && all_lines_idle(num_lines));

        /* Web UI activity (or anything else that pings us) counts as "not
         * idle" for the idle clock even though telephony itself still says
         * TEL_IDLE on every line -- restart the clock from now, and if
         * we'd already gone to sleep, undo that immediately rather than
         * making a live page load wait out the rest of a stale sleep
         * window. This is the fix for the gap this feature originally
         * shipped with (see archive/POWER_SAVING.md): without it, simply
         * using the web UI while the phone itself sits idle doesn't
         * prevent -- or recover from -- WiFi power-save, reintroducing the
         * exact HTTP latency/flakiness WIFI_PS_NONE was chosen to fix. */
        if (s_activity_ping) {
            s_activity_ping = false;
            if (s_sleeping || s_wifi_eco) {
                s_wake_reason_web = true;
                wake_fully(num_lines);
            }
            s_idle_since_us = all_lines_idle(num_lines) ? now : 0;
        }

        if (!s_cfg.enabled || !all_lines_idle(num_lines)) {
            if (s_sleeping || s_wifi_eco) wake_fully(num_lines);
            s_idle_since_us = 0;
            vTaskDelayUntil(&last, period);
            continue;
        }

        if (s_idle_since_us == 0) s_idle_since_us = now;
        int64_t idle_ms = (now - s_idle_since_us) / 1000;
        int64_t threshold_ms = (int64_t)s_cfg.minutes * 60 * 1000;

        if (idle_ms < threshold_ms) {
            /* Idle, but not long enough yet -- stay fully powered. */
            vTaskDelayUntil(&last, period);
            continue;
        }

        if (!s_sleeping) {
            ESP_LOGI(TAG, "entering low-power idle after %u min", (unsigned)s_cfg.minutes);
            s_sleeping = true;
            s_sleep_started_us = now;
            s_sleep_count++;
        }

        int64_t phase = (now / 1000) % current_interval_ms();
        bool want_on = phase < SLIC_POLL_ON_MS;
        for (int line = 0; line < num_lines; line++) set_line_power(line, want_on);

        /* WiFi power-save only once no VoIP server is configured at all --
         * see archive/POWER_SAVING.md for why this is the gate (not live
         * registration status) and re-checked every tick so configuring
         * VoIP while already sleeping immediately restores WIFI_PS_NONE. */
        bool want_wifi_eco = (voip_server()[0] == '\0');
        if (want_wifi_eco != s_wifi_eco) {
            wifi_set_power_save(want_wifi_eco);
            s_wifi_eco = want_wifi_eco;
        }

        vTaskDelayUntil(&last, period);
    }
}

void idle_power_init(void)
{
    load_cfg();
    apply_cpu_scale();
    for (int i = 0; i < BOARD_NUM_LINES; i++) s_slic_on[i] = true;
    BaseType_t ok = xTaskCreate(idle_power_task, "idle_power", 4096, NULL,
                                 tskIDLE_PRIORITY + 2, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "failed to start monitor task");
    }
}

#include "idle_power.h"
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

#define NVS_NS      "idlepwr"
#define NVS_KEY_EN  "enabled"
#define NVS_KEY_MIN "minutes"

/* SLIC duty cycle while past the idle threshold -- see
 * archive/POWER_SAVING.md for the full derivation. AG1171 wake-up time is
 * 50ms and SHK debounce 10ms (datasheet §8.3); 100ms on-time is that plus
 * real margin. 250ms period puts the worst case added latency to notice a
 * genuine pickup (waiting for the next window, plus that window's own
 * settle time) around 250ms, on top of the ~80ms offhook_confirm_ms the
 * system already takes today regardless -- lands close to the "~300ms"
 * figure quoted on the Settings page. */
#define SLIC_POLL_INTERVAL_MS 250
#define SLIC_POLL_ON_MS       100
#define TICK_MS               50

/* AG1171 on-hook vs power-down current, 5V rail (datasheet §8.3): 60mA vs
 * 7mA typ. At 100/250 = 40% duty, average settles to 0.4*60 + 0.6*7 =
 * 28.2mA -- ~31.8mA saved per line, *5V ~= 0.16W/line. WiFi:
 * WIFI_PS_NONE (today) sits close to the RX-active figure, ~80mA @ 3.3V;
 * WIFI_PS_MIN_MODEM's typical range is ~14-38mA depending on CPU load, call
 * it ~25mA typical -> ~55mA saved, *3.3V ~= 0.18W. Both are datasheet-derived
 * estimates, not bench-measured on real hardware -- see
 * archive/POWER_SAVING.md. */
#define SLIC_SAVINGS_W_PER_LINE 0.16f
#define WIFI_SAVINGS_W          0.18f

static idle_power_cfg_t s_cfg = { .enabled = false, .minutes = 5 };
static bool             s_sleeping;
static bool             s_wifi_eco;
static bool             s_slic_on[BOARD_NUM_LINES];
static int64_t          s_idle_since_us; /* 0 while any line is busy */

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
    nvs_close(h);
}

idle_power_cfg_t idle_power_get(void) { return s_cfg; }

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
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) {
        s_cfg = cfg;
        ESP_LOGI(TAG, "idle power %s, %u min", cfg.enabled ? "enabled" : "disabled",
                 (unsigned)cfg.minutes);
    }
    return err;
}

bool idle_power_is_sleeping(void) { return s_sleeping; }

float idle_power_estimated_savings_w(void)
{
    int num_lines = board_hw_config_get().num_lines;
    float w = SLIC_SAVINGS_W_PER_LINE * (float)num_lines;
    if (voip_server()[0] == '\0') w += WIFI_SAVINGS_W;
    return w;
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
        ESP_LOGI(TAG, "leaving low-power idle");
        s_sleeping = false;
    }
}

static void idle_power_task(void *arg)
{
    const TickType_t period = pdMS_TO_TICKS(TICK_MS);
    TickType_t last = xTaskGetTickCount();
    while (1) {
        int num_lines = board_hw_config_get().num_lines;
        int64_t now = esp_timer_get_time();

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
        }

        int64_t phase = (now / 1000) % SLIC_POLL_INTERVAL_MS;
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
    for (int i = 0; i < BOARD_NUM_LINES; i++) s_slic_on[i] = true;
    BaseType_t ok = xTaskCreate(idle_power_task, "idle_power", 4096, NULL,
                                 tskIDLE_PRIORITY + 2, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "failed to start monitor task");
    }
}

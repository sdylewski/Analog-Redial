#include "ag1170.h"
#include "board_pins.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include <string.h>

static const char *TAG = "ag1170";

typedef struct {
    board_line_pins_t pins;

    /* glitch-filtered hook level: 1 = off-hook, 0 = on-hook */
    int     level;
    int     raw_prev;
    int     glitch_cnt;
    int64_t level_since_us;

    bool    offhook_reported;
    int     pulse_count;
    int64_t last_pulse_end_us;
    bool    counting_digit;

    /* ring state */
    bool    ringing;
    int64_t ring_phase_start_us;
    int64_t last_fr_toggle_us;
    int     fr_level;
} line_state_t;

static line_state_t   s_ls[BOARD_NUM_LINES];
static ag1170_config_t s_cfg;
static ag1170_event_cb s_cb;
static void           *s_cb_ctx;
static TaskHandle_t    s_task;

static void emit(int line, ag1170_event_type_t type, int digit)
{
    if (!s_cb) return;
    ag1170_event_t ev = { .line = line, .type = type, .digit = digit };
    s_cb(&ev, s_cb_ctx);
}

void ag1170_set_event_cb(ag1170_event_cb cb, void *ctx)
{
    s_cb = cb;
    s_cb_ctx = ctx;
}

esp_err_t ag1170_set_power(int line, bool run)
{
    if (line < 0 || line >= BOARD_NUM_LINES) return ESP_ERR_INVALID_ARG;
    gpio_set_level(s_ls[line].pins.pd,
                   run ? BOARD_PD_LEVEL_RUN : BOARD_PD_LEVEL_POWERDOWN);
    return ESP_OK;
}

void ag1170_set_fr(int line, int level)
{
    if (line < 0 || line >= BOARD_NUM_LINES) return;
    s_ls[line].fr_level = level ? 1 : 0;
    gpio_set_level(s_ls[line].pins.fr, s_ls[line].fr_level);
}

void ag1170_set_rm(int line, int level)
{
    if (line < 0 || line >= BOARD_NUM_LINES) return;
    gpio_set_level(s_ls[line].pins.rm, level ? 1 : 0);
}

void ag1170_ring_start(int line)
{
    if (line < 0 || line >= BOARD_NUM_LINES) return;
    int64_t now = esp_timer_get_time();
    s_ls[line].ringing = true;
    s_ls[line].ring_phase_start_us = now;
    s_ls[line].last_fr_toggle_us = now;
    ESP_LOGI(TAG, "line %d ring start", line);
}

void ag1170_ring_stop(int line)
{
    if (line < 0 || line >= BOARD_NUM_LINES) return;
    s_ls[line].ringing = false;
    ag1170_set_rm(line, 0);
    ESP_LOGI(TAG, "line %d ring stop", line);
}

bool ag1170_is_ringing(int line)
{
    if (line < 0 || line >= BOARD_NUM_LINES) return false;
    return s_ls[line].ringing;
}

bool ag1170_is_offhook(int line)
{
    if (line < 0 || line >= BOARD_NUM_LINES) return false;
    return s_ls[line].offhook_reported;
}

int ag1170_raw_shk(int line)
{
    if (line < 0 || line >= BOARD_NUM_LINES) return -1;
    return gpio_get_level(s_ls[line].pins.shk);
}

/* ---- per-line ring cadence service ---- */
static void service_ring(int line, int64_t now)
{
    line_state_t *ls = &s_ls[line];
    if (!ls->ringing) return;

    uint32_t period_ms = s_cfg.ring_on_ms + s_cfg.ring_off_ms;
    int64_t  phase_ms  = ((now - ls->ring_phase_start_us) / 1000) % period_ms;
    bool     on        = phase_ms < s_cfg.ring_on_ms;

    if (on) {
        ag1170_set_rm(line, 1);
        int64_t half_us = (int64_t)(1000000 / (2 * (s_cfg.ring_freq_hz ? s_cfg.ring_freq_hz : 20)));
        if (now - ls->last_fr_toggle_us >= half_us) {
            ls->last_fr_toggle_us = now;
            ag1170_set_fr(line, !ls->fr_level);
        }
    } else {
        ag1170_set_rm(line, 0);
    }
}

/* ---- per-line hook + pulse FSM ---- */
static void service_hook(int line, int64_t now)
{
    line_state_t *ls = &s_ls[line];

    int raw = (gpio_get_level(ls->pins.shk) == BOARD_SHK_OFFHOOK_LEVEL) ? 1 : 0;

    /* glitch filter */
    if (raw != ls->level) {
        if (raw == ls->raw_prev) {
            if (++ls->glitch_cnt >= s_cfg.glitch_ms) {
                /* commit new level; first compute how long old level held */
                int old_level = ls->level;
                int64_t held_us = now - ls->level_since_us;
                ls->level = raw;
                ls->level_since_us = now;
                ls->glitch_cnt = 0;

                if (old_level == 0 /*on-hook*/ && raw == 1 /*off-hook*/) {
                    /* loop just reclosed: was it a dial pulse? */
                    if (ls->offhook_reported &&
                        (held_us / 1000) <= s_cfg.pulse_break_max_ms) {
                        ls->pulse_count++;
                        ls->last_pulse_end_us = now;
                        ls->counting_digit = true;
                    }
                }
            }
        } else {
            ls->glitch_cnt = 0;
        }
        ls->raw_prev = raw;
    } else {
        ls->glitch_cnt = 0;
        ls->raw_prev = raw;
    }

    int64_t in_level_ms = (now - ls->level_since_us) / 1000;

    /* off-hook (initial pickup or ring-trip answer) */
    if (ls->level == 1 && !ls->offhook_reported &&
        in_level_ms >= s_cfg.offhook_confirm_ms) {
        ls->offhook_reported = true;
        ls->pulse_count = 0;
        ls->counting_digit = false;
        if (ls->ringing) ag1170_ring_stop(line);
        emit(line, AG1170_EV_OFFHOOK, -1);
    }

    /* on-hook (hangup) */
    if (ls->level == 0 && ls->offhook_reported &&
        in_level_ms >= s_cfg.onhook_confirm_ms) {
        ls->offhook_reported = false;
        ls->pulse_count = 0;
        ls->counting_digit = false;
        emit(line, AG1170_EV_ONHOOK, -1);
    }

    /* digit completion (quiet after a pulse train, still off-hook) */
    if (ls->offhook_reported && ls->counting_digit && ls->level == 1 &&
        ls->pulse_count > 0 &&
        ((now - ls->last_pulse_end_us) / 1000) >= s_cfg.interdigit_ms) {
        int digit = ls->pulse_count % 10; /* 10 pulses => 0 */
        ls->pulse_count = 0;
        ls->counting_digit = false;
        emit(line, AG1170_EV_DIGIT, digit);
    }
}

static void monitor_task(void *arg)
{
    const TickType_t period = pdMS_TO_TICKS(1);
    TickType_t last = xTaskGetTickCount();
    while (1) {
        int64_t now = esp_timer_get_time();
        for (int line = 0; line < BOARD_NUM_LINES; line++) {
            service_hook(line, now);
            service_ring(line, now);
        }
        vTaskDelayUntil(&last, period);
    }
}

static void config_line_gpio(int line)
{
    board_line_pins_t p = board_line_pins(line);
    s_ls[line].pins = p;

    gpio_config_t in_cfg = {
        .pin_bit_mask = 1ULL << p.shk,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&in_cfg);

    gpio_config_t out_cfg = {
        .pin_bit_mask = (1ULL << p.fr) | (1ULL << p.rm) | (1ULL << p.pd),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&out_cfg);

    /* safe idle: run, not ringing */
    gpio_set_level(p.pd, BOARD_PD_LEVEL_RUN);
    gpio_set_level(p.rm, 0);
    gpio_set_level(p.fr, 0);

    s_ls[line].level = 0;
    s_ls[line].raw_prev = 0;
    s_ls[line].level_since_us = esp_timer_get_time();
}

esp_err_t ag1170_init(const ag1170_config_t *cfg)
{
    s_cfg = cfg ? *cfg : AG1170_DEFAULT_CONFIG();
    if (s_cfg.ring_freq_hz == 0) s_cfg.ring_freq_hz = 20;

    for (int line = 0; line < BOARD_NUM_LINES; line++) {
        memset(&s_ls[line], 0, sizeof(s_ls[line]));
        config_line_gpio(line);
    }

    BaseType_t ok = xTaskCreate(monitor_task, "ag1170", 4096, NULL,
                                configMAX_PRIORITIES - 4, &s_task);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "monitor task create failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "init: %d lines, ring %u Hz %u/%u ms",
             BOARD_NUM_LINES, s_cfg.ring_freq_hz, s_cfg.ring_on_ms, s_cfg.ring_off_ms);
    return ESP_OK;
}

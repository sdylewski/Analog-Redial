#include "ag1170.h"
#include "board_pins.h"
#include "board_config.h"
#include "board.h"
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

    /* Whether ag1170_set_power() last commanded this line's PD run (true) or
     * powered it down (false). SHK is meaningless while false -- the AG1171's
     * DC/DC converter is what supplies loop current in the first place, so
     * with it off there's no current to detect a lifted handset with
     * (datasheet §3.4: "logic outputs not valid" for 50ms even just coming
     * back up). service_hook() must not act on it either way. */
    bool    powered;

    /* ring state */
    bool    ringing;
    int64_t ring_phase_start_us;
    int64_t last_fr_toggle_us;
    int     fr_level;
} line_state_t;

static line_state_t   s_ls[BOARD_NUM_LINES]; /* array is always max width; see s_active_lines */
static ag1170_config_t s_cfg;
static ag1170_event_cb s_cb;
static void           *s_cb_ctx;
static TaskHandle_t    s_task;

/* How many lines are actually populated as SLICs, cached from
 * board_hw_config_get() once at init (NVS read; not something to repeat
 * every 1ms in monitor_task()). On a board configured for microSD instead
 * of line 2, GPIO18-21 are wired to the SD card, not a SLIC -- config_line_gpio()
 * must never be called for that line, and every per-line accessor below
 * checks against this (not just the raw array bound), so an inactive line
 * can't end up toggling whatever pins its zero-initialized s_ls[] entry
 * happens to contain. */
static int s_active_lines = 1;

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

static void set_pd_run(gpio_num_t pd, bool run)
{
#if CONFIG_BOARD_TYPE_SMD2
    /* SMD board: PD is driven through a diode (1N4148W, cathode at the PD
     * pin), not a transistor -- see hardware/ROTARY_PHONE_SMD.md, "AG1171
     * SLIC & RJ12": GPIO low pulls PD low through the diode (power-down);
     * high-Z lets the AG1171's own internal pull-up hold PD open (run).
     * Driving the GPIO high instead of releasing it isn't guaranteed
     * equivalent -- it depends on exactly where that internal pull-up sits
     * relative to the GPIO's high level, which isn't documented. */
    if (run) {
        gpio_set_direction(pd, GPIO_MODE_INPUT);
    } else {
        gpio_set_direction(pd, GPIO_MODE_OUTPUT);
        gpio_set_level(pd, 0);
    }
#else
    /* THT board: PD drives an open-collector transistor (BC846) -- GPIO low
     * = "run" (transistor off, PD left open), GPIO high = powerdown. See
     * board_pins_tht.h. */
    gpio_set_level(pd, run ? BOARD_PD_LEVEL_RUN : BOARD_PD_LEVEL_POWERDOWN);
#endif
}

esp_err_t ag1170_set_power(int line, bool run)
{
    if (line < 0 || line >= s_active_lines) return ESP_ERR_INVALID_ARG;
    line_state_t *ls = &s_ls[line];
    if (run && !ls->powered) {
        /* Re-powering after an intentional power-down (idle_power's SLIC
         * duty-cycle) -- start hook detection fresh rather than trusting
         * whatever service_hook() last computed before power-down; mirrors
         * config_line_gpio()'s own power-on-reset init below. */
        ls->level = 0;
        ls->raw_prev = 0;
        ls->glitch_cnt = 0;
        ls->level_since_us = esp_timer_get_time();
    }
    ls->powered = run;
    set_pd_run(ls->pins.pd, run);
    return ESP_OK;
}

void ag1170_set_fr(int line, int level)
{
    if (line < 0 || line >= s_active_lines) return;
    s_ls[line].fr_level = level ? 1 : 0;
    gpio_set_level(s_ls[line].pins.fr, s_ls[line].fr_level);
}

void ag1170_set_rm(int line, int level)
{
    if (line < 0 || line >= s_active_lines) return;
    gpio_set_level(s_ls[line].pins.rm, level ? 1 : 0);
}

void ag1170_ring_start(int line)
{
    if (line < 0 || line >= s_active_lines) return;
    int64_t now = esp_timer_get_time();
    s_ls[line].ringing = true;
    s_ls[line].ring_phase_start_us = now;
    s_ls[line].last_fr_toggle_us = now;
    ESP_LOGI(TAG, "line %d ring start", line);
}

void ag1170_ring_stop(int line)
{
    if (line < 0 || line >= s_active_lines) return;
    s_ls[line].ringing = false;
    ag1170_set_rm(line, 0);
    ESP_LOGI(TAG, "line %d ring stop", line);
}

bool ag1170_is_ringing(int line)
{
    if (line < 0 || line >= s_active_lines) return false;
    return s_ls[line].ringing;
}

void ag1170_ring_cadence_ms(uint16_t *on_ms, uint16_t *off_ms)
{
    if (on_ms) *on_ms = s_cfg.ring_on_ms;
    if (off_ms) *off_ms = s_cfg.ring_off_ms;
}

bool ag1170_is_offhook(int line)
{
    if (line < 0 || line >= s_active_lines) return false;
    return s_ls[line].offhook_reported;
}

int ag1170_raw_shk(int line)
{
    if (line < 0 || line >= s_active_lines) return -1;
    return gpio_get_level(s_ls[line].pins.shk);
}

int ag1170_pulse_count(int line)
{
    if (line < 0 || line >= s_active_lines) return -1;
    return s_ls[line].pulse_count;
}

bool ag1170_is_counting_digit(int line)
{
    if (line < 0 || line >= s_active_lines) return false;
    return s_ls[line].counting_digit;
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
    if (!ls->powered) return; /* SHK is meaningless right now -- see the `powered` field's doc comment */

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

                ESP_LOGD(TAG, "line %d SHK %s (prev level held %lld ms)", line,
                         raw ? "make (loop closed)" : "break (loop open)",
                         (long long)(held_us / 1000));

                if (old_level == 0 /*on-hook*/ && raw == 1 /*off-hook*/) {
                    /* loop just reclosed: was it a dial pulse? Needs both
                     * bounds -- the max alone let through a real, live bug:
                     * a real rotary dial's mechanism can produce a brief
                     * (~3-5ms) contact bounce right as it starts moving off
                     * rest, before the genuine ~46-59ms pulses that follow
                     * (confirmed live, 2026-09-04, two separate dials of
                     * "211": first came out "322" -- every digit had this
                     * extra bounce-as-pulse; second came out "212" -- only
                     * the third digit did. Real, intermittent mechanical
                     * bounce, not a consistent off-by-one, so a per-digit
                     * correction wouldn't have fixed it -- filtering the
                     * bounce itself does regardless of which digit it hits.
                     * The glitch filter above doesn't catch this -- glitch_ms=2
                     * only rejects transitions shorter than 2ms, and this
                     * bounce genuinely holds its level for several ms, long
                     * enough to commit as a real level change. The gap
                     * between the ~5ms bounce and the ~46ms+ real pulses is
                     * wide enough that pulse_break_min_ms=15 rejects the
                     * former with plenty of margin on both sides. */
                    if (ls->offhook_reported &&
                        (held_us / 1000) >= s_cfg.pulse_break_min_ms &&
                        (held_us / 1000) <= s_cfg.pulse_break_max_ms) {
                        ls->pulse_count++;
                        ls->last_pulse_end_us = now;
                        ls->counting_digit = true;
                        ESP_LOGD(TAG, "line %d pulse #%d (break was %lld ms, %u..%u)",
                                 line, ls->pulse_count, (long long)(held_us / 1000),
                                 s_cfg.pulse_break_min_ms, s_cfg.pulse_break_max_ms);
                    } else if (ls->offhook_reported && (held_us / 1000) < s_cfg.pulse_break_min_ms) {
                        ESP_LOGD(TAG, "line %d ignored sub-minimum break (%lld ms < %u -- "
                                      "dial-start contact bounce, not a pulse)",
                                 line, (long long)(held_us / 1000), s_cfg.pulse_break_min_ms);
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
        ESP_LOGD(TAG, "line %d OFF-HOOK confirmed (stable %lld ms >= %u)", line,
                 (long long)in_level_ms, s_cfg.offhook_confirm_ms);
        emit(line, AG1170_EV_OFFHOOK, -1);
    }

    /* on-hook (hangup) */
    if (ls->level == 0 && ls->offhook_reported &&
        in_level_ms >= s_cfg.onhook_confirm_ms) {
        ls->offhook_reported = false;
        ls->pulse_count = 0;
        ls->counting_digit = false;
        ESP_LOGD(TAG, "line %d ON-HOOK confirmed (stable %lld ms >= %u)", line,
                 (long long)in_level_ms, s_cfg.onhook_confirm_ms);
        emit(line, AG1170_EV_ONHOOK, -1);
    }

    /* digit completion (quiet after a pulse train, still off-hook) */
    if (ls->offhook_reported && ls->counting_digit && ls->level == 1 &&
        ls->pulse_count > 0 &&
        ((now - ls->last_pulse_end_us) / 1000) >= s_cfg.interdigit_ms) {
        int digit = ls->pulse_count % 10; /* 10 pulses => 0 */
        ESP_LOGD(TAG, "line %d DIGIT %d (%d pulses, quiet %lld ms >= %u)", line,
                 digit, ls->pulse_count,
                 (long long)((now - ls->last_pulse_end_us) / 1000), s_cfg.interdigit_ms);
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
        for (int line = 0; line < s_active_lines; line++) {
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

    /* PD is not in this mask -- set_pd_run() configures its direction
     * itself (push-pull on THT, direction-toggled input/output on SMD). */
    gpio_config_t out_cfg = {
        .pin_bit_mask = (1ULL << p.fr) | (1ULL << p.rm),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&out_cfg);

    /* safe idle: run, not ringing */
    set_pd_run(p.pd, true);
    gpio_set_level(p.rm, 0);
    gpio_set_level(p.fr, 0);

    s_ls[line].powered = true;
    s_ls[line].level = 0;
    s_ls[line].raw_prev = 0;
    s_ls[line].level_since_us = esp_timer_get_time();
}

esp_err_t ag1170_init(const ag1170_config_t *cfg)
{
    s_cfg = cfg ? *cfg : AG1170_DEFAULT_CONFIG();
    if (s_cfg.ring_freq_hz == 0) s_cfg.ring_freq_hz = 20;

    /* Read once here, not per-call -- see s_active_lines' own comment. On a
     * board configured for microSD, this is 1: GPIO18-21 belong to the SD
     * card, and line 1 (index 1) must never be configured as a SLIC. */
    s_active_lines = board_hw_config_get().num_lines;

    for (int line = 0; line < s_active_lines; line++) {
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
             s_active_lines, s_cfg.ring_freq_hz, s_cfg.ring_on_ms, s_cfg.ring_off_ms);
    return ESP_OK;
}

/* auxpins.c -- see auxpins.h. */
#include "auxpins.h"

#include "board.h"
#include "board_config.h"
#include "board_pins.h"
#include "bt_companion.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdcard.h"
#include <stdio.h>
#include <string.h>

#if BOARD_LINE1_ON_EXPANDER
#include "line_expander.h"
#endif

static const char *TAG = "auxpins";

#if BOARD_LINE1_ON_EXPANDER
/* v6: the line 1 expander's P4-P7, on J10 pins 2-5. The id is the expander
 * pin number, which is what a dial code stores. */
static const int s_pins[AUXPINS_COUNT] = {
    BOARD_EXP_BIT_SPARE0, BOARD_EXP_BIT_SPARE0 + 1, BOARD_EXP_BIT_SPARE0 + 2, BOARD_EXP_BIT_SPARE0 + 3,
};
#else
static const int s_pins[AUXPINS_COUNT] = {
    BOARD_LINE2_PIN_FR_MISO,    /* GPIO18 */
    BOARD_LINE2_PIN_SHK_SCLK,   /* GPIO19 */
    BOARD_LINE2_PIN_RM_MOSI,    /* GPIO20 */
    BOARD_LINE2_PIN_PD_CS,      /* GPIO21 */
};
#endif

static bool                s_cfg[AUXPINS_COUNT];
static esp_timer_handle_t  s_timer[AUXPINS_COUNT];

static int slot_of(int gpio)
{
    for (int i = 0; i < AUXPINS_COUNT; i++) if (s_pins[i] == gpio) return i;
    return -1;
}

size_t auxpins_list(int *out, size_t max)
{
    size_t n = 0;
    for (int i = 0; i < AUXPINS_COUNT && n < max; i++) out[n++] = s_pins[i];
    return n;
}

bool auxpins_valid(int gpio) { return slot_of(gpio) >= 0; }

const char *auxpins_label(int id, char *buf, size_t len)
{
#if BOARD_LINE1_ON_EXPANDER
    snprintf(buf, len, "J10 P%d", id);
#else
    snprintf(buf, len, "GPIO%d", id);
#endif
    return buf;
}

const char *auxpins_header(void)
{
#if BOARD_LINE1_ON_EXPANDER
    return "J10";
#else
    return "J5";
#endif
}

bool auxpins_usable(char *why, size_t why_len)
{
    const char *r = NULL;
#if BOARD_LINE1_ON_EXPANDER
    if (!line_exp_started()) line_exp_init();
    line_exp_status_t st;
    line_exp_status(&st);
    if (!st.chip_ok) r = "the I/O expander that drives J10 (U6) isn't answering on I2C";
#else
    board_hw_config_t cfg = board_hw_config_get();
    if (cfg.num_lines != 1) {
        r = "J5 is set to a second phone line, which uses these pins "
            "(Settings -> General -> Board hardware: choose \"Pin outputs\")";
    } else if (cfg.has_microsd) {
        r = "J5 is set to the microSD card, which uses these pins "
            "(Settings -> General -> Board hardware: choose \"Pin outputs\")";
    } else if (bt_companion_ibdk_ready()) {
        r = "the Bluetooth module is being configured over these pins";
    }
#ifdef BOARD_SD_CD
    else if (sdcard_card_present()) {
        r = "a microSD card is in the socket, and its contacts are on these pins";
    }
#endif
#endif
    if (r && why && why_len) snprintf(why, why_len, "%s", r);
    return r == NULL;
}

#if BOARD_LINE1_ON_EXPANDER
/* The expander's pins are outputs, low, from the moment it's set up. */
static esp_err_t configure(int slot) { s_cfg[slot] = true; return ESP_OK; }
static void pin_set(int slot, int level) { line_exp_set_spare(slot, level); }
static int  pin_get(int slot) { return line_exp_spare(slot); }
static void release(int slot)
{
    if (s_timer[slot]) esp_timer_stop(s_timer[slot]);
    if (s_cfg[slot]) line_exp_set_spare(slot, 0);
    s_cfg[slot] = false;
}
#else
static void pin_set(int slot, int level) { gpio_set_level(s_pins[slot], level); }
static int  pin_get(int slot) { return gpio_get_level(s_pins[slot]); }

static esp_err_t configure(int slot)
{
    if (s_cfg[slot]) return ESP_OK;
    /* Full gpio_config(): input buffer on so a read-back shows the pad, no
     * pulls, no interrupt; low before it becomes an output. */
    gpio_config_t c = {
        .pin_bit_mask = 1ULL << s_pins[slot],
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_hold_dis(s_pins[slot]);
    gpio_set_level(s_pins[slot], 0);
    esp_err_t err = gpio_config(&c);
    if (err != ESP_OK) return err;
    gpio_set_level(s_pins[slot], 0);
    gpio_sleep_sel_dis(s_pins[slot]);   /* keep driving through light sleep */
    s_cfg[slot] = true;
    return ESP_OK;
}

static void release(int slot)
{
    if (s_timer[slot]) esp_timer_stop(s_timer[slot]);
    if (s_cfg[slot]) {
        gpio_set_level(s_pins[slot], 0);
        /* Back to a plain input with no pulls: what it was before. */
        gpio_config_t c = {
            .pin_bit_mask = 1ULL << s_pins[slot],
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&c);
        s_cfg[slot] = false;
    }
}
#endif

static void timer_cb(void *arg)
{
    int slot = (int)(intptr_t)arg;
    char l[16];
    pin_set(slot, 0);
    ESP_LOGI(TAG, "%s low (time up)", auxpins_label(s_pins[slot], l, sizeof(l)));
}

void auxpins_all_off(void)
{
    for (int i = 0; i < AUXPINS_COUNT; i++) release(i);
}

esp_err_t auxpins_pulse(int gpio, int secs)
{
    int slot = slot_of(gpio);
    if (slot < 0) return ESP_ERR_INVALID_ARG;
    char why[120], l[16];
    auxpins_label(gpio, l, sizeof(l));
    if (!auxpins_usable(why, sizeof(why))) {
        ESP_LOGW(TAG, "%s not driven: %s", l, why);
        auxpins_all_off();
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = configure(slot);
    if (err != ESP_OK) return err;

    if (!s_timer[slot]) {
        const esp_timer_create_args_t a = {
            .callback = timer_cb, .arg = (void *)(intptr_t)slot, .name = "auxpin",
        };
        err = esp_timer_create(&a, &s_timer[slot]);
        if (err != ESP_OK) return err;
    }
    esp_timer_stop(s_timer[slot]);

    if (secs <= 0) {
        int now = pin_get(slot);
        pin_set(slot, !now);
        ESP_LOGI(TAG, "%s toggled -> %s", l, now ? "low" : "high");
        return ESP_OK;
    }
    pin_set(slot, 1);
    esp_timer_start_once(s_timer[slot], (uint64_t)secs * 1000000ULL);
    ESP_LOGI(TAG, "%s high for %d s", l, secs);
    return ESP_OK;
}

bool auxpins_level(int gpio)
{
    int slot = slot_of(gpio);
    if (slot < 0 || !s_cfg[slot]) return false;
    return pin_get(slot) != 0;
}

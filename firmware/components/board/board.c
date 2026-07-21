#include "board.h"
#include "board_pins.h"
#include "board_config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdbool.h>

static const char *TAG = "board";
static i2c_master_bus_handle_t s_bus;

esp_err_t board_i2c_init(void)
{
    if (s_bus) {
        return ESP_OK;
    }
    i2c_master_bus_config_t cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_NUM_0,
        .scl_io_num = BOARD_I2C_SCL,
        .sda_io_num = BOARD_I2C_SDA,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(err));
        s_bus = NULL;
        return err;
    }
    ESP_LOGI(TAG, "I2C bus up (SDA=%d SCL=%d, board=%s)", BOARD_I2C_SDA,
             BOARD_I2C_SCL, board_type_name(board_current_type()));
    return ESP_OK;
}

i2c_master_bus_handle_t board_i2c_bus(void)
{
    return s_bus;
}

#define BOARD_ES8388_ADDR 0x10
#define BOARD_WM8731_PROBE_ADDR 0x1A

void board_detect_codec(void)
{
    if (board_i2c_init() != ESP_OK) {
        ESP_LOGW(TAG, "board_detect_codec: I2C bus not available");
        return;
    }
    bool es8388_present = i2c_master_probe(s_bus, BOARD_ES8388_ADDR, 50) == ESP_OK;
    bool wm8731_present = i2c_master_probe(s_bus, BOARD_WM8731_PROBE_ADDR, 50) == ESP_OK;
    board_type_t built_for = board_current_type();

    if (es8388_present && !wm8731_present) {
        ESP_LOGI(TAG, "codec detect: ES8388 @ 0x10 (SMD board)");
        if (built_for != BOARD_TYPE_SMD) {
            ESP_LOGW(TAG, "firmware built for '%s' but an SMD/ES8388 codec is on "
                          "the bus -- rebuild with CONFIG_BOARD_TYPE_SMD2=y",
                     board_type_name(built_for));
        }
    } else if (wm8731_present && !es8388_present) {
        ESP_LOGI(TAG, "codec detect: WM8731 @ 0x1A (THT board)");
        if (built_for != BOARD_TYPE_THT) {
            ESP_LOGW(TAG, "firmware built for '%s' but a THT/WM8731 codec is on "
                          "the bus -- rebuild with CONFIG_BOARD_TYPE_THT=y",
                     board_type_name(built_for));
        }
    } else if (es8388_present && wm8731_present) {
        ESP_LOGW(TAG, "codec detect: both 0x10 and 0x11/0x1A ACKed -- unexpected, "
                      "check for I2C bus contention or an address conflict");
    } else {
        ESP_LOGW(TAG, "codec detect: no codec found on I2C bus (built for '%s')",
                 board_type_name(built_for));
    }
}

#if CONFIG_BOARD_TYPE_SMD2
void board_detect_line2_variant(void)
{
    const gpio_num_t pins[4] = { BOARD_LINE2_PIN_SHK_SCLK, BOARD_LINE2_PIN_RM_MOSI,
                                  BOARD_LINE2_PIN_FR_MISO, BOARD_LINE2_PIN_PD_CS };

    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << BOARD_LINE2_PIN_SHK_SCLK) | (1ULL << BOARD_LINE2_PIN_RM_MOSI) |
                        (1ULL << BOARD_LINE2_PIN_FR_MISO) | (1ULL << BOARD_LINE2_PIN_PD_CS),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
    /* No internal pull fighting the external network -- just let the nets
     * settle (RC, not a clock) before sampling. */
    vTaskDelay(pdMS_TO_TICKS(2));

    int level[4];
    for (int i = 0; i < 4; i++) {
        level[i] = gpio_get_level(pins[i]);
    }
    ESP_LOGI(TAG, "line2 variant detect: SHK/SCLK=%d RM/MOSI=%d FR/MISO=%d PD/CS=%d "
                  "(low=likely SLIC pulldown, see hardware/ROTARY_PHONE_SMD.md)",
             level[0], level[1], level[2], level[3]);

    /* SHK is the only pin with a *documented* passive network (R16-19, the
     * 10k/15k SHK divider) -- RM/FR/PD are logged above for the empirical
     * comparison in ROTARY_PHONE_SMD.md, but the mismatch warning below is
     * anchored on SHK alone until we know whether they corroborate it. */
    bool line2_slic_wired = level[0] == 0;

    if (line2_slic_wired) {
#if CONFIG_BOARD_MICROSD
        ESP_LOGW(TAG, "firmware built for microSD (CONFIG_BOARD_MICROSD=y) but SHK/SCLK reads "
                      "like the Line 2 SLIC pulldown is stuffed -- rebuild without "
                      "CONFIG_BOARD_MICROSD if this is a dual-line board");
#endif
    } else {
#if !CONFIG_BOARD_MICROSD
        ESP_LOGW(TAG, "firmware built for Line 2 SLIC (CONFIG_BOARD_MICROSD not set) but "
                      "SHK/SCLK doesn't read like the SLIC pulldown is stuffed -- check R7-R10 "
                      "stuffing or rebuild with CONFIG_BOARD_MICROSD=y");
#endif
    }
}
#else
void board_detect_line2_variant(void)
{
    /* THT board has no Line2/microSD XOR on GPIO 18-21 -- nothing to probe. */
}
#endif

#if CONFIG_BOARD_TYPE_SMD2
#define LED_TICK_MS   100  /* fastest blink rate (BOOTING); ERROR blinks
                             * slower by only toggling every few ticks */
#define LED_DUTY_MAX  1023 /* 10-bit LEDC resolution */
#define LED_TIMER     LEDC_TIMER_0
#define LED_CHANNEL   LEDC_CHANNEL_0
#define LED_SPEED     LEDC_LOW_SPEED_MODE /* C6 has no high-speed LEDC mode */

static esp_timer_handle_t   s_led_timer;
static board_led_state_t    s_led_state = BOARD_LED_BOOTING;

static void led_set_duty(uint32_t duty)
{
    ledc_set_duty(LED_SPEED, LED_CHANNEL, duty);
    ledc_update_duty(LED_SPEED, LED_CHANNEL);
}

static void led_timer_cb(void *arg)
{
    static unsigned tick;
    tick++;
    switch (s_led_state) {
    case BOARD_LED_OK:
        led_set_duty(0);
        break;
    case BOARD_LED_ERROR:
        led_set_duty(((tick / 5) % 2) ? LED_DUTY_MAX : 0); /* ~500ms half-period */
        break;
    case BOARD_LED_WIFI_CONNECTING: {
        /* Slow linear ramp up/down (a triangle wave, not a true sine, but
         * reads as "breathing" at this rate and needs no lookup table) --
         * full cycle every 3s (30 ticks at 100ms/tick). */
        const unsigned period = 30, half = period / 2;
        unsigned phase = tick % period;
        uint32_t duty = (phase < half) ? (phase * LED_DUTY_MAX) / half
                                        : LED_DUTY_MAX - ((phase - half) * LED_DUTY_MAX) / half;
        led_set_duty(duty);
        break;
    }
    case BOARD_LED_BOOTING:
    default:
        led_set_duty((tick % 2) ? LED_DUTY_MAX : 0); /* ~100ms half-period */
        break;
    }
}

void board_led_init(void)
{
    ledc_timer_config_t tcfg = {
        .speed_mode = LED_SPEED,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LED_TIMER,
        .freq_hz = 2000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&tcfg));

    ledc_channel_config_t ccfg = {
        .gpio_num = BOARD_STATUS_LED,
        .speed_mode = LED_SPEED,
        .channel = LED_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LED_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ccfg));

    const esp_timer_create_args_t targs = {
        .callback = &led_timer_cb,
        .name = "status_led",
    };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_led_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_led_timer, LED_TICK_MS * 1000));
}

void board_led_set_state(board_led_state_t state)
{
    s_led_state = state;
}
#else
void board_led_init(void) { }
void board_led_set_state(board_led_state_t state) { (void)state; }
#endif

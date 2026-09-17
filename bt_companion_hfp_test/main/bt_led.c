#include "bt_led.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "bt_led";

#define STATUS_LED_GPIO      GPIO_NUM_2
#define BLINK_PERIOD_US      (300 * 1000)
#define PAIRING_TIMEOUT_US   (60 * 1000 * 1000) /* give up blinking after 60s if nobody connects */

static esp_timer_handle_t s_blink_timer;
static esp_timer_handle_t s_timeout_timer;
static bool s_led_on = false;
static bool s_connected = false;

static void set_led(bool on)
{
    s_led_on = on;
    gpio_set_level(STATUS_LED_GPIO, on ? 1 : 0);
}

static void blink_cb(void *arg)
{
    set_led(!s_led_on);
}

static void settle(void)
{
    esp_timer_stop(s_blink_timer); /* no-op, harmless, if not running */
    set_led(s_connected);
}

static void pairing_timeout_cb(void *arg)
{
    ESP_LOGI(TAG, "pairing window closed with no connection -- LED settling");
    settle();
}

void bt_led_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << STATUS_LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&cfg);
    set_led(false);

    const esp_timer_create_args_t blink_args = { .callback = blink_cb, .name = "bt_led_blink" };
    esp_timer_create(&blink_args, &s_blink_timer);
    const esp_timer_create_args_t timeout_args = { .callback = pairing_timeout_cb, .name = "bt_led_pair_to" };
    esp_timer_create(&timeout_args, &s_timeout_timer);
}

void bt_led_start_pairing(void)
{
    esp_timer_stop(s_timeout_timer);
    esp_timer_stop(s_blink_timer);
    set_led(false);
    esp_timer_start_periodic(s_blink_timer, BLINK_PERIOD_US);
    esp_timer_start_once(s_timeout_timer, PAIRING_TIMEOUT_US);
}

void bt_led_set_connected(bool connected)
{
    s_connected = connected;
    esp_timer_stop(s_timeout_timer);
    settle();
}

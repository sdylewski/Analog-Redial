#include "bt_gpio_test.h"
#include "bt_audio_io.h"
#include "driver/gpio.h"
#include "esp_console.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* GPIO34/35/36/39 are ADC1-only, input-only pins on the original ESP32 --
 * no output driver exists on the pin at all, so gpio_set_level() on one of
 * these is a silent no-op at the hardware level (the register write itself
 * doesn't fail, there's just no transistor there to act on it). Anything
 * that iterates a pin list has to know this or it'll "mysteriously" never
 * show voltage on those four regardless of firmware correctness. */
static bool is_input_only(int pin)
{
    return pin == 34 || pin == 35 || pin == 36 || pin == 39;
}

static int cmd_gpio_set(int argc, char **argv)
{
    if (argc < 3) { printf("usage: gpio_set <pin> <0|1>\n"); return 1; }
    int pin = atoi(argv[1]);
    int level = atoi(argv[2]) ? 1 : 0;
    if (is_input_only(pin)) {
        printf("GPIO%d is input-only on this chip -- can't drive it, try gpio_get\n", pin);
        return 1;
    }
    gpio_config_t cfg = { .pin_bit_mask = 1ULL << pin, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&cfg);
    gpio_set_level(pin, level);
    vTaskDelay(pdMS_TO_TICKS(5)); /* let it settle before reading back */
    printf("GPIO%d set to %d, reads back as %d\n", pin, level, gpio_get_level(pin));
    return 0;
}

static int cmd_gpio_get(int argc, char **argv)
{
    if (argc < 2) { printf("usage: gpio_get <pin>\n"); return 1; }
    int pin = atoi(argv[1]);
    gpio_config_t cfg = { .pin_bit_mask = 1ULL << pin, .mode = GPIO_MODE_INPUT };
    gpio_config(&cfg);
    printf("GPIO%d = %d\n", pin, gpio_get_level(pin));
    return 0;
}

/* Note: 25/26 are also the bt_audio_io.c DAC pins, already claimed for
 * DAC_CHANNEL_MASK_CH0/CH1 by bt_audio_io_init() at boot. Driving them as
 * plain GPIO here reconfigures them out of DAC mode -- fine for a one-off
 * wiring check, but the tone/level diagnostics won't work again on that
 * pin until the board reboots (which re-runs bt_audio_io_init()). */
static const int CYCLE_PINS[] = { 36, 39, 34, 35, 32, 33, 25, 26, 27 };
#define CYCLE_PIN_COUNT (sizeof(CYCLE_PINS) / sizeof(CYCLE_PINS[0]))
static TaskHandle_t s_cycle_task;

static void gpio_cycle_task(void *arg)
{
    int ms = (int)(intptr_t)arg;
    while (1) {
        for (size_t i = 0; i < CYCLE_PIN_COUNT; i++) {
            int pin = CYCLE_PINS[i];
            if (is_input_only(pin)) {
                gpio_config_t cfg = { .pin_bit_mask = 1ULL << pin, .mode = GPIO_MODE_INPUT };
                gpio_config(&cfg);
                printf("GPIO%d (input-only, reading) = %d\n", pin, gpio_get_level(pin));
                vTaskDelay(pdMS_TO_TICKS(ms));
            } else {
                gpio_config_t cfg = { .pin_bit_mask = 1ULL << pin, .mode = GPIO_MODE_OUTPUT };
                gpio_config(&cfg);
                gpio_set_level(pin, 1);
                printf("GPIO%d -> HIGH\n", pin);
                vTaskDelay(pdMS_TO_TICKS(ms));
                gpio_set_level(pin, 0);
                printf("GPIO%d -> LOW\n", pin);
            }
        }
    }
}

static int cmd_gpio_cycle(int argc, char **argv)
{
    if (s_cycle_task) {
        vTaskDelete(s_cycle_task);
        s_cycle_task = NULL;
        printf("gpio_cycle stopped\n");
    }
    if (argc < 2 || strcmp(argv[1], "stop") == 0) return 0;
    int ms = atoi(argv[1]);
    if (ms <= 0) { printf("usage: gpio_cycle <ms-per-pin> | gpio_cycle stop\n"); return 1; }
    if (xTaskCreate(gpio_cycle_task, "gpio_cycle", 3072, (void *)(intptr_t)ms,
                     tskIDLE_PRIORITY + 1, &s_cycle_task) != pdPASS) {
        printf("xTaskCreate failed\n");
        return 1;
    }
    printf("cycling GPIO 36,39,34,35,32,33,25,26,27 at %dms/pin, forever "
           "(input-only pins 34/35/36/39 just get read, not driven) -- "
           "'gpio_cycle stop' to stop\n", ms);
    return 0;
}

static int cmd_tone(int argc, char **argv)
{
    if (argc < 3) { printf("usage: tone <hz> <ms>\n"); return 1; }
    int hz = atoi(argv[1]);
    int ms = atoi(argv[2]);
    if (hz <= 0 || ms <= 0) { printf("hz and ms must both be positive\n"); return 1; }
    bt_audio_io_test_tone(hz, ms);
    printf("playing %dHz for %dms on GPIO25 (bypasses Bluetooth/the C6 link entirely) "
           "-- if 25 or 26 were just used with gpio_set/gpio_cycle, reset the board "
           "first or this won't come out right, that reclaims them from GPIO mode\n",
           hz, ms);
    return 0;
}

void bt_gpio_test_register_commands(void)
{
    const esp_console_cmd_t tone_cmd = {
        .command = "tone",
        .help = "Play a test tone straight out GPIO25, no Bluetooth needed: tone <hz> <ms>",
        .func = cmd_tone,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&tone_cmd));

    const esp_console_cmd_t set_cmd = {
        .command = "gpio_set",
        .help = "Drive a GPIO high/low and read it back: gpio_set <pin> <0|1>",
        .func = cmd_gpio_set,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&set_cmd));

    const esp_console_cmd_t get_cmd = {
        .command = "gpio_get",
        .help = "Read a GPIO's current level: gpio_get <pin>",
        .func = cmd_gpio_get,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&get_cmd));

    const esp_console_cmd_t cycle_cmd = {
        .command = "gpio_cycle",
        .help = "Cycle through 36,39,34,35,32,33,25,26,27 forever, <ms> per pin: "
                "gpio_cycle <ms> | gpio_cycle stop",
        .func = cmd_gpio_cycle,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cycle_cmd));
}

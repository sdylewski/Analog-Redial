/*
 * app_main.c — ESP32-C6 Rotary Phone Gateway entry point.
 *
 * Boots all subsystems in dependency order, then drops into a serial console
 * that is BOTH the bring-up test harness and the live inspection tool. The
 * full telephony stack runs concurrently, so you can lift a handset and dial
 * while also issuing test commands.
 *
 * Build/flash/run: see firmware/README.md.
 */
#include "board.h"
#include "wm8731.h"
#include "audio.h"
#include "ag1170.h"
#include "dialplan.h"
#include "telephony.h"
#include "bringup.h"
#include "esp_log.h"
#include "esp_err.h"

static const char *TAG = "app";

void app_main(void)
{
    ESP_LOGI(TAG, "Rotary Phone Gateway (ESP32-C6) starting");

    /* 1. Shared buses. */
    ESP_ERROR_CHECK(board_i2c_init());

    /* 2. Audio engine first (sets the I2S master clocks the codec needs). */
    audio_config_t acfg = AUDIO_DEFAULT_CONFIG(); /* 48 kHz stereo */
    esp_err_t err = audio_init(&acfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "audio_init failed: %s (continuing for console tests)",
                 esp_err_to_name(err));
    }

    /* 3. Codec config over I2C. Non-fatal if the PCB/codec isn't present yet. */
    wm8731_config_t ccfg = WM8731_DEFAULT_CONFIG();
    ccfg.sample_rate_hz = audio_sample_rate() ? audio_sample_rate() : 48000;
    err = wm8731_init(&ccfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wm8731_init failed: %s (use 'i2c_scan'/'wm8731_init' "
                      "from the console once hardware is connected)",
                 esp_err_to_name(err));
    }

    /* 4. SLIC line interface. */
    ag1170_config_t scfg = AG1170_DEFAULT_CONFIG();
    ESP_ERROR_CHECK(ag1170_init(&scfg));

    /* 5. Telephony state machine (also inits dialplan + voip stub). */
    ESP_ERROR_CHECK(telephony_start());

    ESP_LOGI(TAG, "All subsystems up. Lift a handset to get dial tone.");

    /* 6. Bring-up / inspection console (does not return). */
    bringup_console_start();
}

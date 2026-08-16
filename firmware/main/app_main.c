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
#include "board_config.h"
#if CONFIG_BOARD_TYPE_SMD2
#include "es8388.h"
#else
#include "wm8731.h"
#endif
#include "audio.h"
#include "aec.h"
#include "ag1170.h"
#include "dialplan.h"
#include "telephony.h"
#include "wifi.h"
#include "storage.h"
#include "sdcard.h"
#include "logbuf.h"
#include "logctl.h"
#include "webui.h"
#include "bringup.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "app";

/* GPIO9 (BOOT) held ~3 s at runtime -> wipe WiFi creds and reboot, so the
 * device comes back up unprovisioned (SoftAP setup). See board.h. */
static void on_factory_reset(void)
{
    ESP_LOGW(TAG, "factory reset: erasing WiFi credentials and rebooting");
    board_led_set_state(BOARD_LED_ERROR);
    wifi_clear_credentials();
    vTaskDelay(pdMS_TO_TICKS(300)); /* let the NVS commit + log flush */
    esp_restart();
}

void app_main(void)
{
    /* Before anything else logs, so the ring buffer catches the whole boot
     * sequence, not just whatever happens after this point. */
    logbuf_init();
    /* Apply the saved runtime log level (default INFO) before subsystems
     * start logging -- lets a DEBUG level chosen from the web UI/console
     * persist across reboots (watch pulse/hook timing, etc.). */
    logctl_init();

    ESP_LOGI(TAG, "Rotary Phone Gateway (ESP32-C6) starting");

    /* 0. Status LED first -- fast blink means "booting", visible even if
     * something below hangs before reaching the "all subsystems up" log. */
    board_led_init();
    bool had_soft_failure = false;

    /* 1. Shared buses. */
    ESP_ERROR_CHECK(board_i2c_init());
    board_detect_codec(); /* sanity-check flashed board type vs. codec on the bus */

    /* 2. Audio engine first (sets the I2S master clocks the codec needs). */
    audio_config_t acfg = AUDIO_DEFAULT_CONFIG(); /* 48 kHz stereo */
    esp_err_t err = audio_init(&acfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "audio_init failed: %s (continuing for console tests)",
                 esp_err_to_name(err));
        had_soft_failure = true;
    }

    /* 3. Codec config over I2C. Non-fatal if the PCB/codec isn't present yet. */
#if CONFIG_BOARD_TYPE_SMD2
    es8388_config_t ccfg = ES8388_DEFAULT_CONFIG();
    ccfg.sample_rate_hz = audio_sample_rate() ? audio_sample_rate() : 48000;
    err = es8388_init(&ccfg);
#else
    wm8731_config_t ccfg = WM8731_DEFAULT_CONFIG();
    ccfg.sample_rate_hz = audio_sample_rate() ? audio_sample_rate() : 48000;
    err = wm8731_init(&ccfg);
#endif
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "codec init failed: %s (use 'i2c_scan'/'codec_init' "
                      "from the console once hardware is connected)",
                 esp_err_to_name(err));
        had_soft_failure = true;
    }

    /* 3b. Echo canceller state (starts disabled; enable per line at runtime
     * once a call is up -- see aec.h and the `aec` console command). */
    aec_init();

    /* 4. SLIC line interface. Line 2 vs. microSD on the shared GPIO 18-21
     * pads is a runtime choice now (board_hw_config_get(), board.h) rather
     * than a separate firmware build -- ag1170_init() itself reads that
     * config and only claims line 2's GPIO as a SLIC if it's actually
     * configured that way. */
    ag1170_config_t scfg = AG1170_DEFAULT_CONFIG();
    ESP_ERROR_CHECK(ag1170_init(&scfg));

    /* 5. Telephony state machine (also inits dialplan + voip stub). */
    ESP_ERROR_CHECK(telephony_start());

    /* LED now reflects "is the phone itself healthy" -- set it here, before
     * touching WiFi, so wifi_init() below is free to drive the LED through
     * its own connecting/connected states without this line stomping it
     * afterward (wifi_init() returns as soon as a connection attempt is
     * kicked off, well before it actually succeeds or fails). */
    board_led_set_state(had_soft_failure ? BOARD_LED_ERROR : BOARD_LED_OK);

    /* 6. Network: WiFi (+SNTP+mDNS), storage, web UI. None of these gate the
     * phone itself -- a WiFi/storage hiccup shouldn't take down telephony,
     * so just warn and carry on. wifi_init() drives the LED itself from
     * here (connecting/connected); storage/webui failures are rarer and not
     * worth their own LED state, so just force ERROR directly. */
    if ((err = wifi_init()) != ESP_OK) {
        ESP_LOGW(TAG, "wifi_init failed: %s", esp_err_to_name(err));
        board_led_set_state(BOARD_LED_ERROR);
    }
    if ((err = storage_init()) != ESP_OK) {
        ESP_LOGW(TAG, "storage_init failed: %s", esp_err_to_name(err));
        board_led_set_state(BOARD_LED_ERROR);
    }
    if (board_hw_config_get().has_microsd) {
        /* microSD configured (line 2 traded for the SD card, board.h): mount
         * it here so the boot log shows whether the card actually came up.
         * Non-fatal -- sdcard_init() itself no-ops harmlessly if this board
         * isn't actually configured for SD. */
        if ((err = sdcard_init()) != ESP_OK) {
            ESP_LOGW(TAG, "sdcard_init failed: %s (insert a FAT-formatted card, then 'sd_test')",
                     esp_err_to_name(err));
        }
    }
    if ((err = webui_start()) != ESP_OK) {
        ESP_LOGW(TAG, "webui_start failed: %s", esp_err_to_name(err));
        board_led_set_state(BOARD_LED_ERROR);
    }

    /* 7. BOOT-strap watcher: hold GPIO9 low ~3 s at runtime to factory-reset
     * WiFi. Started last so it can't interfere with subsystem bring-up. */
    board_factory_reset_start(on_factory_reset);

    ESP_LOGI(TAG, "All subsystems up. Lift a handset to get dial tone.");

    /* 8. Bring-up / inspection console (does not return). */
    bringup_console_start();
}

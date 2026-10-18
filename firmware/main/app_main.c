/*
 * app_main.c — Analog Redial (ESP32-C6) entry point.
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
#include "es8388.h"
#include "audio.h"
#include "aec.h"
#include "prompts.h"
#include "ag1170.h"
#include "dialplan.h"
#include "telephony.h"
#include "wifi.h"
#include "storage.h"
#include "sdcard.h"
#include "idle_power.h"
#include "bt_companion.h"
#include "esp_ota_ops.h"
#include "logbuf.h"
#include "logctl.h"
#include "activity.h"
#include "webui.h"
#include "webauth.h"
#include "webhook.h"
#include "mqtt_ha.h"
#include "bringup.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
#include "driver/usb_serial_jtag.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "app";

/* BOOT held ~3 s then released -> reboot into the setup SoftAP with the
 * saved credentials intact, so the web UI is reachable from a phone when the
 * board's usual network isn't. See board.h / wifi_request_setup_ap(). */
static void on_wifi_rescue(void)
{
    ESP_LOGW(TAG, "rescue: rebooting into the setup AP (credentials kept)");
    board_led_set_state(BOARD_LED_WIFI_CONNECTING);
    wifi_request_setup_ap();
    vTaskDelay(pdMS_TO_TICKS(300)); /* let the NVS commit + log flush */
    esp_restart();
}

/* BOOT held ~10 s -> wipe WiFi creds and reboot, so the device comes back up
 * unprovisioned. See board.h. */
static void on_factory_reset(void)
{
    ESP_LOGW(TAG, "factory reset: erasing WiFi credentials and the web password, rebooting");
    board_led_set_state(BOARD_LED_ERROR);
    wifi_clear_credentials();
    webauth_clear();
    vTaskDelay(pdMS_TO_TICKS(300)); /* let the NVS commit + log flush */
    esp_restart();
}

/* Physical/config safety guard: board_sd_line1_conflict() (board.h) is true
 * when a microSD card is mechanically present but this board is configured
 * for 2 lines -- GPIO18-21 are a real jumper (J5, docs/hardware.md "J5:
 * line-1 SLIC vs. microSD") between line 1's SLIC and the microSD socket,
 * so this isn't electrical contention on those lines (jumpered for line 1,
 * the card's data lines are simply disconnected) -- but it IS a real
 * physical/config mismatch: someone jumpered/configured this board for one
 * option and left a card meant for the other sitting in the socket, which
 * would otherwise show up only as "the SD card silently doesn't work."
 * Line 1 gets powered down here as a precaution -- not because the card
 * threatens it electrically, but because a board caught in this mismatched
 * state deserves a human's attention before carrying a real call, and
 * depowering makes "something is wrong" impossible to miss even without
 * the web UI (see webui.c's append_page_head() for the on-screen banner).
 * Polled rather than interrupt-driven since a card getting inserted/pulled
 * is a human-timescale event -- 2s is plenty responsive without spinning a
 * task needlessly. */
#define SD_LINE1_CONFLICT_POLL_MS 2000

static void sd_line1_conflict_task(void *arg)
{
    bool was_active = false;
    while (1) {
        bool active = board_sd_line1_conflict();
        if (active && !was_active) {
            ESP_LOGE(TAG, "microSD card present on a 2-line board -- disabling line 1 "
                          "until this is resolved (see Settings -> Hardware)");
            activity_log("Config mismatch: microSD card present on a 2-line board -- line 1 disabled");
            board_led_set_state(BOARD_LED_ERROR);
            ag1170_set_power(1, false);
        } else if (!active && was_active) {
            ESP_LOGW(TAG, "microSD/line-1 conflict cleared -- restoring line 1");
            activity_log("Config mismatch cleared -- line 1 re-enabled");
            board_led_set_state(BOARD_LED_OK);
            if (board_hw_config_get().num_lines == 2) ag1170_set_power(1, true);
        }
        was_active = active;
        vTaskDelay(pdMS_TO_TICKS(SD_LINE1_CONFLICT_POLL_MS));
    }
}

/* ---- Black box ----
 * The log lives in RAM, so whatever happened during a session the board
 * couldn't report from (off the network, then unplugged) is gone by the time
 * it's plugged in somewhere it can be read. So the log is also saved to
 * internal flash as blackbox.log: every 30 s for the first 10 minutes after
 * start-up (where power-supply trouble shows up), and once a minute while
 * Wi-Fi is down. At start-up the previous session's copy is kept as
 * blackbox_prev.log. Both download from Settings -> Storage. Internal flash,
 * not the SD card: a sagging supply is exactly what upsets the card. */
#define BLACKBOX_NAME        "blackbox.log"
#define BLACKBOX_PREV_NAME   "blackbox_prev.log"
#define BLACKBOX_EARLY_MS    (10 * 60 * 1000)
#define BLACKBOX_EARLY_EVERY 30000
#define BLACKBOX_DOWN_EVERY  60000
#define BLACKBOX_CAP         (8192 + 256 + 1536)

static bool blackbox_write(const char *name, const char *buf, size_t n)
{
    storage_writer_t w;
    if (storage_write_begin_local(name, n, &w) != ESP_OK) return false;
    if (storage_write_chunk(&w, buf, n) != ESP_OK) { storage_write_abort(&w); return false; }
    return storage_write_end(&w) == ESP_OK;
}

/* CPU use per task since the previous save -- if something hogs the CPU
 * (the symptom: low-priority work like the web server just stops), this
 * names it. Also whether a computer is on the USB port. */
static size_t blackbox_task_stats(char *out, size_t cap)
{
    static struct { TaskHandle_t h; uint32_t rt; } prev[40];
    static uint32_t prev_total;
    size_t off = 0;
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
    UBaseType_t n = uxTaskGetNumberOfTasks();
    TaskStatus_t *ts = malloc(sizeof(TaskStatus_t) * (n + 4));
    if (!ts) return 0;
    uint32_t total = 0;
    n = uxTaskGetSystemState(ts, n + 4, &total);
    uint32_t dt = total - prev_total;
    off += snprintf(out + off, cap - off, "usb host: %s; cpu since last save:",
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
                    usb_serial_jtag_is_connected() ? "yes" : "no"
#else
                    "?"
#endif
                    );
    for (UBaseType_t i = 0; i < n && off < cap - 40; i++) {
        uint32_t before = 0;
        for (size_t k = 0; k < sizeof(prev) / sizeof(prev[0]); k++)
            if (prev[k].h == ts[i].xHandle) { before = prev[k].rt; break; }
        uint32_t used = ts[i].ulRunTimeCounter - before;
        unsigned pct = dt ? (unsigned)((uint64_t)used * 100 / dt) : 0;
        if (pct >= 1) off += snprintf(out + off, cap - off, " %s %u%%", ts[i].pcTaskName, pct);
    }
    off += snprintf(out + off, cap - off, "\n");
    for (UBaseType_t i = 0; i < n && i < sizeof(prev) / sizeof(prev[0]); i++) {
        prev[i].h = ts[i].xHandle;
        prev[i].rt = ts[i].ulRunTimeCounter;
    }
    prev_total = total;
    free(ts);
#endif
    if (off >= cap) off = cap - 1;
    return off;
}

static void blackbox_save(const char *why)
{
    char *buf = malloc(BLACKBOX_CAP);
    if (!buf) return;
    board_restart_stats_t rs;
    board_restart_stats_get(&rs);
    int hn = snprintf(buf, 256,
        "=== black box (%s): start-up #%u, up %lld s, Wi-Fi %s (RSSI %d, TX %d dBm), "
        "heap %u free (min %u, largest %u) ===\n",
        why, (unsigned)rs.boots, (long long)(esp_timer_get_time() / 1000000),
        wifi_is_connected() ? "connected" : "down", wifi_rssi(), wifi_get_tx_power_dbm(),
        (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size(),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    if (hn < 0 || hn >= 256) hn = 0;
    hn += blackbox_task_stats(buf + hn, 1536);
    size_t n = hn + logbuf_snapshot(buf + hn, BLACKBOX_CAP - hn);
    blackbox_write(BLACKBOX_NAME, buf, n);
    free(buf);
}

/* The previous session's black box becomes blackbox_prev.log, before this
 * session's first save overwrites it. */
static void blackbox_rotate(void)
{
    char *buf = malloc(BLACKBOX_CAP);
    if (!buf) return;
    size_t n = 0;
    if (storage_read(BLACKBOX_NAME, buf, BLACKBOX_CAP, &n) == ESP_OK && n > 0)
        blackbox_write(BLACKBOX_PREV_NAME, buf, n);
    free(buf);
}

static void blackbox_task(void *arg)
{
    blackbox_rotate();
    bool was_up = false;
    int64_t down_since = 0, last_save = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        int64_t now = esp_timer_get_time() / 1000;
        bool up = wifi_is_connected();
        if (up) { was_up = true; down_since = 0; }
        else if (was_up && !down_since) {
            down_since = now;
            ESP_LOGW(TAG, "Wi-Fi lost; saving the log to %s", BLACKBOX_NAME);
        }
        int64_t every = 0;
        if (now < BLACKBOX_EARLY_MS) every = BLACKBOX_EARLY_EVERY;
        else if (down_since) every = BLACKBOX_DOWN_EVERY;
        if (every && now - last_save >= every) {
            blackbox_save(down_since ? "Wi-Fi down" : "periodic");
            last_save = now;
        }
    }
}

static void power_event_cb(void *arg)
{
    (void)arg;
    webhook_event("power", 0, "restarted after a brownout");
}

void app_main(void)
{
    /* Before anything else logs, so the ring buffer catches the whole boot
     * sequence, not just whatever happens after this point. */
    logbuf_init();
    /* Structured "what just happened" ring buffer for the web UI's Status
     * page (calls, recordings, dial errors...); independent of log level. */
    activity_init();
    {
        /* Why the last restart happened, so a crash, watchdog or brownout
         * (e.g. a weak supply while ringing) leaves a trace on the Status
         * page instead of looking like a board that just restarted. */
        esp_reset_reason_t rr = esp_reset_reason();
        const char *why = rr == ESP_RST_PANIC ? "a crash" :
                          rr == ESP_RST_INT_WDT || rr == ESP_RST_TASK_WDT || rr == ESP_RST_WDT
                              ? "a watchdog timeout" :
                          rr == ESP_RST_BROWNOUT ? "a power brownout (supply too weak?)" : NULL;
        if (why) {
            ESP_LOGW(TAG, "restarted after %s (reset reason %d)", why, (int)rr);
            activity_log("Restarted after %s", why);
        }
    }
    /* Apply the saved runtime log level (default INFO) before subsystems
     * start logging -- lets a DEBUG level chosen from the web UI/console
     * persist across reboots (watch pulse/hook timing, etc.). */
    logctl_init();
    /* Count this boot and its reason in NVS (needs NVS, which logctl_init()
     * brought up): the RAM log above doesn't survive a power cycle. */
    board_restart_record();

    /* Applies the saved TZ string (Settings -> General) to the C library's
     * timezone, if any -- board_quiet_hours_active() is the only thing that
     * calls localtime_r() rather than gmtime_r(), but it needs this done
     * before the first incoming call, not lazily. logctl_init() above
     * already brought NVS up. */
    board_timezone_init();

    ESP_LOGI(TAG, "Analog Redial (ESP32-C6) starting");

    /* Detect a firmware/physical-board mismatch (e.g. SMD4 firmware on a
     * hand-soldered SMD2 unit) before touching any board-specific GPIO --
     * see board_type_check()'s own doc comment (board.h). */
    board_type_check();

    /* 0. Status LED first -- fast blink means "booting", visible even if
     * something below hangs before reaching the "all subsystems up" log. */
    board_led_init();
    bool had_soft_failure = false;
    if (board_type_mismatch()) {
        ESP_LOGE(TAG, "continuing boot despite the board type mismatch above so the web "
                      "UI/console stay reachable to fix it");
        had_soft_failure = true;
    }

    /* 1. Shared buses. */
    board_early_pin_init();
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
    es8388_config_t ccfg = ES8388_DEFAULT_CONFIG();
    ccfg.sample_rate_hz = audio_sample_rate() ? audio_sample_rate() : 48000;
    err = es8388_init(&ccfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "codec init failed: %s (use 'i2c_scan'/'codec_init' "
                      "from the console once hardware is connected)",
                 esp_err_to_name(err));
        had_soft_failure = true;
    }

    /* 3b. Echo canceller state (starts disabled; enable per line at runtime
     * once a call is up -- see aec.h and the `aec` console command). */
    aec_init();
    prompts_init();

    /* 3c. Best-effort hardware auto-detect -- only ever does anything on a
     * truly fresh board/factory-reset (no-op once any value, guessed or
     * confirmed, already exists). Must run before ag1170_init() below,
     * which is the first thing that actually claims GPIO 18-21 based on
     * whatever board_hw_config_get() says. */
    board_hw_config_autodetect();

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
    } else {
        telephony_check_clip_files();
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
        /* Picks up a card inserted/removed after this point -- sdcard_init()
         * above only ever runs this once, at boot. No-op on boards with no
         * card-detect pin to watch (see sdcard_hotplug_start()'s own doc
         * comment). */
        sdcard_hotplug_start();
    }
    webauth_init();
    webhook_init();
    mqtt_ha_init();
    if (esp_reset_reason() == ESP_RST_BROWNOUT) {
        /* Tell Home Assistant & co. once the network has had time to come
         * up: the webhook doesn't retry, so an event queued now would be
         * lost before Wi-Fi connects. */
        static esp_timer_handle_t t;
        const esp_timer_create_args_t a = { .callback = power_event_cb, .name = "pwr_evt" };
        if (esp_timer_create(&a, &t) == ESP_OK) esp_timer_start_once(t, 20 * 1000000ULL);
    }
    if ((err = webui_start()) != ESP_OK) {
        ESP_LOGW(TAG, "webui_start failed: %s", esp_err_to_name(err));
        board_led_set_state(BOARD_LED_ERROR);
    }

    /* Optional low-power idle mode (disabled by default) -- after ag1170,
     * telephony/voip and wifi so it has everything it needs to watch.
     * See idle_power.h and archive/POWER_SAVING.md. */
    idle_power_init();

    /* Optional Bluetooth Classic audio companion board (disabled by
     * default -- no hardware exists on any board yet). See
     * archive/BLUETOOTH_AUDIO_UPGRADE.md and bt_companion.h. */
    bt_companion_init();

    /* 7. BOOT-button watcher: ~3 s then release brings up the setup AP with
     * credentials intact; ~10 s factory-resets WiFi. Started last so it
     * can't interfere with subsystem bring-up. */
    board_boot_button_start(on_wifi_rescue, on_factory_reset);

    /* microSD/line-1 physical config guard -- see sd_line1_conflict_task()'s
     * own doc comment above. After ag1170_init() so line 1's SLIC (if
     * configured) has already claimed its GPIO before this might power it
     * back down/up. */
    xTaskCreate(sd_line1_conflict_task, "sd_line1_guard", 3072, NULL, tskIDLE_PRIORITY + 1, NULL);
    /* High priority so it still runs if something lower is hogging the CPU;
     * it only wakes once a second and writes every 30 s at most. */
    xTaskCreate(blackbox_task, "blackbox", 4096, NULL, tskIDLE_PRIORITY + 15, NULL);

    /* Confirms this image is good, canceling CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE's
     * pending auto-revert -- everything that could plausibly fail to come up
     * (audio/codec, SLIC, telephony, WiFi/webui) has already run by this
     * point, so reaching here is a real enough health signal. A firmware
     * pushed via Settings -> Hardware -> Firmware that crashes or hangs
     * before this line stays un-confirmed, and the bootloader reverts to
     * the previous slot on the next boot instead of leaving a board that
     * isn't physically reachable stuck on bad firmware. No-op (returns
     * ESP_ERR_INVALID_STATE, harmlessly ignored) when rollback isn't
     * pending -- i.e. every normal boot after the first one following an
     * update. */
    esp_ota_mark_app_valid_cancel_rollback();

    ESP_LOGI(TAG, "All subsystems up. Lift a handset to get dial tone.");

    /* 8. Bring-up / inspection console (does not return). */
    bringup_console_start();
}

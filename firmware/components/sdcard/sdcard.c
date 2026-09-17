#include "sdcard.h"
#include "sdkconfig.h"
#include "board.h"
#include "board_pins.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "sdcard";
#define SD_MOUNT "/sd"

static sdmmc_card_t *s_card;

#ifdef BOARD_SD_CD
static bool s_cd_configured;

/* Standard push-push microSD socket convention: a normally-open mechanical
 * switch that shorts CD to GND once a card is fully seated -- LOW = present,
 * HIGH (via the internal weak pull-up, ~45k on this chip -- plenty for a
 * slow mechanical contact, no external resistor needed) = empty. */
static void ensure_cd_configured(void)
{
    if (s_cd_configured) return;
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BOARD_SD_CD,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&cfg);
    s_cd_configured = true;
}
#endif

bool sdcard_card_present(void)
{
#ifdef BOARD_SD_CD
    ensure_cd_configured();
    return gpio_get_level(BOARD_SD_CD) == 0;
#else
    return true; /* no CD pin on this board -- can't tell, so don't block */
#endif
}

/* True if the LAST failed mount attempt got far enough to talk to the card
 * over SPI (sdmmc_card_init() succeeded) but found no valid FAT filesystem
 * on it -- i.e. a formatting problem (exFAT, a multi-partition card like a
 * Raspberry Pi OS image, or a blank/corrupted card), not a wiring/hardware
 * one. Distinguishing these two matters: esp_vfs_fat_sdspi_mount() returns
 * a SPECIFIC esp_err_t (ESP_ERR_INVALID_CRC, ESP_ERR_TIMEOUT, etc.) when the
 * card-communication step itself fails, but collapses every filesystem-
 * mount failure (wrong FS, corrupt FS) down to a generic ESP_FAIL (see
 * s_f_mount() in ESP-IDF's vfs_fat_sdmmc.c) -- so ESP_FAIL specifically
 * (and only that code) means "the card works, the filesystem doesn't."
 * False on a successful mount and on every OTHER failure (no card, SPI/CRC
 * error, etc.) -- those are hardware problems, not formatting ones. */
static bool s_needs_format;

bool sdcard_needs_format(void) { return s_needs_format; }

static esp_err_t do_mount(bool format_if_failed)
{
    /* microSD is a runtime board config now (board.h), not a separate
     * firmware build -- GPIO18-21 might be wired to line 2's SLIC instead
     * on this specific board, so bail out before touching any SPI/GPIO
     * hardware at all if that's not what's configured. */
    if (!board_hw_config_get().has_microsd) return ESP_ERR_NOT_SUPPORTED;

    if (!sdcard_card_present()) {
        ESP_LOGW(TAG, "no card detected (CD pin) -- not attempting a mount");
        return ESP_ERR_NOT_FOUND;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;
    /* SDSPI_HOST_DEFAULT() leaves this at SDMMC_FREQ_DEFAULT (20MHz).
     * SDMMC_FREQ_HIGHSPEED (40MHz) is a standard ESP-IDF value, not an
     * aggressive/unsupported overclock -- most cards handle it fine even
     * over SPI. This helps raw transfer time, not the bigger cost on this
     * board (SPI-mode's per-file-open protocol overhead, unrelated to
     * clock rate -- see webui.c's STORAGE_LIST_MAX/wav_duration_str() doc
     * comments for the fixes that actually target that), but it's free. */
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;

    spi_bus_config_t bus = {
        .mosi_io_num = BOARD_SD_MOSI,
        .miso_io_num = BOARD_SD_MISO,
        .sclk_io_num = BOARD_SD_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };
    esp_err_t err = spi_bus_initialize(host.slot, &bus, SDSPI_DEFAULT_DMA);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) { /* INVALID_STATE = already init */
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
        return err;
    }

    sdspi_device_config_t dev = SDSPI_DEVICE_CONFIG_DEFAULT();
    dev.gpio_cs  = BOARD_SD_CS;
    dev.host_id  = host.slot;

    esp_vfs_fat_sdmmc_mount_config_t mnt = {
        .format_if_mount_failed = format_if_failed, /* default false -- don't wipe a card
                                                       * that just isn't FAT; true only via
                                                       * sdcard_force_format(), an explicit,
                                                       * user-initiated action */
        .max_files = 6,
        .allocation_unit_size = 16 * 1024,
    };

    err = esp_vfs_fat_sdspi_mount(SD_MOUNT, &host, &dev, &mnt, &s_card);
    if (err != ESP_OK) {
        s_needs_format = (err == ESP_FAIL);
        if (s_needs_format) {
            ESP_LOGW(TAG, "SD card responds fine over SPI, but has no valid FAT filesystem -- "
                          "wrong format (exFAT, a multi-partition card e.g. a Raspberry Pi OS "
                          "image, or blank/corrupted) -- reformat it as a single FAT32 partition");
        } else {
            ESP_LOGW(TAG, "SD mount failed: %s -- likely wiring/hardware, not a formatting "
                          "issue (CS=%d SCLK=%d MOSI=%d MISO=%d)",
                     esp_err_to_name(err), BOARD_SD_CS, BOARD_SD_SCLK, BOARD_SD_MOSI, BOARD_SD_MISO);
        }
        s_card = NULL;
        return err;
    }
    s_needs_format = false;

    uint64_t cap = (uint64_t)s_card->csd.capacity * s_card->csd.sector_size;
    ESP_LOGI(TAG, "SD mounted at %s: '%s' %lluMB, %d-bit, %d kHz",
             SD_MOUNT, s_card->cid.name, (unsigned long long)(cap / (1024 * 1024)),
             s_card->log_bus_width ? (1 << s_card->log_bus_width) : 1,
             s_card->max_freq_khz);
    return ESP_OK;
}

esp_err_t sdcard_init(void)
{
    if (s_card) return ESP_OK;
    return do_mount(false);
}

/* Explicit, user-initiated "format this card and mount it" -- for a card
 * that responds over SPI but has no valid FAT filesystem (see
 * sdcard_needs_format()'s doc comment), where the normal sdcard_format()
 * can't help because it requires an already-mounted card. Real data loss:
 * this WILL overwrite whatever is on the card, valid filesystem or not --
 * the web UI gates this behind its own confirm() dialog, same as
 * sdcard_format(). ESP_ERR_INVALID_STATE if already mounted (use
 * sdcard_format() there instead) or no card present. */
esp_err_t sdcard_force_format(void)
{
    if (s_card) return ESP_ERR_INVALID_STATE;
    return do_mount(true);
}

bool sdcard_mounted(void) { return s_card != NULL; }

void sdcard_deinit(void)
{
    if (!s_card) return;
    esp_vfs_fat_sdcard_unmount(SD_MOUNT, s_card);
    s_card = NULL;
}

#ifdef BOARD_SD_CD
#define SD_HOTPLUG_POLL_MS 1500

static void hotplug_task(void *arg)
{
    /* Starts from whatever sdcard_init() already established at boot (a
     * card present at boot is already mounted by the time this task's first
     * loop runs), so it only reacts to a CHANGE from here on, never
     * redundantly re-triggers the boot-time state it was handed. */
    bool was_present = sdcard_card_present();
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(SD_HOTPLUG_POLL_MS));
        bool present = sdcard_card_present();
        if (present && !was_present) {
            ESP_LOGI(TAG, "card inserted -- mounting");
            esp_err_t err = sdcard_init();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "mount after insertion failed: %s", esp_err_to_name(err));
            }
        } else if (!present && was_present) {
            ESP_LOGW(TAG, "card removed");
            sdcard_deinit();
        }
        was_present = present;
    }
}
#endif

void sdcard_hotplug_start(void)
{
#ifdef BOARD_SD_CD
    if (!board_hw_config_get().has_microsd) return;
    xTaskCreate(hotplug_task, "sd_hotplug", 3072, NULL, tskIDLE_PRIORITY + 1, NULL);
#endif
}

esp_err_t sdcard_capacity(uint64_t *total_bytes, uint64_t *free_bytes)
{
    if (!s_card) return ESP_ERR_INVALID_STATE;
    return esp_vfs_fat_info(SD_MOUNT, total_bytes, free_bytes);
}

void sdcard_print_info(void)
{
    if (!board_hw_config_get().has_microsd) {
        printf("SD: this board isn't configured for microSD (Settings -> "
               "Hardware, or it's set to 2-line mode -- GPIO18-21 are "
               "line 2's SLIC instead).\n");
        return;
    }
    if (!s_card) { printf("SD: not mounted\n"); return; }
    sdmmc_card_print_info(stdout, s_card);
    uint64_t total = 0, freeb = 0;
    if (esp_vfs_fat_info(SD_MOUNT, &total, &freeb) == ESP_OK) {
        printf("FAT: %llu MB total, %llu MB free\n",
               (unsigned long long)(total / (1024 * 1024)),
               (unsigned long long)(freeb / (1024 * 1024)));
    }
}

bool sdcard_get_info(sdcard_info_t *out)
{
    if (!s_card || !out) return false;
    memset(out->name, 0, sizeof(out->name));
    memcpy(out->name, s_card->cid.name, sizeof(s_card->cid.name));
    out->mfg_id = s_card->cid.mfg_id;
    out->capacity_bytes = (uint64_t)s_card->csd.capacity * (uint64_t)s_card->csd.sector_size;
    out->max_freq_khz = s_card->max_freq_khz;
    out->real_freq_khz = s_card->real_freq_khz;
    out->bus_width = s_card->log_bus_width ? (1 << s_card->log_bus_width) : 1;
    return true;
}

#define SELFTEST_BYTES (128 * 1024)

esp_err_t sdcard_selftest(float *write_kbps, float *read_kbps)
{
    if (write_kbps) *write_kbps = 0;
    if (read_kbps) *read_kbps = 0;
    if (!s_card) { printf("SD: not mounted\n"); return ESP_ERR_INVALID_STATE; }
    const char *path = SD_MOUNT "/_sdtest.bin";

    uint8_t *buf = malloc(SELFTEST_BYTES);
    if (!buf) { printf("SD: out of memory for self-test buffer\n"); return ESP_FAIL; }
    /* A repeating-but-not-constant pattern -- catches a stuck-bit or
     * torn-write failure a run of all-zero/all-0xFF bytes could hide. */
    for (size_t i = 0; i < SELFTEST_BYTES; i++) buf[i] = (uint8_t)(i * 31 + 7);

    int64_t t0 = esp_timer_get_time();
    FILE *f = fopen(path, "wb");
    if (!f) { printf("SD: open-for-write failed\n"); free(buf); return ESP_FAIL; }
    size_t wrote = fwrite(buf, 1, SELFTEST_BYTES, f);
    fclose(f);
    int64_t t1 = esp_timer_get_time();
    if (wrote != SELFTEST_BYTES) {
        printf("SD: short write (%u/%u bytes)\n", (unsigned)wrote, SELFTEST_BYTES);
        unlink(path);
        free(buf);
        return ESP_FAIL;
    }
    if (write_kbps) *write_kbps = (float)SELFTEST_BYTES / 1024.0f / ((float)(t1 - t0) / 1000000.0f);

    uint8_t *readback = malloc(SELFTEST_BYTES);
    if (!readback) { printf("SD: out of memory for readback buffer\n"); unlink(path); free(buf); return ESP_FAIL; }
    int64_t t2 = esp_timer_get_time();
    f = fopen(path, "rb");
    if (!f) { printf("SD: open-for-read failed\n"); unlink(path); free(buf); free(readback); return ESP_FAIL; }
    size_t got = fread(readback, 1, SELFTEST_BYTES, f);
    fclose(f);
    int64_t t3 = esp_timer_get_time();
    unlink(path);
    if (read_kbps) *read_kbps = (float)got / 1024.0f / ((float)(t3 - t2) / 1000000.0f);

    bool match = (got == SELFTEST_BYTES) && (memcmp(buf, readback, SELFTEST_BYTES) == 0);
    free(buf);
    free(readback);
    if (!match) {
        printf("SD: readback mismatch (%u/%u bytes, content %s)\n", (unsigned)got, SELFTEST_BYTES,
               (got == SELFTEST_BYTES) ? "differs" : "short");
        return ESP_FAIL;
    }
    printf("SD: write+read+verify OK (%.0f KB/s write, %.0f KB/s read)\n",
           write_kbps ? *write_kbps : 0, read_kbps ? *read_kbps : 0);
    return ESP_OK;
}

esp_err_t sdcard_format(void)
{
    if (!s_card) return ESP_ERR_INVALID_STATE;
    ESP_LOGW(TAG, "formatting SD card at %s -- all data on it is about to be erased", SD_MOUNT);
    return esp_vfs_fat_sdcard_format(SD_MOUNT, s_card);
}

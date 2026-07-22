#include "sdcard.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include <stdio.h>

static const char *TAG = "sdcard";
#define SD_MOUNT "/sd"

#if CONFIG_BOARD_MICROSD

#include "board_pins_smd.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include <string.h>
#include <unistd.h>

static sdmmc_card_t *s_card;

esp_err_t sdcard_init(void)
{
    if (s_card) return ESP_OK;

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;

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
        .format_if_mount_failed = false, /* don't wipe a card that just isn't FAT */
        .max_files = 6,
        .allocation_unit_size = 16 * 1024,
    };

    err = esp_vfs_fat_sdspi_mount(SD_MOUNT, &host, &dev, &mnt, &s_card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD mount failed: %s -- card inserted & FAT-formatted? "
                      "(CS=%d SCLK=%d MOSI=%d MISO=%d)",
                 esp_err_to_name(err), BOARD_SD_CS, BOARD_SD_SCLK, BOARD_SD_MOSI, BOARD_SD_MISO);
        s_card = NULL;
        return err;
    }

    uint64_t cap = (uint64_t)s_card->csd.capacity * s_card->csd.sector_size;
    ESP_LOGI(TAG, "SD mounted at %s: '%s' %lluMB, %d-bit, %d kHz",
             SD_MOUNT, s_card->cid.name, (unsigned long long)(cap / (1024 * 1024)),
             s_card->log_bus_width ? (1 << s_card->log_bus_width) : 1,
             s_card->max_freq_khz);
    return ESP_OK;
}

bool sdcard_mounted(void) { return s_card != NULL; }

void sdcard_print_info(void)
{
    if (!s_card) { printf("SD: not mounted\n"); return; }
    sdmmc_card_print_info(stdout, s_card);
    uint64_t total = 0, freeb = 0;
    if (esp_vfs_fat_info(SD_MOUNT, &total, &freeb) == ESP_OK) {
        printf("FAT: %llu MB total, %llu MB free\n",
               (unsigned long long)(total / (1024 * 1024)),
               (unsigned long long)(freeb / (1024 * 1024)));
    }
}

esp_err_t sdcard_selftest(void)
{
    if (!s_card) { printf("SD: not mounted\n"); return ESP_ERR_INVALID_STATE; }
    const char *path = SD_MOUNT "/_sdtest.txt";
    const char *want = "rotary-phone sd round-trip\n";

    FILE *f = fopen(path, "w");
    if (!f) { printf("SD: open-for-write failed\n"); return ESP_FAIL; }
    fputs(want, f);
    fclose(f);

    char buf[64] = {0};
    f = fopen(path, "r");
    if (!f) { printf("SD: open-for-read failed\n"); return ESP_FAIL; }
    fgets(buf, sizeof(buf), f);
    fclose(f);
    unlink(path);

    if (strcmp(buf, want) != 0) {
        printf("SD: readback mismatch ('%s')\n", buf);
        return ESP_FAIL;
    }
    printf("SD: write+read+verify OK\n");
    return ESP_OK;
}

#else /* !CONFIG_BOARD_MICROSD */

esp_err_t sdcard_init(void) { return ESP_ERR_NOT_SUPPORTED; }
bool sdcard_mounted(void) { return false; }
void sdcard_print_info(void)
{
    printf("SD: this firmware isn't built with CONFIG_BOARD_MICROSD "
           "(dual-line SLIC build). Rebuild with microSD enabled + 1 line.\n");
}
esp_err_t sdcard_selftest(void) { return ESP_ERR_NOT_SUPPORTED; }

#endif

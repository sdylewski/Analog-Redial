/*
 * sdcard.h — microSD (SPI mode) mount + bring-up, for the SMD build that
 * trades Line 2 for an SD card (CONFIG_BOARD_MICROSD, GPIO 18-21).
 *
 * SPI-mode SD via the shared SD pads: CS=18, SCLK=19, MOSI=20, MISO=21
 * (BOARD_SD_* in board_pins_smd.h). Mounts a FAT filesystem at /sd.
 *
 * All calls are no-op stubs when the firmware isn't built with
 * CONFIG_BOARD_MICROSD, so app_main/console can reference them unconditionally.
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bring up the SPI bus + card and mount FAT at /sd. Logs card info on success.
 * ESP_ERR_NOT_SUPPORTED if built without CONFIG_BOARD_MICROSD. */
esp_err_t sdcard_init(void);

bool sdcard_mounted(void);

/* Print card details (type, capacity) + FAT total/free to stdout (console). */
void sdcard_print_info(void);

/* Write a temp file, read it back, verify, delete. ESP_OK if the card
 * round-trips. */
esp_err_t sdcard_selftest(void);

#ifdef __cplusplus
}
#endif

/*
 * sdcard.h — microSD (SPI mode) mount + bring-up, for SMD boards that trade
 * Line 2 for an SD card on the shared GPIO 18-21 pads. Runtime choice now
 * (board_hw_config_get().has_microsd, see board.h), not a separate build --
 * sdcard_init() itself no-ops with ESP_ERR_NOT_SUPPORTED on a board that
 * isn't configured for SD, so callers can invoke it unconditionally.
 *
 * SPI-mode SD via the shared SD pads: CS=18, SCLK=19, MOSI=20, MISO=21
 * (BOARD_SD_* in board_pins_smd.h). Mounts a FAT filesystem at /sd.
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bring up the SPI bus + card and mount FAT at /sd. Logs card info on success.
 * ESP_ERR_NOT_SUPPORTED if this board isn't configured for microSD. */
esp_err_t sdcard_init(void);

bool sdcard_mounted(void);

/* Total/free bytes on the mounted card, straight from FATFS's own
 * bookkeeping (not a directory walk -- storage.c uses this for the SD
 * listing's capacity display, and needs it fast and accurate for real free
 * space, not just "sum of files we happen to know about"). uint64_t, not
 * size_t: a card can easily exceed 4 GB, which would silently wrap on this
 * chip's 32-bit size_t. ESP_ERR_INVALID_STATE if not mounted. */
esp_err_t sdcard_capacity(uint64_t *total_bytes, uint64_t *free_bytes);

/* Print card details (type, capacity) + FAT total/free to stdout (console). */
void sdcard_print_info(void);

/* Write a temp file, read it back, verify, delete. ESP_OK if the card
 * round-trips. */
esp_err_t sdcard_selftest(void);

#ifdef __cplusplus
}
#endif

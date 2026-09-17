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
 * ESP_ERR_NOT_SUPPORTED if this board isn't configured for microSD.
 * ESP_ERR_NOT_FOUND if the board has a card-detect pin (BOARD_SD_CD) and it
 * reads "no card" -- checked before touching SPI at all, so a genuinely
 * empty slot fails fast with a clear reason instead of a mount timeout. */
esp_err_t sdcard_init(void);

bool sdcard_mounted(void);

/* True if the last failed mount attempt talked to the card fine over SPI
 * but found no valid FAT filesystem on it -- a formatting problem (exFAT, a
 * multi-partition card like a Raspberry Pi OS image, or blank/corrupted),
 * NOT a wiring/hardware one. Lets the web UI tell those two apart instead
 * of showing the same generic "mount failed" for both. False after a
 * successful mount, and false for every other failure (no card, CRC/comms
 * error, etc.) -- see do_mount()'s doc comment in sdcard.c for exactly how
 * this is derived from ESP-IDF's own (otherwise-generic) error code. */
bool sdcard_needs_format(void);

/* Explicit, user-initiated format-and-mount for a card sdcard_needs_format()
 * flagged -- sdcard_format() below can't help there since it requires an
 * already-mounted card. Real, irreversible data loss on whatever's on the
 * card; the web UI gates this behind its own confirm() dialog, same as
 * sdcard_format(). ESP_ERR_INVALID_STATE if already mounted (use
 * sdcard_format() instead) or if no card is present. */
esp_err_t sdcard_force_format(void);

/* Unmounts and releases the card handle -- the counterpart sdcard_init()
 * itself doesn't have, needed so a hot-removed card (sdcard_hotplug_start(),
 * below) doesn't leave sdcard_mounted() reporting stale "yes" state against
 * a card that's physically gone, or leave the SPI/FATFS handle open for
 * whatever gets inserted next. No-op if not currently mounted. */
void sdcard_deinit(void);

/* Starts a small background task that watches the card-detect pin (on
 * boards that have one -- BOARD_SD_CD, board_pins_smd4.h) and calls
 * sdcard_init()/sdcard_deinit() on insertion/removal, so a card swapped in
 * after boot gets picked up within a couple of seconds instead of needing a
 * reboot. No-op if this board isn't configured for microSD, or has no CD
 * pin to watch (sdcard_card_present() can't tell "present" from "absent"
 * there, so there's nothing to poll for -- see that function's own doc
 * comment). Safe to call unconditionally from app startup, same as
 * sdcard_init() itself. */
void sdcard_hotplug_start(void);

/* True if a card is physically in the slot, per the board's CD (card
 * detect) pin -- independent of sdcard_init()/sdcard_mounted(), so callers
 * (webui status, sdcard_init() itself) can tell "no card" apart from "card
 * present but mount failed" before attempting anything on the SPI bus.
 * Boards with no CD pin wired (BOARD_SD_CD undefined -- true today for every
 * board except where noted in its board_pins header) always return true
 * here: no hardware signal to distinguish "empty" from "not checked yet",
 * so this degrades to the same "just try the mount" behavior those boards
 * already had before CD support existed. */
bool sdcard_card_present(void);

/* Total/free bytes on the mounted card, straight from FATFS's own
 * bookkeeping (not a directory walk -- storage.c uses this for the SD
 * listing's capacity display, and needs it fast and accurate for real free
 * space, not just "sum of files we happen to know about"). uint64_t, not
 * size_t: a card can easily exceed 4 GB, which would silently wrap on this
 * chip's 32-bit size_t. ESP_ERR_INVALID_STATE if not mounted. */
esp_err_t sdcard_capacity(uint64_t *total_bytes, uint64_t *free_bytes);

/* Print card details (type, capacity) + FAT total/free to stdout (console). */
void sdcard_print_info(void);

/* Raw identification fields for the web UI's card-info display -- the same
 * CID/CSD data sdcard_print_info() already prints to console, just handed
 * back structured instead of formatted. `name` is the card's own embedded
 * product string (CID register, up to 8 chars -- not a marketing name like
 * "Extreme", just whatever the manufacturer programmed there, often
 * something like "SD32G"); `mfg_id`/`capacity_bytes` are what the web UI
 * derives a manufacturer name and SDSC/SDHC/SDXC class from (see webui.c).
 * `bus_width` is 1 on every board today -- SPI mode, not native SD 4-bit --
 * so `max_freq_khz`/`real_freq_khz` (the card's rated max vs. what's
 * actually negotiated) tell you more about real throughput here than bus
 * width would. Returns false (leaving *out untouched) if not mounted. */
typedef struct {
    char     name[9];
    int      mfg_id;
    uint64_t capacity_bytes;
    int      max_freq_khz;
    int      real_freq_khz;
    int      bus_width;
} sdcard_info_t;
bool sdcard_get_info(sdcard_info_t *out);

/* Write, read back, verify, and time a 128KB round-trip (large enough for a
 * meaningful throughput figure, small enough to run inline in an HTTP
 * handler without a long page hang) -- deletes the temp file after. SD
 * cards have no portable SMART-style health telemetry the way SATA/NVMe
 * drives do (some support a proprietary, undocumented vendor health
 * command; nothing standard or safe to rely on here), so this write/read/
 * verify/speed round-trip is the practical substitute: a card that's
 * failing or too slow tends to show up here as either a verify mismatch or
 * a throughput number well below what a healthy card manages (order of a
 * few MB/s even over this board's SPI-mode 1-bit bus). *write_kbps/
 * *read_kbps are 0 on failure (mismatch or I/O error partway through) --
 * still report whichever timing was actually measured before that point,
 * rather than leaving them uninitialized. ESP_ERR_INVALID_STATE if not
 * mounted, otherwise ESP_OK (round-tripped correctly) or ESP_FAIL
 * (mismatch/IO error -- see the log for which). */
esp_err_t sdcard_selftest(float *write_kbps, float *read_kbps);

/* Formats the mounted card as FAT -- destroys everything on it. Real,
 * irreversible data loss; the web UI gates this behind its own confirm()
 * dialog (see webui.c's /sd_format handler), this function itself has no
 * additional confirmation of its own. Remounts at the same path
 * afterward on success, matching esp_vfs_fat_sdcard_format()'s own
 * behavior. ESP_ERR_INVALID_STATE if not mounted. */
esp_err_t sdcard_format(void);

#ifdef __cplusplus
}
#endif

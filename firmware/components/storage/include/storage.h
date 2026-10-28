/*
 * storage.h — capacity-capped file storage for recordings (and, later,
 * prompt/clip assets).
 *
 * Two backends, callers never touch the filesystem directly:
 *   - internal: SPIFFS on the "storage" flash partition (a few MB), /assets.
 *     Recordings are capped at CONFIG_STORAGE_RECORDING_CAP_BYTES rather than
 *     the full partition, oldest file evicted first once that's hit -- a
 *     ring buffer, not a permanent archive. This is the only backend on a
 *     board with no microSD.
 *   - sd: microSD (sdcard.h), when the board is configured for it AND a card
 *     is actually mounted. No eviction -- a card is big enough that treating
 *     it as a ring buffer would surprise someone expecting their recordings
 *     to just... stay. storage_write_begin() fails once it's genuinely full
 *     rather than silently deleting anything.
 *
 * The plain API (storage_write(), storage_read(), storage_list(),
 * storage_used_bytes(), storage_cap_bytes(), ...) always targets whichever backend is PRIMARY:
 * the SD card when one's mounted, internal flash otherwise. Existing callers
 * (recorder.c, dialplan clip playback, the web UI's general summary) don't
 * need to know or care which -- inserting a card just gives them more room,
 * with no code changes. storage_using_sd() reports which one that currently is.
 *
 * Reads (storage_read/storage_read_open) and storage_delete()/storage_patch()
 * also fall back to the OTHER backend if a name isn't on the primary one --
 * so recordings made before a card was inserted stay reachable afterward,
 * rather than silently disappearing from view. Order: primary, then the
 * other backend, then (reads only) the read-only bundled partition.
 *
 * For UI surfaces that want to show both backends explicitly rather than
 * just "whichever is primary" (Settings -> Storage files), use the
 * _internal() / _sd() variants below instead.
 *
 * Clips/prompts (operator.wav, dial-code clips, record_prompt.wav) are the
 * one exception to "primary": they always use storage_write_begin_local(),
 * which forces internal flash regardless of SD presence, so pulling the
 * card mid-use can never take dial tone, the operator message, or 211's
 * prompt/beep with it. Only real recordings (recorder.c) follow the card.
 */
#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define STORAGE_MAX_NAME 32

typedef struct {
    char   name[STORAGE_MAX_NAME];
    size_t size;
} storage_entry_t;

esp_err_t storage_init(void);

/* Changes every time a file is written, patched or deleted. */
uint32_t storage_generation(void);

/* Evicts oldest files (by mtime) first if needed to fit within the cap.
 * Fails with ESP_ERR_INVALID_SIZE if len alone exceeds the cap. */
esp_err_t storage_write(const char *name, const void *data, size_t len);

/*
 * Streaming write -- for data too big to buffer in RAM as one chunk (e.g. an
 * HTTP upload body pulled off the socket a few KB at a time). Same eviction/
 * capacity behavior as storage_write(), just spread across calls:
 *
 *   storage_writer_t w;
 *   if (storage_write_begin("clip.wav", total_len, &w) != ESP_OK) ...
 *   while (more data) {
 *       if (storage_write_chunk(&w, buf, n) != ESP_OK) { storage_write_abort(&w); ... }
 *   }
 *   storage_write_end(&w);
 *
 * `expected_len` must be the exact total you intend to write across all
 * storage_write_chunk() calls -- it's used up front for the cap/eviction
 * check, same as storage_write()'s `len`.
 */
typedef struct {
    void *file;  /* FILE*, opaque here to keep stdio out of the public header */
    char  path[288];
    bool  sd_held; /* holds the SD card mounted until end/abort */
} storage_writer_t;

esp_err_t storage_write_begin(const char *name, size_t expected_len, storage_writer_t *w);

/* Same, but always internal flash -- never the SD card, even when one's
 * mounted and would otherwise be primary. For clips/prompts (operator.wav,
 * dial-code clips, record_prompt.wav: anything the phone needs to keep
 * working on its own, not something the user is archiving) so pulling the
 * microSD card mid-use never takes dial tone, the operator message, or
 * 211's prompt/beep with it. Real recordings (recorder.c) should keep using
 * storage_write_begin() -- those are the thing the card is *for*. */
esp_err_t storage_write_begin_local(const char *name, size_t expected_len, storage_writer_t *w);

esp_err_t storage_write_chunk(storage_writer_t *w, const void *data, size_t len);
esp_err_t storage_write_end(storage_writer_t *w);
/* Closes and deletes the partial file. Safe to call after any failed
 * storage_write_chunk()/storage_write_begin(). */
void      storage_write_abort(storage_writer_t *w);

esp_err_t storage_read(const char *name, void *buf, size_t buf_len, size_t *out_len);

/*
 * Streaming read counterpart to storage_write_begin()/chunk()/end() -- for
 * playing back a file (see the `player` component) without buffering the
 * whole thing in RAM, which doesn't fit for anything past a few hundred KB
 * alongside WiFi/httpd. Usage:
 *
 *   storage_reader_t r;
 *   if (storage_read_open("clip.wav", &r) != ESP_OK) ...
 *   storage_read_seek(&r, data_offset);   // optional, e.g. to skip a WAV header
 *   size_t n;
 *   while ((n = storage_read_chunk(&r, buf, sizeof(buf))) > 0) { ... }
 *   storage_read_close(&r);
 */
typedef struct {
    void *file; /* FILE*, opaque here to keep stdio out of the public header */
    bool  sd_held; /* holds the SD card mounted until storage_read_close() */
} storage_reader_t;

/* ESP_ERR_NOT_FOUND if `name` doesn't exist. */
esp_err_t storage_read_open(const char *name, storage_reader_t *r);
esp_err_t storage_read_seek(storage_reader_t *r, size_t offset);
/* Total size of the file storage_read_open() actually opened -- see
 * storage.c's own doc comment for why this matters (media_handler()'s
 * Range support needs the real, exact size of the opened file, not a
 * separate lookup that can disagree about which backend/file it is).
 * Restores the read position; 0 if not open or on error. */
size_t    storage_read_size(storage_reader_t *r);
/* Returns bytes actually read (0 = EOF). */
size_t    storage_read_chunk(storage_reader_t *r, void *buf, size_t len);
void      storage_read_close(storage_reader_t *r);

esp_err_t storage_delete(const char *name);

/* Overwrite `len` bytes at `offset` in an existing file, in place. Used to
 * patch a streamed WAV's header size fields (and playback sample-rate for
 * pitch effects) once the final length is known. ESP_ERR_NOT_FOUND if the
 * file doesn't exist. */
esp_err_t storage_patch(const char *name, size_t offset, const void *data, size_t len);

/* Fills `out` (oldest first), returns the number of entries written
 * (<= max_entries). Pass out=NULL/max_entries=0 just to get a count via the
 * return value combined with storage_used_bytes(). */
size_t storage_list(storage_entry_t *out, size_t max_entries);

/* Same, but for the read-only "bundled" partition (firmware/assets/, baked
 * in at build time -- see storage.c). Informational only: there's no
 * storage_delete()/write for this partition, and none needed since
 * uploading/recording a same-named file to the writable partition already
 * takes priority over a bundled one at playback (storage_read_open()). */
size_t storage_list_bundled(storage_entry_t *out, size_t max_entries);

/* True if `name` is in writable storage (uploaded, recorded or generated
 * here), i.e. it isn't just the bundled default. */
bool storage_has_own(const char *name);

/* PRIMARY backend: SD if mounted, else internal. See the file header. */
size_t storage_used_bytes(void);
size_t storage_cap_bytes(void);
bool   storage_using_sd(void); /* == sdcard_mounted(); false if this board has no SD at all */

/* ---- explicit-backend access, for showing both at once (Settings -> Storage files) ---- */
bool     storage_sd_present(void); /* same as storage_using_sd() -- named for clarity at call sites that show both */

size_t   storage_list_internal(storage_entry_t *out, size_t max_entries);
size_t   storage_used_bytes_internal(void);
size_t   storage_cap_bytes_internal(void); /* always CONFIG_STORAGE_RECORDING_CAP_BYTES */

/* 0 for all three if no card is mounted. uint64_t, not size_t -- a card can
 * exceed 4 GB, which would wrap this chip's 32-bit size_t silently. */
size_t   storage_list_sd(storage_entry_t *out, size_t max_entries);
uint64_t storage_used_bytes_sd(void);
uint64_t storage_cap_bytes_sd(void);

#ifdef __cplusplus
}
#endif

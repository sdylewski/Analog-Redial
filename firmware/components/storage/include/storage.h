/*
 * storage.h — capacity-capped file storage for recordings (and, later,
 * prompt/clip assets).
 *
 * Backing store today: SPIFFS on the internal "storage" flash partition
 * (2MB, see partitions.csv), mounted at /assets. Recordings are capped at
 * CONFIG_STORAGE_RECORDING_CAP_BYTES rather than the full partition, oldest
 * file evicted first once that's hit -- a ring buffer, not a permanent
 * archive.
 *
 * When a microSD backend exists (CONFIG_BOARD_MICROSD build), this is the
 * seam to swap it in behind the same API -- callers (console commands, the
 * web UI) never touch the filesystem directly, only these functions.
 */
#pragma once

#include "esp_err.h"
#include <stddef.h>
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
    char  path[64];
} storage_writer_t;

esp_err_t storage_write_begin(const char *name, size_t expected_len, storage_writer_t *w);
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
} storage_reader_t;

/* ESP_ERR_NOT_FOUND if `name` doesn't exist. */
esp_err_t storage_read_open(const char *name, storage_reader_t *r);
esp_err_t storage_read_seek(storage_reader_t *r, size_t offset);
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

size_t storage_used_bytes(void);
size_t storage_cap_bytes(void);
bool   storage_using_sd(void); /* always false until an SD backend exists */

#ifdef __cplusplus
}
#endif

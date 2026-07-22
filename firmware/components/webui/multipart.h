/*
 * multipart.h — minimal streaming multipart/form-data reader for
 * esp_http_server, used by the browser-based audio upload forms (a plain
 * <input type=file> can't POST any other way). Nothing in this codebase
 * needed this before -- every other upload is either a raw-body POST
 * (curl/gen_prompt.py) or a small url-encoded form.
 *
 * Usage:
 *   multipart_reader_t r;
 *   multipart_reader_init(&r, req);
 *   char name[32], filename[64];
 *   while (multipart_next_part(&r, name, sizeof(name), filename, sizeof(filename)) == ESP_OK) {
 *       size_t n;
 *       while ((n = multipart_read_body(&r, buf, sizeof(buf))) > 0) { ... }
 *   }
 *   multipart_reader_close(&r);
 *
 * Put <input type=file> LAST in the HTML form -- browsers submit fields in
 * DOM order, so small text fields (code, desc, ...) arrive and are fully
 * parsed before the file part starts streaming, letting the file part be
 * piped straight through (e.g. into audio_convert + storage_write_chunk)
 * without ever buffering the whole upload.
 */
#pragma once

#include "esp_err.h"
#include "esp_http_server.h"
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MULTIPART_BOUNDARY_MAX 96
#define MULTIPART_BUF_SIZE     4096

typedef struct {
    httpd_req_t *req;
    char   delim[MULTIPART_BOUNDARY_MAX]; /* "--" + boundary */
    size_t delim_len;
    long   remaining;              /* bytes of request body not yet read via httpd_req_recv */
    uint8_t *buf;                  /* heap-allocated, MULTIPART_BUF_SIZE bytes -- this struct
                                     * is meant to be a stack local in callers, and 4KB inline
                                     * would blow the httpd task's stack (hit exactly that,
                                     * "Stack protection fault", before this was a pointer) */
    size_t buf_len;                /* valid bytes in buf */
    size_t buf_pos;                /* next unconsumed byte */
    bool   done;                   /* final boundary seen */
} multipart_reader_t;

/* Reads Content-Type off `req` to find the boundary. ESP_ERR_INVALID_ARG if
 * it isn't multipart/form-data or has no boundary; ESP_ERR_NO_MEM if the
 * internal buffer couldn't be allocated. Call multipart_reader_close() when
 * done (or on any early-return error path) to free it. */
esp_err_t multipart_reader_init(multipart_reader_t *r, httpd_req_t *req);

/* Advances to the next part, filling `name` (form field name) and
 * `filename` (empty string if this part has none, i.e. a plain text field
 * rather than a file). ESP_ERR_NOT_FOUND once the terminal boundary is
 * reached (no more parts) -- that's the normal end-of-loop condition, not
 * an error. */
esp_err_t multipart_next_part(multipart_reader_t *r, char *name, size_t name_len,
                               char *filename, size_t filename_len);

/* Reads up to `cap` bytes of the CURRENT part's body into `out`. Returns 0
 * when the part is exhausted (next boundary reached) -- call
 * multipart_next_part() again at that point. */
size_t multipart_read_body(multipart_reader_t *r, void *out, size_t cap);

void multipart_reader_close(multipart_reader_t *r);

#ifdef __cplusplus
}
#endif

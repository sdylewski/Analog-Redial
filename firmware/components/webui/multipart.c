#include "multipart.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>

/* Compacts unread bytes to the front of the buffer, then tries to fill it
 * (up to capacity or the rest of the request body) via httpd_req_recv().
 * Returns false only on an actual recv() failure -- NOT reaching `want`
 * bytes because the body legitimately ended is not a failure, callers check
 * how much ended up available afterward. */
static bool ensure(multipart_reader_t *r, size_t want)
{
    if (r->buf_pos > 0) {
        size_t unread = r->buf_len - r->buf_pos;
        if (unread > 0) memmove(r->buf, r->buf + r->buf_pos, unread);
        r->buf_len = unread;
        r->buf_pos = 0;
    }
    while (r->buf_len < want && r->remaining > 0 && r->buf_len < MULTIPART_BUF_SIZE) {
        size_t space = MULTIPART_BUF_SIZE - r->buf_len;
        size_t ask = ((size_t)r->remaining < space) ? (size_t)r->remaining : space;
        int n = httpd_req_recv(r->req, (char *)r->buf + r->buf_len, ask);
        if (n <= 0) return false;
        r->buf_len += (size_t)n;
        r->remaining -= n;
    }
    return true;
}

esp_err_t multipart_reader_init(multipart_reader_t *r, httpd_req_t *req)
{
    memset(r, 0, sizeof(*r));
    r->req = req;
    r->remaining = (long)req->content_len;

    char ctype[160];
    if (httpd_req_get_hdr_value_str(req, "Content-Type", ctype, sizeof(ctype)) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    const char *b = strstr(ctype, "boundary=");
    if (!b) return ESP_ERR_INVALID_ARG;
    b += strlen("boundary=");

    char boundary[MULTIPART_BOUNDARY_MAX - 2];
    size_t i = 0;
    if (*b == '"') {
        b++;
        while (*b && *b != '"' && i < sizeof(boundary) - 1) boundary[i++] = *b++;
    } else {
        while (*b && *b != ';' && *b != '\r' && *b != '\n' && i < sizeof(boundary) - 1) {
            boundary[i++] = *b++;
        }
    }
    boundary[i] = '\0';
    if (i == 0) return ESP_ERR_INVALID_ARG;

    r->buf = malloc(MULTIPART_BUF_SIZE);
    if (!r->buf) return ESP_ERR_NO_MEM;

    snprintf(r->delim, sizeof(r->delim), "--%s", boundary);
    r->delim_len = strlen(r->delim);
    return ESP_OK;
}

/* Bare "--boundary" at the front of the (already CRLF-skipped) buffer. Used
 * by multipart_next_part() right after it has skipped past any leading
 * CRLF, so this never needs to search -- it only ever checks offset 0. */
static bool at_delim(multipart_reader_t *r)
{
    return r->buf_len - r->buf_pos >= r->delim_len &&
           memcmp(r->buf + r->buf_pos, r->delim, r->delim_len) == 0;
}

/* Searches the buffered window for "\r\n--boundary" -- the real delimiter
 * per RFC 2046 (the CRLF belongs to the delimiter, not the preceding part's
 * body). Sets *out_off to its offset (relative to buf_pos) if found. */
static bool find_body_end(multipart_reader_t *r, size_t *out_off)
{
    size_t avail = r->buf_len - r->buf_pos;
    size_t pattern_len = r->delim_len + 2;
    if (avail < pattern_len) return false;
    const uint8_t *hay = r->buf + r->buf_pos;
    for (size_t i = 0; i + pattern_len <= avail; i++) {
        if (hay[i] == '\r' && hay[i + 1] == '\n' &&
            memcmp(hay + i + 2, r->delim, r->delim_len) == 0) {
            *out_off = i;
            return true;
        }
    }
    return false;
}

esp_err_t multipart_next_part(multipart_reader_t *r, char *name, size_t name_len,
                               char *filename, size_t filename_len)
{
    if (name_len) name[0] = '\0';
    if (filename_len) filename[0] = '\0';
    if (r->done) return ESP_ERR_NOT_FOUND;

    /* read_body() (or the very first call here) leaves buf_pos sitting
     * right at "\r\n--delim" -- except the very first delimiter in the
     * whole body, which per RFC 2046 has no leading CRLF. Skip it if
     * present, then the buffer must have the bare delimiter at offset 0. */
    if (!ensure(r, r->delim_len + 2)) return ESP_ERR_INVALID_STATE;
    if (r->buf_len - r->buf_pos >= 2 &&
        r->buf[r->buf_pos] == '\r' && r->buf[r->buf_pos + 1] == '\n') {
        r->buf_pos += 2;
    }
    if (!ensure(r, r->delim_len) || !at_delim(r)) return ESP_ERR_INVALID_STATE;
    r->buf_pos += r->delim_len;

    /* Terminal boundary is "--delim--"; a mid-stream one is "--delim\r\n". */
    if (!ensure(r, 2) || r->buf_len - r->buf_pos < 2) return ESP_ERR_INVALID_STATE;
    if (r->buf[r->buf_pos] == '-' && r->buf[r->buf_pos + 1] == '-') {
        r->done = true;
        return ESP_ERR_NOT_FOUND;
    }
    if (r->buf[r->buf_pos] == '\r' && r->buf[r->buf_pos + 1] == '\n') {
        r->buf_pos += 2;
    }

    /* Part headers: CRLF-terminated lines until a blank line. Form-data
     * headers are always tiny, so one buffered window (4KB) is always
     * enough -- no need to grow/retry. */
    for (;;) {
        if (!ensure(r, 2)) return ESP_ERR_INVALID_STATE;
        size_t avail = r->buf_len - r->buf_pos;
        if (avail >= 2 && r->buf[r->buf_pos] == '\r' && r->buf[r->buf_pos + 1] == '\n') {
            r->buf_pos += 2; /* blank line: end of headers */
            break;
        }

        size_t line_len = 0;
        bool found_eol = false;
        for (size_t i = 0; i + 1 < avail; i++) {
            if (r->buf[r->buf_pos + i] == '\r' && r->buf[r->buf_pos + i + 1] == '\n') {
                line_len = i;
                found_eol = true;
                break;
            }
        }
        if (!found_eol) return ESP_ERR_INVALID_STATE; /* header line too long or malformed */

        char line[256];
        size_t n = (line_len < sizeof(line) - 1) ? line_len : sizeof(line) - 1;
        memcpy(line, r->buf + r->buf_pos, n);
        line[n] = '\0';
        r->buf_pos += line_len + 2;

        if (strncasecmp(line, "Content-Disposition:", sizeof("Content-Disposition:") - 1) == 0) {
            const char *p = strstr(line, "name=\"");
            if (p) {
                p += 6;
                const char *e = strchr(p, '"');
                if (e && name_len) {
                    size_t cn = (size_t)(e - p);
                    if (cn >= name_len) cn = name_len - 1;
                    memcpy(name, p, cn);
                    name[cn] = '\0';
                }
            }
            p = strstr(line, "filename=\"");
            if (p) {
                p += 10;
                const char *e = strchr(p, '"');
                if (e && filename_len) {
                    size_t cn = (size_t)(e - p);
                    if (cn >= filename_len) cn = filename_len - 1;
                    memcpy(filename, p, cn);
                    filename[cn] = '\0';
                }
            }
        }
        /* Other headers (e.g. this part's own Content-Type) are skipped. */
    }

    return ESP_OK;
}

size_t multipart_read_body(multipart_reader_t *r, void *out, size_t cap)
{
    if (cap == 0) return 0;
    if (!ensure(r, r->delim_len + 2)) return 0;

    size_t avail = r->buf_len - r->buf_pos;
    size_t off;
    bool found = find_body_end(r, &off);
    if (found && off == 0) return 0; /* sitting right at the delimiter: part exhausted */

    size_t safe;
    if (found) {
        safe = off;
    } else {
        /* No delimiter in the buffered window -- hold back enough trailing
         * bytes that one straddling this fill and the next can't be missed,
         * unless the body has truly ended (nothing left to complete a
         * partial match with, so there's nothing to gain by holding back). */
        size_t hold = r->delim_len + 1;
        if (r->remaining <= 0 && avail <= hold) {
            safe = avail;
        } else {
            safe = (avail > hold) ? avail - hold : 0;
        }
    }

    size_t n = (safe < cap) ? safe : cap;
    if (n > 0) {
        memcpy(out, r->buf + r->buf_pos, n);
        r->buf_pos += n;
    }
    return n;
}

void multipart_reader_close(multipart_reader_t *r)
{
    free(r->buf);
    r->buf = NULL;
}

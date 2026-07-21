/*
 * logbuf.h — in-RAM ring buffer tap on esp_log, so recent log output is
 * viewable over HTTP, not just the serial console.
 */
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Hooks esp_log_set_vprintf(); console/UART output is unaffected (still
 * chains to whatever vprintf was previously installed), this only taps a
 * copy into the ring buffer. Call once, early in app_main(). */
void logbuf_init(void);

/* Copies the buffer's current contents into `out` (NUL-terminated,
 * truncated to the most recent bytes if `out_len` is smaller than what's
 * buffered). Returns the number of bytes written, excluding the NUL. */
size_t logbuf_snapshot(char *out, size_t out_len);

#ifdef __cplusplus
}
#endif

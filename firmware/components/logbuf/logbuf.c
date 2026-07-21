#include "logbuf.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include <stdarg.h>
#include <stdio.h>

#define LOGBUF_SIZE 8192

static char             s_buf[LOGBUF_SIZE];
static size_t           s_head;   /* next write position, wraps */
static size_t           s_filled; /* valid bytes in s_buf, caps at LOGBUF_SIZE */
static portMUX_TYPE      s_mux = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t   s_orig;

static int logbuf_vprintf(const char *fmt, va_list args)
{
    char line[256];
    va_list copy;
    va_copy(copy, args);
    int n = vsnprintf(line, sizeof(line), fmt, copy);
    va_end(copy);

    if (n > 0) {
        size_t len = ((size_t)n < sizeof(line)) ? (size_t)n : sizeof(line) - 1;
        portENTER_CRITICAL(&s_mux);
        for (size_t i = 0; i < len; i++) {
            s_buf[s_head] = line[i];
            s_head = (s_head + 1) % LOGBUF_SIZE;
            if (s_filled < LOGBUF_SIZE) s_filled++;
        }
        portEXIT_CRITICAL(&s_mux);
    }

    return s_orig ? s_orig(fmt, args) : vprintf(fmt, args);
}

void logbuf_init(void)
{
    s_orig = esp_log_set_vprintf(logbuf_vprintf);
}

size_t logbuf_snapshot(char *out, size_t out_len)
{
    if (out_len == 0) return 0;

    portENTER_CRITICAL(&s_mux);
    size_t filled = s_filled;
    size_t start = (filled < LOGBUF_SIZE) ? 0 : s_head; /* oldest byte position */
    size_t n = filled;
    if (n > out_len - 1) {
        /* buffer bigger than caller's out -- keep the most recent bytes */
        size_t skip = n - (out_len - 1);
        start = (start + skip) % LOGBUF_SIZE;
        n = out_len - 1;
    }
    for (size_t i = 0; i < n; i++) {
        out[i] = s_buf[(start + i) % LOGBUF_SIZE];
    }
    portEXIT_CRITICAL(&s_mux);

    out[n] = '\0';
    return n;
}

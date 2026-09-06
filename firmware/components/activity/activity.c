#include "activity.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define ACTIVITY_CAP 20
#define ACTIVITY_MSG_LEN 80

typedef struct {
    int64_t boot_ms;
    char    msg[ACTIVITY_MSG_LEN];
} activity_entry_t;

static activity_entry_t s_entries[ACTIVITY_CAP];
static size_t           s_head;   /* index the NEXT entry will be written to */
static size_t           s_count;  /* number of valid entries, caps at ACTIVITY_CAP */
static portMUX_TYPE      s_mux = portMUX_INITIALIZER_UNLOCKED;

void activity_init(void)
{
    portENTER_CRITICAL(&s_mux);
    s_head = 0;
    s_count = 0;
    portEXIT_CRITICAL(&s_mux);
}

void activity_log(const char *fmt, ...)
{
    char msg[ACTIVITY_MSG_LEN];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    int64_t now_ms = esp_timer_get_time() / 1000;

    portENTER_CRITICAL(&s_mux);
    activity_entry_t *e = &s_entries[s_head];
    e->boot_ms = now_ms;
    memcpy(e->msg, msg, sizeof(e->msg));
    s_head = (s_head + 1) % ACTIVITY_CAP;
    if (s_count < ACTIVITY_CAP) s_count++;
    portEXIT_CRITICAL(&s_mux);
}

static void format_relative(int64_t age_ms, char *out, size_t out_len)
{
    int64_t age_s = age_ms / 1000;
    if (age_s < 60) {
        snprintf(out, out_len, "%llds ago", (long long)age_s);
    } else if (age_s < 3600) {
        snprintf(out, out_len, "%lldm ago", (long long)(age_s / 60));
    } else {
        snprintf(out, out_len, "%lldh ago", (long long)(age_s / 3600));
    }
}

size_t activity_get(char out_lines[][96], size_t max)
{
    activity_entry_t snap[ACTIVITY_CAP];
    size_t count, head;

    portENTER_CRITICAL(&s_mux);
    count = s_count;
    head = s_head;
    memcpy(snap, s_entries, sizeof(snap));
    portEXIT_CRITICAL(&s_mux);

    int64_t now_ms = esp_timer_get_time() / 1000;

    size_t n = count < max ? count : max;
    for (size_t i = 0; i < n; i++) {
        /* newest entry is at (head - 1), walking backwards through the ring */
        size_t idx = (head + ACTIVITY_CAP - 1 - i) % ACTIVITY_CAP;
        activity_entry_t *e = &snap[idx];
        /* Sized so rel's worst case (14 chars) + ": " (2) + msg's worst case
         * (ACTIVITY_MSG_LEN-1 = 79) exactly fills out_lines[i]'s 96 bytes
         * including the NUL -- keeps -Wformat-truncation happy without
         * silently dropping real content in practice (a relative-time tag
         * is never anywhere close to 14 chars). */
        char rel[15];
        int64_t age_ms = now_ms - e->boot_ms;
        if (age_ms < 0) age_ms = 0;
        format_relative(age_ms, rel, sizeof(rel));
        snprintf(out_lines[i], 96, "%s: %s", rel, e->msg);
    }
    return n;
}

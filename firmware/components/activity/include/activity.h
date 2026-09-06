/*
 * activity.h — in-RAM ring buffer of structured "what just happened" events
 * (calls, recordings, dial errors...) for the web UI's Status page. Distinct
 * from logbuf.h: logbuf taps raw esp_log text at whatever log level is set,
 * activity stores short human-readable entries independent of log level.
 */
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Call once, early in app_main() (alongside logbuf_init()). */
void activity_init(void);

/* printf-style; formatted message is capped to ~80 chars (truncated with the
 * NUL still in place if longer). Safe to call from any task. */
void activity_log(const char *fmt, ...);

/* Fills out_lines[0..n) with the most recent entries, newest first, each
 * pre-formatted as "<relative time> ago: <message>" (e.g. "2m ago: Line 1
 * called line 2"), with the relative time computed fresh against the current
 * clock at call time. Returns the number of lines written (<= max). */
size_t activity_get(char out_lines[][96], size_t max);

#ifdef __cplusplus
}
#endif

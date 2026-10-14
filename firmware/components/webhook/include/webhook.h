/*
 * webhook.h — POST a small JSON event to one URL (e.g. a Home Assistant
 * webhook) when something happens on the phone lines: off-hook, on-hook,
 * a dial code, an incoming call, a saved recording.
 *
 * Body: {"board":"phone","event":"offhook","line":0,"detail":"...","time":1760000000}
 * (`line` is 0-based like the rest of the HTTP API; `time` is Unix seconds,
 * 0 if the clock isn't set yet.)
 *
 * Plain http:// only: HTTPS would need a TLS session (~30 KB of heap) per
 * post. Nothing is allocated until a URL is set: the sender task and its
 * queue are created on first use and stay for the rest of the boot.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WEBHOOK_URL_MAX 128

/* Loads the saved URL; starts the sender if one is set. */
void webhook_init(void);

/* Queues an event; never blocks. Dropped silently if no URL is set or the
 * queue (8 events) is full. detail may be NULL. Safe from any task. */
void webhook_event(const char *event, int line, const char *detail);

/* printf-style detail. */
void webhook_eventf(const char *event, int line, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/* Second consumer of the same events (the MQTT client): called, on the task
 * that raised the event, for every event whether or not a URL is set. Must not
 * block. NULL removes it. */
typedef void (*webhook_tap_t)(const char *event, int line, const char *detail);
void webhook_set_tap(webhook_tap_t tap);

void webhook_url_get(char *out, size_t len);
/* "" turns webhooks off. ESP_ERR_INVALID_ARG unless it starts with http://. */
esp_err_t webhook_url_set(const char *url);

/* Result of the last post: HTTP status (200..299 is success), a negative
 * esp_err_t if it never got an answer, 0 if nothing was sent this boot. */
int webhook_last_status(void);
unsigned webhook_sent_count(void);
unsigned webhook_dropped_count(void);

#ifdef __cplusplus
}
#endif

/*
 * webui.h — status/log/recordings over esp_http_server.
 *
 * Routes:
 *   GET /              status page (WiFi, uptime, heap, per-line telephony
 *                       state, storage usage)
 *   GET /api/status     same, as JSON
 *   GET /api/log         recent log output (logbuf), text/plain
 *   GET /api/recordings  recordings list (storage), JSON
 *   GET /media/<name>    download a recording
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t webui_start(void);

#ifdef __cplusplus
}
#endif

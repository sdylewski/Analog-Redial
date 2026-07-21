/*
 * wifi.h — station-mode WiFi + SNTP bring-up.
 *
 * Credentials: NVS (set via wifi_set_credentials()/the 'wifi_set' console
 * command) always takes priority; falls back to CONFIG_WIFI_SSID/
 * CONFIG_WIFI_PASSWORD if NVS has nothing saved yet. Either source works
 * standalone -- no need to touch both.
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Starts WiFi station mode and SNTP. Non-blocking -- returns once the
 * connection attempt is kicked off, not once it succeeds; check
 * wifi_is_connected() or watch the log. Safe to call with no credentials
 * configured (logs a warning and waits for wifi_set_credentials()). */
esp_err_t wifi_init(void);

/* Save new credentials to NVS and reconnect using them immediately. */
esp_err_t wifi_set_credentials(const char *ssid, const char *password);

bool        wifi_is_connected(void);
int8_t      wifi_rssi(void);                       /* dBm; 0 if not connected */
void        wifi_get_ip_str(char *buf, size_t buf_len); /* "0.0.0.0" if not connected */
const char *wifi_get_ssid(void);                   /* configured SSID, "" if none */

#ifdef __cplusplus
}
#endif

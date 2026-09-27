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

/* Erase saved WiFi credentials from NVS (the GPIO9 factory reset). Does NOT
 * disconnect or reboot -- the caller decides (app_main reboots so the device
 * comes back up with no creds -> provisioning). */
esp_err_t wifi_clear_credentials(void);

/* Write credentials to NVS without connecting (used by the provisioning AP,
 * which then reboots into station mode). wifi_set_credentials() is the live
 * "save and reconnect now" variant for when we're already on a network. */
esp_err_t wifi_save_credentials(const char *ssid, const char *password);

/* True while the setup SoftAP is up (no credentials configured). The web UI
 * serves its captive setup page instead of the normal dashboard in this
 * state. */
bool wifi_is_provisioning(void);

/* Re-read the board name (board_hostname()/board_name_get()) and push it to
 * mDNS as the <name>.local hostname + instance name. Call after
 * board_name_set() so the .local address updates without a reboot. No-op-ish
 * if mDNS didn't start. */
void wifi_apply_hostname(void);

bool        wifi_is_connected(void);
int8_t      wifi_rssi(void);                       /* dBm; 0 if not connected */
void        wifi_get_ip_str(char *buf, size_t buf_len); /* "0.0.0.0" if not connected */
const char *wifi_get_ssid(void);                   /* configured SSID, "" if none */

typedef struct {
    char    ssid[33];
    int8_t  rssi;
    bool    secured;
} wifi_scan_result_t;

/* Blocking active scan for nearby SSIDs (roughly 0.5-3 s). Works both while
 * provisioning (the setup SoftAP runs AP+STA so the STA side can scan) and
 * once connected as a normal station. Results are deduped by SSID (multiple
 * APs/BSSIDs for the same network keep only the strongest RSSI) and sorted
 * strongest-first. Returns the number of entries written to out (<= max), or
 * -1 if the scan itself failed. */
int wifi_scan(wifi_scan_result_t *out, int max);

/* Runtime WiFi power-save toggle for idle_power.c -- see that component and
 * archive/POWER_SAVING.md. true = WIFI_PS_MIN_MODEM (radio sleeps between
 * DTIM beacons), false = WIFI_PS_NONE (this device's normal default; see the
 * comment in wifi_init() for why). Not for general use -- only safe to
 * enable when nothing needs the radio to stay instantly reachable. */
void wifi_set_power_save(bool eco);

/* Maximum WiFi transmit power, in whole dBm (clamped to 2..20 -- the PHY's
 * configured ceiling is CONFIG_ESP_PHY_MAX_WIFI_TX_POWER=20). Only meaningful
 * once WiFi has started; returns the error from esp_wifi_set_max_tx_power()
 * otherwise. */
esp_err_t wifi_set_tx_power_dbm(int dbm);
/* Current maximum TX power in whole dBm, or 0 if WiFi isn't running. */
int       wifi_get_tx_power_dbm(void);

#ifdef __cplusplus
}
#endif

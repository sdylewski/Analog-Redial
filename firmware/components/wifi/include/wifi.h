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

/* Erase every saved WiFi network from NVS (the GPIO9 factory reset). Does NOT
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

/* ---- known networks ----
 * Several saved SSID/password pairs; at connect time the board scans and
 * joins whichever is strongest, so carrying it between home and somewhere
 * else just works. Boards saved before this existed migrate their single
 * pair into slot 0 automatically. */
#define WIFI_MAX_NETWORKS 5

typedef struct {
    char ssid[33];
    bool has_password;
    int  slot;          /* storage index, for removal */
} wifi_known_net_t;

int       wifi_known_list(wifi_known_net_t *out, int max);   /* returns count */
esp_err_t wifi_known_add(const char *ssid, const char *pass); /* adds or updates */
esp_err_t wifi_known_remove(const char *ssid);

/* Name of the open setup network: "Analog Redial - <board name>" once the
 * board has a name (cut to Wi-Fi's 32-byte SSID limit), otherwise
 * "AnalogRedial-XXXX" from the MAC, for a brand-new board. Pass at least
 * 33 bytes. */
void wifi_setup_ap_name(char *out, size_t len);

bool wifi_is_provisioning(void);

/* Ask the next boot to come up in the setup SoftAP even though credentials
 * are saved -- the BOOT-button rescue gesture. One-shot: consumed at the
 * next wifi_init(), so an ordinary reboot after that rejoins the saved
 * network. Credentials are never touched. Caller reboots. */
esp_err_t wifi_request_setup_ap(void);

/* True when this boot's SoftAP came from wifi_request_setup_ap() rather than
 * from having no credentials. The web UI serves its full dashboard in that
 * case -- the point of the gesture is reaching the UI, not re-provisioning. */
bool wifi_ap_is_forced(void);

/* Re-read the board name (board_hostname()/board_name_get()) and push it to
 * mDNS as the <name>.local hostname + instance name. Call after
 * board_name_set() so the .local address updates without a reboot. No-op-ish
 * if mDNS didn't start. If the name is taken on the network it answers as
 * <name>1 etc. for this boot only -- nothing is saved. */
void wifi_apply_hostname(void);

/* The name/hostname the board is actually using right now: the saved board
 * name, or e.g. "phone1"/"phone1" while "phone" is taken on this network.
 * For display -- anything that must be stable (VoIP, prompts) should use
 * board_name_get() instead. */
void wifi_live_name(char *out, size_t len);
void wifi_live_hostname(char *out, size_t len);

bool        wifi_is_connected(void);
int8_t      wifi_rssi(void);                       /* dBm; 0 if not connected */
void        wifi_get_ip_str(char *buf, size_t buf_len); /* "0.0.0.0" if not connected */
const char *wifi_get_ssid(void);                   /* configured SSID, "" if none */

typedef struct {
    char    ssid[33];
    int8_t  rssi;
    bool    secured;
    uint8_t authmode;   /* wifi_auth_mode_t: open, WPA2, WPA3, ... */
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

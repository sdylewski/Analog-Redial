#include "wifi.h"
#include "board.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_mac.h"
#include "mdns.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* Not "wifi" -- that tag belongs to the (very chatty) internal WiFi driver
 * logs, which we deliberately quiet below. A distinct tag lets our own
 * connect/status messages stay visible without fighting over one level. */
static const char *TAG = "wifi_app";
#define NVS_NS "wifi"

static bool           s_connected;
static char            s_ssid[33];
static esp_ip4_addr_t  s_ip;
static bool           s_mdns_ready;  /* mdns_init() succeeded */
static bool           s_mdns_named;  /* hostname resolved+set since boot */
static bool           s_provisioning; /* running the setup SoftAP (no creds) */
static esp_netif_t    *s_sta_netif;   /* for esp_netif_set_hostname() -- DHCP hostname */

static esp_err_t load_creds_from_nvs(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    size_t n = ssid_len;
    err = nvs_get_str(h, "ssid", ssid, &n);
    if (err != ESP_OK) { nvs_close(h); return err; }
    n = pass_len;
    err = nvs_get_str(h, "pass", pass, &n);
    nvs_close(h);
    return err;
}

static esp_err_t do_connect(const char *ssid, const char *pass)
{
    wifi_config_t wc = { 0 };
    strncpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, pass, sizeof(wc.sta.password) - 1);
    wc.sta.threshold.authmode = strlen(pass) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    strncpy(s_ssid, ssid, sizeof(s_ssid) - 1);
    ESP_LOGI(TAG, "connecting to '%s'", ssid);
    board_led_set_state(BOARD_LED_WIFI_CONNECTING);

    esp_err_t err = esp_wifi_connect();
    if (err == ESP_ERR_WIFI_CONN) {
        /* A previous attempt (e.g. with different credentials) is still
         * mid-handshake -- not a real failure. The config above is already
         * saved and applied; WIFI_EVENT_STA_DISCONNECTED's handler retries
         * with it automatically once the in-flight attempt ends. */
        ESP_LOGI(TAG, "previous connection attempt still in progress; will "
                      "retry with these credentials once it ends");
        return ESP_OK;
    }
    return err;
}

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_ssid[0]) esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        /* The reason code is what tells a flaky link (beacon timeout, 200+,
         * e.g. radio interference) apart from the AP deliberately kicking us
         * (auth/assoc codes) -- without it, "disconnected, retrying" alone
         * can't distinguish them. See wifi_err_reason_t in esp_wifi_types.h. */
        if (s_connected) {
            const wifi_event_sta_disconnected_t *d = (const wifi_event_sta_disconnected_t *)data;
            ESP_LOGW(TAG, "WiFi disconnected (reason %d, RSSI %d dBm), retrying",
                     d ? d->reason : -1, d ? d->rssi : 0);
        }
        s_connected = false;
        if (s_ssid[0]) esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        s_ip = e->ip_info.ip;
        s_connected = true;
        board_led_set_state(BOARD_LED_OK);
        ESP_LOGI(TAG, "connected, IP=" IPSTR, IP2STR(&s_ip));
        /* Now that the LAN is reachable, claim our mDNS name (with a collision
         * check that needs connectivity). Once per boot; reconnects skip it. */
        if (!s_mdns_named) {
            wifi_apply_hostname();
            s_mdns_named = true;
        }
    }
}

/* ---- WiFi setup SoftAP (provisioning) ----
 * When there are no saved credentials, come up as our own open AP so the
 * phone can be configured from a browser. A tiny captive-portal DNS server
 * points every lookup at our AP IP so the "sign in to network" sheet pops. */

/* Minimal DNS responder: answers every query with an A record for
 * 192.168.4.1 (the SoftAP gateway). Not a real resolver -- just enough to
 * trigger captive-portal detection on phones/laptops. */
static void captive_dns_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) { vTaskDelete(NULL); return; }
    struct sockaddr_in bind_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        ESP_LOGW(TAG, "captive DNS: bind :53 failed");
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    uint8_t buf[512];
    while (1) {
        struct sockaddr_in src;
        socklen_t slen = sizeof(src);
        int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&src, &slen);
        if (n < 12) continue;                 /* smaller than a DNS header */
        if (n + 16 > (int)sizeof(buf)) continue;

        buf[2] = 0x81; buf[3] = 0x80;         /* QR=1, RD copied, RA=1, no error */
        buf[6] = 0x00; buf[7] = 0x01;         /* ANCOUNT = 1 */
        buf[8] = buf[9] = buf[10] = buf[11] = 0; /* NS/AR counts = 0 */

        uint8_t *p = buf + n;                 /* append answer after the question */
        *p++ = 0xC0; *p++ = 0x0C;             /* name -> pointer to the question */
        *p++ = 0x00; *p++ = 0x01;             /* type A */
        *p++ = 0x00; *p++ = 0x01;             /* class IN */
        *p++ = 0x00; *p++ = 0x00; *p++ = 0x00; *p++ = 0x3C; /* TTL 60 s */
        *p++ = 0x00; *p++ = 0x04;             /* RDLENGTH 4 */
        *p++ = 192; *p++ = 168; *p++ = 4; *p++ = 1; /* 192.168.4.1 */

        sendto(sock, buf, (int)(p - buf), 0, (struct sockaddr *)&src, slen);
    }
}

/* Reads and clears the one-shot "come up in the setup AP" flag. */
static bool take_forced_ap_flag(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    uint8_t v = 0;
    bool set = (nvs_get_u8(h, "force_ap", &v) == ESP_OK) && v;
    if (set) { nvs_erase_key(h, "force_ap"); nvs_commit(h); }
    nvs_close(h);
    return set;
}

static bool s_forced_ap;

static esp_err_t start_provisioning(void)
{
    s_provisioning = true;
    esp_netif_create_default_wifi_ap();

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);

    wifi_config_t ap = { 0 };
    int n = snprintf((char *)ap.ap.ssid, sizeof(ap.ap.ssid),
                     "AnalogRedial-%02X%02X", mac[4], mac[5]);
    ap.ap.ssid_len = (uint8_t)n;
    ap.ap.channel = 1;
    ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_OPEN; /* open network: easiest to join for setup */

    /* AP+STA, not AP-only: the STA side never connects to anything here, but
     * its presence is what lets wifi_scan() list nearby networks for the
     * setup page's dropdown while the SoftAP is up. */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());

    board_led_set_state(BOARD_LED_WIFI_CONNECTING); /* "needs setup" */
    xTaskCreate(captive_dns_task, "captdns", 3072, NULL, 4, NULL);

    ESP_LOGW(TAG, "no WiFi credentials -- started setup AP '%s' (open). Join it, then "
                  "a setup page opens (or browse to http://192.168.4.1)", (char *)ap.ap.ssid);
    return ESP_OK;
}

bool wifi_is_provisioning(void) { return s_provisioning; }
bool wifi_ap_is_forced(void) { return s_forced_ap; }

esp_err_t wifi_request_setup_ap(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, "force_ap", 1);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t wifi_save_credentials(const char *ssid, const char *password)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, "ssid", ssid);
    if (err == ESP_OK) err = nvs_set_str(h, "pass", password);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t wifi_init(void)
{
    /* The internal WiFi/PHY driver logs (tags below) are extremely chatty at
     * their default INFO level -- every auth/assoc state transition, AMPDU
     * block-ack setup, RF calibration detail, etc. Quiet them to WARN so our
     * own connect/status messages (tag "wifi_app") aren't buried. */
    esp_log_level_set("wifi", ESP_LOG_WARN);
    esp_log_level_set("wifi_init", ESP_LOG_WARN);
    esp_log_level_set("phy_init", ESP_LOG_WARN);
    esp_log_level_set("net80211", ESP_LOG_WARN);
    esp_log_level_set("pp", ESP_LOG_WARN);
    esp_log_level_set("esp_netif_handlers", ESP_LOG_WARN);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta_netif = esp_netif_create_default_wifi_sta();

    /* DHCP hostname: without this, the router's client list shows every
     * board as CONFIG_LWIP_LOCAL_HOSTNAME's generic default ("espressif"),
     * useless for picking one out among several -- and, unlike the mDNS
     * name, the router's page has no idea what ".local" even means, so it's
     * the one identifier that's always visible there regardless of whether
     * mDNS itself is working. Best-effort: uses whatever board name is
     * configured right now (falls back to the same default as the .local
     * name, "phone", until the user sets one). Re-applied from
     * wifi_apply_hostname() too so a later rename takes effect on the next
     * DHCP renewal/reboot, though it won't retroactively relabel an
     * already-leased entry -- same caveat as any DHCP hostname. */
    char dhcp_host[64];
    board_hostname(dhcp_host, sizeof(dhcp_host));
    esp_netif_set_hostname(s_sta_netif, dhcp_host);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));

    char ssid[33] = {0}, pass[65] = {0};
    if (load_creds_from_nvs(ssid, sizeof(ssid), pass, sizeof(pass)) != ESP_OK) {
        strncpy(ssid, CONFIG_WIFI_SSID, sizeof(ssid) - 1);
        strncpy(pass, CONFIG_WIFI_PASSWORD, sizeof(pass) - 1);
    }
    /* Rescue gesture (BOOT held ~3 s) asks for the setup AP even though
     * credentials exist -- see board_boot_button_start(). One-shot: the flag
     * is cleared here, so the next ordinary reboot goes back to the saved
     * network. Credentials are untouched throughout. */
    if (take_forced_ap_flag()) {
        s_forced_ap = true;
        ESP_LOGW(TAG, "setup AP requested from the BOOT button -- saved credentials kept");
        return start_provisioning();
    }

    if (ssid[0] == '\0') {
        /* Nothing saved anywhere -> bring up the setup AP instead of idling.
         * SNTP/mDNS wait for the real network after the user configures one. */
        return start_provisioning();
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* Disable modem power-save. The default (WIFI_PS_MIN_MODEM) sleeps the
     * radio between DTIM beacons, which for this always-on, mains-powered
     * gateway just means ~100 ms+ HTTP latency and intermittent "no route to
     * host" dropouts under load -- bad for a device whose whole job is to
     * serve a web UI and (eventually) carry VoIP RTP. Keep the radio awake. */
    esp_wifi_set_ps(WIFI_PS_NONE);

    /* Background SNTP client -- syncs whenever there's connectivity, no need
     * to gate this on wifi being up yet. */
    esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    ESP_ERROR_CHECK(esp_netif_sntp_init(&sntp_cfg));

    /* mDNS responder -- reachable as <board name>.local once connected. */
    err = mdns_init();
    if (err == ESP_OK) {
        s_mdns_ready = true;
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
        /* Hostname is set later, from the got-IP handler: the collision check
         * in wifi_apply_hostname() has to query the LAN, which needs an
         * active connection. */
    } else {
        ESP_LOGW(TAG, "mdns_init failed: %s (no .local hostname, IP still works)",
                 esp_err_to_name(err));
    }

    return do_connect(ssid, pass);
}

void wifi_apply_hostname(void)
{
    char base[64], name[32];
    board_hostname(base, sizeof(base));
    board_name_get(name, sizeof(name));

    /* Refresh the DHCP hostname too (see wifi_init()'s comment) so a rename
     * takes effect on the router's client list on the next lease renewal --
     * independent of mDNS, so this runs even if mdns_init() never came up. */
    esp_netif_set_hostname(s_sta_netif, base);

    if (!s_mdns_ready) return;

    char host[80];
    strncpy(host, base, sizeof(host) - 1);
    host[sizeof(host) - 1] = '\0';

    /* Don't stomp another <host>.local already on the LAN (e.g. the Pi at
     * phone.local). Query mDNS first; if someone answers, append a digit and
     * try again. We query before setting our own hostname, so we can't answer
     * our own query. mdns_query_a() returns ESP_OK only when something
     * resolved -- NOT_FOUND (free) or any error ends the search. */
    bool bumped = false;
    for (int n = 1; n <= 9; n++) {
        esp_ip4_addr_t addr;
        if (mdns_query_a(host, 800, &addr) != ESP_OK) break;
        ESP_LOGW(TAG, "mDNS: '%s.local' already answered by " IPSTR " -- trying another",
                 host, IP2STR(&addr));
        snprintf(host, sizeof(host), "%s%d", base, n);
        bumped = true;
    }

    mdns_hostname_set(host);
    if (bumped && !board_name_is_set()) {
        /* First boot, default name clashed: persist the picked one so the
         * address stays stable across reboots even while the other board is
         * up, and so the displayed name matches the .local address. */
        board_name_set(host);
        mdns_instance_name_set(host);
        ESP_LOGW(TAG, "auto-named this board '%s' (reachable as %s.local) to avoid a "
                      "clash on the network -- rename it in Settings if you like",
                 host, host);
    } else if (bumped) {
        mdns_instance_name_set(name);
        ESP_LOGW(TAG, "board name '%s' clashes on the network -- reachable as %s.local; "
                      "pick a unique name in Settings", name, host);
    } else {
        mdns_instance_name_set(name);
        ESP_LOGI(TAG, "mDNS: '%s' reachable as %s.local", name, host);
    }
}

esp_err_t wifi_set_credentials(const char *ssid, const char *password)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, "ssid", ssid);
    if (err == ESP_OK) err = nvs_set_str(h, "pass", password);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) return err;

    s_connected = false;
    return do_connect(ssid, password);
}

esp_err_t wifi_clear_credentials(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    /* NOT_FOUND is fine -- means there was nothing saved to begin with. */
    nvs_erase_key(h, "ssid");
    nvs_erase_key(h, "pass");
    err = nvs_commit(h);
    nvs_close(h);
    ESP_LOGW(TAG, "WiFi credentials erased from NVS");
    return err;
}

bool wifi_is_connected(void) { return s_connected; }

int8_t wifi_rssi(void)
{
    wifi_ap_record_t info;
    if (!s_connected || esp_wifi_sta_get_ap_info(&info) != ESP_OK) return 0;
    return info.rssi;
}

void wifi_get_ip_str(char *buf, size_t buf_len)
{
    if (s_connected) {
        snprintf(buf, buf_len, IPSTR, IP2STR(&s_ip));
    } else {
        strncpy(buf, "0.0.0.0", buf_len - 1);
        buf[buf_len - 1] = 0;
    }
}

const char *wifi_get_ssid(void) { return s_ssid; }

void wifi_set_power_save(bool eco)
{
    /* Only called by idle_power.c, and only once the system's been fully
     * idle for a while with no VoIP server configured -- see
     * archive/POWER_SAVING.md. WIFI_PS_MIN_MODEM is exactly the mode
     * wifi_init()'s own comment above documents as bad for this device
     * generally (RTP timing, HTTP latency); it's safe here specifically
     * because idle_power.c never engages it while a call could be in
     * progress or expected. No-op if WiFi isn't actually associated (nothing
     * to sleep between DTIM beacons for). */
    if (eco && !s_connected) return;
    esp_wifi_set_ps(eco ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE);
}

static int scan_rssi_cmp(const void *a, const void *b)
{
    const wifi_scan_result_t *ra = a, *rb = b;
    return (int)rb->rssi - (int)ra->rssi;
}

int wifi_scan(wifi_scan_result_t *out, int max)
{
    wifi_scan_config_t scan_cfg = { .show_hidden = false };
    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true /* block until done */);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wifi_scan: scan_start failed: %s", esp_err_to_name(err));
        return -1;
    }

    uint16_t count = 0;
    esp_wifi_scan_get_ap_num(&count);
    if (count == 0) return 0;

    wifi_ap_record_t *recs = calloc(count, sizeof(*recs));
    if (!recs) return -1;
    err = esp_wifi_scan_get_ap_records(&count, recs);
    if (err != ESP_OK) {
        free(recs);
        return -1;
    }

    int n = 0;
    for (int i = 0; i < (int)count && n < max; i++) {
        const char *ssid = (const char *)recs[i].ssid;
        if (!ssid[0]) continue; /* hidden network -- nothing to show in a picker */

        /* Repeaters/mesh nodes can advertise the same SSID from several
         * BSSIDs -- keep only the strongest one already collected. */
        bool dup = false;
        for (int j = 0; j < n; j++) {
            if (strcmp(out[j].ssid, ssid) == 0) {
                dup = true;
                if (recs[i].rssi > out[j].rssi) {
                    out[j].rssi = recs[i].rssi;
                    out[j].secured = recs[i].authmode != WIFI_AUTH_OPEN;
                }
                break;
            }
        }
        if (dup) continue;

        strncpy(out[n].ssid, ssid, sizeof(out[n].ssid) - 1);
        out[n].ssid[sizeof(out[n].ssid) - 1] = '\0';
        out[n].rssi = recs[i].rssi;
        out[n].secured = recs[i].authmode != WIFI_AUTH_OPEN;
        n++;
    }
    free(recs);

    qsort(out, n, sizeof(*out), scan_rssi_cmp);
    return n;
}

esp_err_t wifi_set_tx_power_dbm(int dbm)
{
    if (dbm < 2) dbm = 2;
    if (dbm > 20) dbm = 20;
    return esp_wifi_set_max_tx_power((int8_t)(dbm * 4)); /* units of 0.25 dBm */
}

int wifi_get_tx_power_dbm(void)
{
    int8_t q = 0;
    if (esp_wifi_get_max_tx_power(&q) != ESP_OK) return 0;
    return (q + 2) / 4;
}

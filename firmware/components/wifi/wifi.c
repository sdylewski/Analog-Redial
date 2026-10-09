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
#include "freertos/event_groups.h"
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
static char           s_live_host[80]; /* mDNS hostname actually claimed, "" before */
static char           s_live_name[40]; /* name + clash suffix, "" when no clash */
static esp_netif_t    *s_sta_netif;   /* for esp_netif_set_hostname() -- DHCP hostname */

/* ---- known networks ----
 * Up to WIFI_MAX_NETWORKS saved SSID/password pairs in NVS as ssid0..ssidN /
 * pass0..passN. At connect time we scan and take the *strongest* one that's
 * in the list, so carrying the board between home and a friend's house just
 * works. Boards saved before this existed have a single legacy "ssid"/"pass"
 * pair; migrate_legacy_creds() folds that into slot 0 once. */
static esp_err_t known_get(int slot, char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return ESP_FAIL;
    char k[12];
    snprintf(k, sizeof(k), "ssid%d", slot);
    size_t n = ssid_len;
    esp_err_t err = nvs_get_str(h, k, ssid, &n);
    if (err == ESP_OK && pass) {
        snprintf(k, sizeof(k), "pass%d", slot);
        n = pass_len;
        if (nvs_get_str(h, k, pass, &n) != ESP_OK) pass[0] = '\0';
    }
    nvs_close(h);
    return err;
}

/* Per saved-network slot: set when the router refuses that network's
 * password (or its lack of one), cleared when it connects or the password
 * is saved again. RAM only -- a reboot simply retries. */
static bool s_bad_password[WIFI_MAX_NETWORKS];
static int  s_trying_slot = -1;

static bool is_password_reason(int reason)
{
    return reason == WIFI_REASON_AUTH_FAIL || reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
           reason == WIFI_REASON_HANDSHAKE_TIMEOUT || reason == WIFI_REASON_MIC_FAILURE ||
           reason == WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY;
}

static esp_err_t known_put(int slot, const char *ssid, const char *pass)
{
    if (slot >= 0 && slot < WIFI_MAX_NETWORKS) s_bad_password[slot] = false;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    char k[12];
    snprintf(k, sizeof(k), "ssid%d", slot);
    err = nvs_set_str(h, k, ssid);
    if (err == ESP_OK) {
        snprintf(k, sizeof(k), "pass%d", slot);
        err = nvs_set_str(h, k, pass ? pass : "");
    }
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

/* One-time: fold a pre-list "ssid"/"pass" pair into slot 0. */
static void migrate_legacy_creds(void)
{
    char s0[33] = {0};
    if (known_get(0, s0, sizeof(s0), NULL, 0) == ESP_OK && s0[0]) return; /* already migrated */
    char ssid[33] = {0}, pass[65] = {0};
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t n = sizeof(ssid);
    bool have = (nvs_get_str(h, "ssid", ssid, &n) == ESP_OK) && ssid[0];
    if (have) { n = sizeof(pass); if (nvs_get_str(h, "pass", pass, &n) != ESP_OK) pass[0] = '\0'; }
    nvs_close(h);
    if (have) {
        known_put(0, ssid, pass);
        ESP_LOGI(TAG, "migrated saved network '%s' into the known-networks list", ssid);
    }
}

int wifi_known_list(wifi_known_net_t *out, int max)
{
    migrate_legacy_creds();
    int n = 0;
    for (int i = 0; i < WIFI_MAX_NETWORKS && n < max; i++) {
        char ssid[33] = {0}, pass[65] = {0};
        if (known_get(i, ssid, sizeof(ssid), pass, sizeof(pass)) == ESP_OK && ssid[0]) {
            strlcpy(out[n].ssid, ssid, sizeof(out[n].ssid));
            out[n].has_password = pass[0] != '\0';
            out[n].bad_password = s_bad_password[i];
            out[n].slot = i;
            n++;
        }
    }
    return n;
}

esp_err_t wifi_known_add(const char *ssid, const char *pass)
{
    if (!ssid || !ssid[0]) return ESP_ERR_INVALID_ARG;
    migrate_legacy_creds();
    int free_slot = -1;
    for (int i = 0; i < WIFI_MAX_NETWORKS; i++) {
        char s0[33] = {0};
        if (known_get(i, s0, sizeof(s0), NULL, 0) == ESP_OK && s0[0]) {
            if (strcmp(s0, ssid) == 0) return known_put(i, ssid, pass); /* update in place */
        } else if (free_slot < 0) {
            free_slot = i;
        }
    }
    if (free_slot < 0) return ESP_ERR_NO_MEM; /* list full */
    return known_put(free_slot, ssid, pass);
}

esp_err_t wifi_known_remove(const char *ssid)
{
    if (!ssid || !ssid[0]) return ESP_ERR_INVALID_ARG;
    for (int i = 0; i < WIFI_MAX_NETWORKS; i++) {
        char s0[33] = {0};
        if (known_get(i, s0, sizeof(s0), NULL, 0) == ESP_OK && strcmp(s0, ssid) == 0) {
            nvs_handle_t h;
            if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return ESP_FAIL;
            char k[12];
            snprintf(k, sizeof(k), "ssid%d", i); nvs_erase_key(h, k);
            snprintf(k, sizeof(k), "pass%d", i); nvs_erase_key(h, k);
            nvs_commit(h); nvs_close(h);
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

/* ---- connection manager ----
 * One task owns every connect decision; the event handler only reports what
 * happened. Policy:
 *
 *   1. Scan. Candidates are saved networks that show up in the scan right
 *      now -- a network that isn't on the air isn't tried at all, which is
 *      what keeps a missing network from eating minutes of retries. (So a
 *      hidden SSID, which never appears in a scan, is never tried either.)
 *   2. The network we were last on goes first, then the rest strongest
 *      first. Each gets CONNECT_ATTEMPTS tries, with a growing pause
 *      between them (RETRY_BACKOFF_MS).
 *   3. None connected: bring up the setup AP, and rescan every
 *      FALLBACK_RESCAN_MS. When a saved network reappears (e.g. the router
 *      booted slower than we did after a power cut), connect and drop the AP.
 *      Rescans are skipped while someone is joined to the AP, because a scan
 *      briefly takes the radio off the AP's channel.
 *   4. Connected: wait for the link to drop, then start over at 1 -- which
 *      tries the same network first again.
 *
 * Everything here runs on the manager's own stack. The system event task's
 * stack is only CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE (2304 bytes); doing
 * a scan from the handler overflowed it once already. */
#define CONNECT_ATTEMPTS     3
#define CONNECT_TIMEOUT_MS   15000
#define RETRY_BACKOFF_MS     2000   /* wait 2 s before try 2, 4 s before try 3 */
#define FALLBACK_RESCAN_MS   60000

#define EV_GOT_IP  BIT0
#define EV_DISC    BIT1
#define EV_KICK    BIT2   /* re-plan now: credentials changed */

static EventGroupHandle_t s_ev;
static TaskHandle_t       s_mgr_task;

static void ap_up(const char *why);   /* defined with the SoftAP below */
static void ap_down(void);
static int  ap_client_count(void);

static void apply_sta_config(const char *ssid, const char *pass)
{
    wifi_config_t wc;
    memset(&wc, 0, sizeof(wc)); /* a union: "= {0}" only covers its first member */
    strncpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, pass, sizeof(wc.sta.password) - 1);
    wc.sta.threshold.authmode = strlen(pass) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    /* WPA3: allow both SAE password-element methods. A zeroed config means
     * "unspecified", which doesn't cover routers that require hash-to-
     * element (H2E) -- those fail authentication with reason 202 even with
     * the right password. PMF "capable" lets WPA2/WPA3 transition networks
     * use WPA3. */
    wc.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    wc.sta.pmf_cfg.capable = true;
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    strncpy(s_ssid, ssid, sizeof(s_ssid) - 1);
}

/* One attempt. True once we have an IP. */
static bool try_connect(const char *ssid, const char *pass, int attempt)
{
    xEventGroupClearBits(s_ev, EV_GOT_IP | EV_DISC);
    apply_sta_config(ssid, pass);
    ESP_LOGI(TAG, "connecting to '%s' (try %d of %d)", ssid, attempt, CONNECT_ATTEMPTS);
    board_led_set_state(BOARD_LED_WIFI_CONNECTING);
    if (esp_wifi_connect() != ESP_OK) return false;

    EventBits_t b = xEventGroupWaitBits(s_ev, EV_GOT_IP | EV_DISC | EV_KICK, pdFALSE, pdFALSE,
                                        pdMS_TO_TICKS(CONNECT_TIMEOUT_MS));
    if (b & EV_GOT_IP) return true;
    if (!(b & (EV_DISC | EV_KICK))) {
        ESP_LOGW(TAG, "'%s': no answer in %d s", ssid, CONNECT_TIMEOUT_MS / 1000);
        esp_wifi_disconnect();
        /* Let the resulting disconnect event land now, so it can't be
         * mistaken for the next attempt failing. */
        xEventGroupWaitBits(s_ev, EV_DISC, pdTRUE, pdFALSE, pdMS_TO_TICKS(2000));
    }
    return false;
}

typedef struct { int slot; int8_t rssi; } candidate_t;

/* Saved networks that are on the air now: the current one first, then by
 * signal strength (wifi_scan() returns strongest first). */
static int build_candidates(candidate_t *out, int max)
{
    wifi_known_net_t known[WIFI_MAX_NETWORKS];
    int nk = wifi_known_list(known, WIFI_MAX_NETWORKS);
    if (nk == 0) return 0;

    wifi_scan_result_t *scan = calloc(32, sizeof(*scan));
    if (!scan) return 0;
    int ns = wifi_scan(scan, 32);

    int n = 0;
    for (int i = 0; i < ns && n < max; i++) {
        for (int k = 0; k < nk; k++) {
            if (strcmp(scan[i].ssid, known[k].ssid) != 0) continue;
            candidate_t c = { known[k].slot, scan[i].rssi };
            if (s_ssid[0] && strcmp(known[k].ssid, s_ssid) == 0) {
                memmove(&out[1], &out[0], n * sizeof(*out));   /* current one first */
                out[0] = c;
            } else {
                out[n] = c;
            }
            n++;
        }
    }
    free(scan);
    ESP_LOGI(TAG, "%d of %d saved network(s) in range", n, nk);
    return n;
}

static void manager_task(void *arg)
{
    for (;;) {
        xEventGroupClearBits(s_ev, EV_KICK);
        candidate_t cand[WIFI_MAX_NETWORKS];
        int n = build_candidates(cand, WIFI_MAX_NETWORKS);

        bool connected = false;
        for (int i = 0; i < n && !connected; i++) {
            char ssid[33] = {0}, pass[65] = {0};
            if (known_get(cand[i].slot, ssid, sizeof(ssid), pass, sizeof(pass)) != ESP_OK) continue;
            s_trying_slot = cand[i].slot;
            for (int a = 1; a <= CONNECT_ATTEMPTS && !connected; a++) {
                if (a > 1) {
                    /* Back off before retrying. A rejected attempt fails in
                     * ~0.3 s, and routers often refuse an immediate retry:
                     * on 2026-10-02 the SMD2 burned all three tries in one
                     * second with AUTH_FAIL (202), then joined the same
                     * network with the same password on the next pass. */
                    xEventGroupWaitBits(s_ev, EV_KICK, pdFALSE, pdFALSE,
                                        pdMS_TO_TICKS(RETRY_BACKOFF_MS * (a - 1)));
                    if (xEventGroupGetBits(s_ev) & EV_KICK) break;
                }
                connected = try_connect(ssid, pass, a);
                if (xEventGroupGetBits(s_ev) & EV_KICK) break;
            }
            if (xEventGroupGetBits(s_ev) & EV_KICK) break;
        }
        if (xEventGroupGetBits(s_ev) & EV_KICK) continue;

        if (connected) {
            ap_down();
            /* Stay here until the link drops or someone changes the saved
             * networks, then go back round -- current network first. */
            EventBits_t b = xEventGroupWaitBits(s_ev, EV_DISC | EV_KICK, pdFALSE, pdFALSE,
                                                portMAX_DELAY);
            if (b & EV_KICK) esp_wifi_disconnect();
            continue;
        }

        ap_up(n ? "couldn't join any saved network" : "no saved network in range");
        /* Wait, then look again -- unless someone is using the setup AP. */
        do {
            xEventGroupWaitBits(s_ev, EV_KICK, pdFALSE, pdFALSE, pdMS_TO_TICKS(FALLBACK_RESCAN_MS));
        } while (!(xEventGroupGetBits(s_ev) & EV_KICK) && ap_client_count() > 0);
    }
}

/* Re-plan now (credentials changed). */
static void manager_kick(void)
{
    if (s_ev) xEventGroupSetBits(s_ev, EV_KICK);
}

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        /* An IPv6 link-local address lets mDNS answer AAAA queries for
         * <name>.local. Without one, macOS waits ~5 s for an AAAA answer that
         * never comes before using the IPv4 address. */
        esp_netif_create_ip6_linklocal(s_sta_netif);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        /* The reason code is what tells a flaky link (beacon timeout, 200+,
         * e.g. radio interference) apart from the AP deliberately kicking us
         * (auth/assoc codes) -- see wifi_err_reason_t in esp_wifi_types.h. */
        const wifi_event_sta_disconnected_t *d = (const wifi_event_sta_disconnected_t *)data;
        if (s_connected) {
            ESP_LOGW(TAG, "WiFi disconnected (reason %d, RSSI %d dBm)",
                     d ? d->reason : -1, d ? d->rssi : 0);
        } else if (d) {
            ESP_LOGI(TAG, "'%s' didn't connect (reason %d)", s_ssid, d->reason);
            if (is_password_reason(d->reason) && s_trying_slot >= 0) {
                s_bad_password[s_trying_slot] = true;
            }
        }
        s_connected = false;
        if (s_ev) xEventGroupSetBits(s_ev, EV_DISC);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        s_ip = e->ip_info.ip;
        s_connected = true;
        if (s_trying_slot >= 0) s_bad_password[s_trying_slot] = false;
        board_led_set_state(BOARD_LED_OK);
        ESP_LOGI(TAG, "connected to '%s', IP=" IPSTR, s_ssid, IP2STR(&s_ip));
        if (s_ev) xEventGroupSetBits(s_ev, EV_GOT_IP);
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

static esp_netif_t *s_ap_netif;
static bool         s_dns_started;

/* Brings up the open setup AP alongside the station (AP+STA, so scans keep
 * working for both the setup page's dropdown and the manager's rescans). */
static void ap_up(const char *why)
{
    if (s_provisioning) return;
    s_provisioning = true;
    if (!s_ap_netif) s_ap_netif = esp_netif_create_default_wifi_ap();

    char ssid[33];
    wifi_setup_ap_name(ssid, sizeof(ssid));
    wifi_config_t ap;
    memset(&ap, 0, sizeof(ap)); /* a union: "= {0}" only covers its first member */
    size_t n = strlen(ssid);
    memcpy(ap.ap.ssid, ssid, n);           /* SSIDs aren't NUL-terminated */
    ap.ap.ssid_len = (uint8_t)n;
    ap.ap.channel = 1;
    ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_OPEN; /* open network: easiest to join for setup */

    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &ap);
    esp_wifi_start();                 /* no-op if the station already started it */

    board_led_set_state(BOARD_LED_WIFI_CONNECTING); /* "needs setup" */
    if (!s_dns_started) {
        s_dns_started = true;
        xTaskCreate(captive_dns_task, "captdns", 3072, NULL, 4, NULL);
    }
    ESP_LOGW(TAG, "%s -- started setup AP '%s' (open). Join it, then a setup page opens "
                  "(or browse to http://192.168.4.1)", why, ssid);
}

/* Back to station-only once a saved network has connected. The BOOT-button
 * rescue AP stays up regardless -- that's the point of asking for it. */
static void ap_down(void)
{
    if (!s_provisioning || s_forced_ap) return;
    esp_wifi_set_mode(WIFI_MODE_STA);
    s_provisioning = false;
    ESP_LOGI(TAG, "on a saved network -- setup AP closed");
}

static int ap_client_count(void)
{
    if (!s_provisioning) return 0;
    wifi_sta_list_t list;
    return esp_wifi_ap_get_sta_list(&list) == ESP_OK ? list.num : 0;
}

void wifi_setup_ap_name(char *out, size_t len)
{
    if (len == 0) return;
    if (board_name_is_set()) {
        char name[32];
        board_name_get(name, sizeof(name));
        snprintf(out, len, "Analog Redial - %s", name);
        if (len > 32) out[32] = '\0';     /* 32-byte SSID limit */
        /* If that cut through a UTF-8 character, drop the partial one. */
        size_t n = strlen(out), i = n;
        while (i > 0 && ((uint8_t)out[i - 1] & 0xC0) == 0x80) i--;   /* continuation bytes */
        if (i > 0) {
            uint8_t lead = (uint8_t)out[i - 1];
            size_t want = lead < 0x80 ? 1 : lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : 2;
            if (n - (i - 1) < want) out[i - 1] = '\0';
        }
        return;
    }
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(out, len, "AnalogRedial-%02X%02X", mac[4], mac[5]);
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
    wifi_known_add(ssid, password); /* remember it alongside any others */
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

    migrate_legacy_creds();
    wifi_known_net_t probe[WIFI_MAX_NETWORKS];
    int nk = wifi_known_list(probe, WIFI_MAX_NETWORKS);
    if (nk == 0 && CONFIG_WIFI_SSID[0]) {
        /* Nothing saved: fall back to any build-time default. */
        wifi_known_add(CONFIG_WIFI_SSID, CONFIG_WIFI_PASSWORD);
        nk = 1;
    }
    /* Rescue gesture (BOOT held ~3 s) asks for the setup AP even though
     * credentials exist -- see board_boot_button_start(). One-shot: the flag
     * is cleared here, so the next ordinary reboot goes back to the saved
     * network. Credentials are untouched throughout. */
    if (take_forced_ap_flag()) {
        s_forced_ap = true;
        ap_up("setup AP requested from the BOOT button (saved networks kept)");
        return ESP_OK;
    }
    if (nk == 0) {
        /* Nothing saved anywhere -> the setup AP instead of idling. SNTP and
         * mDNS wait for the real network after the user configures one (the
         * setup page reboots into station mode). */
        ap_up("no WiFi credentials");
        return ESP_OK;
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
        /* Hostname (and the _http service, which mdns refuses to add until a
         * hostname exists) are set later from the got-IP handler: the
         * collision check in wifi_apply_hostname() needs the LAN. */
    } else {
        ESP_LOGW(TAG, "mdns_init failed: %s (no .local hostname, IP still works)",
                 esp_err_to_name(err));
    }

    s_ev = xEventGroupCreate();
    if (!s_ev || xTaskCreate(manager_task, "wifi_mgr", 4096, NULL, 3, &s_mgr_task) != pdPASS) {
        ESP_LOGE(TAG, "couldn't start the WiFi connection manager");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
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
    int suffix = 0;
    for (int n = 1; n <= 9; n++) {
        esp_ip4_addr_t addr;
        if (mdns_query_a(host, 800, &addr) != ESP_OK) break;
        ESP_LOGW(TAG, "mDNS: '%s.local' already answered by " IPSTR " -- trying another",
                 host, IP2STR(&addr));
        snprintf(host, sizeof(host), "%s%d", base, n);
        suffix = n;
        bumped = true;
    }

    mdns_hostname_set(host);
    strlcpy(s_live_host, host, sizeof(s_live_host));
    if (!mdns_service_exists("_http", "_tcp", NULL)) {
        esp_err_t serr = mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
        if (serr != ESP_OK) ESP_LOGW(TAG, "mDNS _http service: %s", esp_err_to_name(serr));
    }
    if (bumped) {
        /* One name everywhere for as long as the clash lasts: web UI, .local
         * address and router list all say e.g. "phone1". Deliberately NOT
         * saved -- a board prepared next to another one must still come up
         * as plain "phone" in a house where it's alone. name + digit (not
         * host) keeps "Kitchen" readable as "Kitchen1". */
        snprintf(s_live_name, sizeof(s_live_name), "%.30s%d", name, suffix);
        esp_netif_set_hostname(s_sta_netif, host);
        mdns_instance_name_set(s_live_name);
        ESP_LOGW(TAG, "'%s.local' is taken on this network -- using '%s' (%s.local) "
                      "until the next reboot", base, s_live_name, host);
    } else {
        s_live_name[0] = '\0';
        mdns_instance_name_set(name);
        ESP_LOGI(TAG, "mDNS: '%s' reachable as %s.local", name, host);
    }
}

void wifi_live_name(char *out, size_t len)
{
    if (s_live_name[0]) strlcpy(out, s_live_name, len);
    else board_name_get(out, len);
}

void wifi_live_hostname(char *out, size_t len)
{
    if (s_live_host[0]) strlcpy(out, s_live_host, len);
    else board_hostname(out, len);
}

esp_err_t wifi_set_credentials(const char *ssid, const char *password)
{
    wifi_known_add(ssid, password); /* remember it alongside any others */
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, "ssid", ssid);
    if (err == ESP_OK) err = nvs_set_str(h, "pass", password);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) return err;

    /* Make it the "current" network so the manager tries it first. */
    strncpy(s_ssid, ssid, sizeof(s_ssid) - 1);
    s_ssid[sizeof(s_ssid) - 1] = '\0';
    manager_kick();
    return ESP_OK;
}

esp_err_t wifi_clear_credentials(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    /* NOT_FOUND is fine -- means there was nothing saved to begin with. The
     * saved-networks slots must go too: wifi_init() joins any of them, so
     * erasing only the legacy pair never actually reached setup mode. */
    nvs_erase_key(h, "ssid");
    nvs_erase_key(h, "pass");
    for (int i = 0; i < WIFI_MAX_NETWORKS; i++) {
        char k[12];
        snprintf(k, sizeof(k), "ssid%d", i); nvs_erase_key(h, k);
        snprintf(k, sizeof(k), "pass%d", i); nvs_erase_key(h, k);
    }
    err = nvs_commit(h);
    nvs_close(h);
    ESP_LOGW(TAG, "all saved WiFi networks erased from NVS");
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
                    out[j].authmode = recs[i].authmode;
                }
                break;
            }
        }
        if (dup) continue;

        strncpy(out[n].ssid, ssid, sizeof(out[n].ssid) - 1);
        out[n].ssid[sizeof(out[n].ssid) - 1] = '\0';
        out[n].rssi = recs[i].rssi;
        out[n].secured = recs[i].authmode != WIFI_AUTH_OPEN;
        out[n].authmode = recs[i].authmode;
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

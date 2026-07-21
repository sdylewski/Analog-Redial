#include "wifi.h"
#include "board.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "mdns.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>
#include <stdio.h>

/* Not "wifi" -- that tag belongs to the (very chatty) internal WiFi driver
 * logs, which we deliberately quiet below. A distinct tag lets our own
 * connect/status messages stay visible without fighting over one level. */
static const char *TAG = "wifi_app";
#define NVS_NS "wifi"

static bool           s_connected;
static char            s_ssid[33];
static esp_ip4_addr_t  s_ip;

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
        if (s_connected) ESP_LOGW(TAG, "WiFi disconnected, retrying");
        s_connected = false;
        if (s_ssid[0]) esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        s_ip = e->ip_info.ip;
        s_connected = true;
        board_led_set_state(BOARD_LED_OK);
        ESP_LOGI(TAG, "connected, IP=" IPSTR, IP2STR(&s_ip));
    }
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
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* Background SNTP client -- syncs whenever there's connectivity, no need
     * to gate this on wifi being up yet. */
    esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    ESP_ERROR_CHECK(esp_netif_sntp_init(&sntp_cfg));

    /* mDNS responder -- reachable as CONFIG_MDNS_HOSTNAME.local once
     * connected. Doesn't need to wait for an actual connection; it just
     * starts answering once there's an IP. */
    err = mdns_init();
    if (err == ESP_OK) {
        mdns_hostname_set(CONFIG_MDNS_HOSTNAME);
        mdns_instance_name_set("Rotary Phone Gateway");
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
        ESP_LOGI(TAG, "mDNS: reachable as %s.local", CONFIG_MDNS_HOSTNAME);
    } else {
        ESP_LOGW(TAG, "mdns_init failed: %s (no .local hostname, IP still works)",
                 esp_err_to_name(err));
    }

    char ssid[33] = {0}, pass[65] = {0};
    if (load_creds_from_nvs(ssid, sizeof(ssid), pass, sizeof(pass)) != ESP_OK) {
        strncpy(ssid, CONFIG_WIFI_SSID, sizeof(ssid) - 1);
        strncpy(pass, CONFIG_WIFI_PASSWORD, sizeof(pass) - 1);
    }
    if (ssid[0] == '\0') {
        ESP_LOGW(TAG, "no WiFi credentials (NVS empty, CONFIG_WIFI_SSID empty) -- "
                      "run 'wifi_set <ssid> <password>' from the console, or set "
                      "CONFIG_WIFI_SSID/CONFIG_WIFI_PASSWORD via menuconfig and rebuild");
        return ESP_OK;
    }
    return do_connect(ssid, pass);
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

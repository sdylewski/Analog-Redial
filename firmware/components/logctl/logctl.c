#include "logctl.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>

static const char *TAG = "logctl";
#define LOG_NVS_NS  "logctl"
#define LOG_NVS_KEY "level"

static esp_log_level_t s_level = ESP_LOG_INFO;

/* WiFi/PHY stack tags that spam at DEBUG/VERBOSE and would bury the app's
 * own debug output (pulse/hook timing etc.). Re-pinned to WARN whenever the
 * global level is raised past WARN. */
static const char *QUIET_TAGS[] = {
    "wifi", "wifi_init", "phy_init", "net80211", "pp", "esp_netif_handlers",
    "esp_netif_lwip",
    /* esp_http_server's own per-request parse/session/tx-rx tracing --
     * every header line, every parse state transition. Not useful for
     * debugging this project's own code, and (see webui.c's .k CSS fix)
     * its tag names are also longer than anything else this firmware logs. */
    "httpd", "httpd_parse", "httpd_sess", "httpd_txrx", "httpd_uri",
};

static void apply(esp_log_level_t level)
{
    esp_log_level_set("*", level);
    if (level > ESP_LOG_WARN) {
        for (size_t i = 0; i < sizeof(QUIET_TAGS) / sizeof(QUIET_TAGS[0]); i++) {
            esp_log_level_set(QUIET_TAGS[i], ESP_LOG_WARN);
        }
    }
}

static void nvs_ready(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
}

void logctl_init(void)
{
    nvs_ready();
    nvs_handle_t h;
    if (nvs_open(LOG_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t v;
        if (nvs_get_u8(h, LOG_NVS_KEY, &v) == ESP_OK && v <= ESP_LOG_VERBOSE) {
            s_level = (esp_log_level_t)v;
        }
        nvs_close(h);
    }
    apply(s_level);
    ESP_LOGI(TAG, "log level = %s", logctl_level_name(s_level));
}

esp_err_t logctl_set_level(esp_log_level_t level)
{
    if (level > ESP_LOG_VERBOSE) return ESP_ERR_INVALID_ARG;
    s_level = level;
    apply(level);

    nvs_handle_t h;
    if (nvs_open(LOG_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, LOG_NVS_KEY, (uint8_t)level);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGW(TAG, "log level set to %s", logctl_level_name(level));
    return ESP_OK;
}

esp_log_level_t logctl_get_level(void)
{
    return s_level;
}

const char *logctl_level_name(esp_log_level_t level)
{
    switch (level) {
        case ESP_LOG_NONE:    return "none";
        case ESP_LOG_ERROR:   return "error";
        case ESP_LOG_WARN:    return "warn";
        case ESP_LOG_INFO:    return "info";
        case ESP_LOG_DEBUG:   return "debug";
        case ESP_LOG_VERBOSE: return "verbose";
        default:              return "?";
    }
}

esp_log_level_t logctl_level_from_name(const char *name)
{
    if (!name) return (esp_log_level_t)0xFF;
    if (strcmp(name, "none") == 0)    return ESP_LOG_NONE;
    if (strcmp(name, "error") == 0)   return ESP_LOG_ERROR;
    if (strcmp(name, "warn") == 0)    return ESP_LOG_WARN;
    if (strcmp(name, "info") == 0)    return ESP_LOG_INFO;
    if (strcmp(name, "debug") == 0)   return ESP_LOG_DEBUG;
    if (strcmp(name, "verbose") == 0) return ESP_LOG_VERBOSE;
    return (esp_log_level_t)0xFF;
}

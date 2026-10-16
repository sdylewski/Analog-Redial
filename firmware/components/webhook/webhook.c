#include "webhook.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "board.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"

static const char *TAG = "webhook";

#define NVS_NS      "webhook"
#define QUEUE_LEN   8
#define TASK_STACK  4608
#define TIMEOUT_MS  4000

typedef struct {
    char    event[20];   /* longest is "answering_machine" */
    int8_t  line;
    char    detail[72];
    int64_t time;
} wh_evt_t;

static char s_url[WEBHOOK_URL_MAX];
static QueueHandle_t s_q;
static volatile webhook_tap_t s_tap;
static volatile int s_last_status;
static volatile unsigned s_sent, s_dropped;

/* Copies s into out as a JSON string body (no quotes), dropping control
 * characters. */
static void json_escape(const char *s, char *out, size_t len)
{
    size_t o = 0;
    for (; s && *s && o + 2 < len; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
        else if (c >= 0x20)        { out[o++] = (char)c; }
    }
    out[o] = '\0';
}

static void post(const wh_evt_t *e)
{
    char name[40], ename[80], edetail[150], body[320];
    board_name_get(name, sizeof(name));
    json_escape(name, ename, sizeof(ename));
    json_escape(e->detail, edetail, sizeof(edetail));
    int n = snprintf(body, sizeof(body),
                     "{\"board\":\"%s\",\"event\":\"%s\",\"line\":%d,\"detail\":\"%s\",\"time\":%lld}",
                     ename, e->event, e->line, edetail, (long long)e->time);
    if (n < 0 || n >= (int)sizeof(body)) n = (int)strlen(body);

    esp_http_client_config_t cfg = {
        .url = s_url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = TIMEOUT_MS,
        .buffer_size = 512,
        .buffer_size_tx = 512,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) { s_last_status = -ESP_ERR_NO_MEM; return; }
    esp_http_client_set_header(c, "Content-Type", "application/json");
    esp_http_client_set_post_field(c, body, n);
    esp_err_t err = esp_http_client_perform(c);
    s_last_status = err == ESP_OK ? esp_http_client_get_status_code(c) : -err;
    esp_http_client_cleanup(c);
    if (err == ESP_OK) s_sent++;
    ESP_LOGD(TAG, "%s line %d -> %d", e->event, e->line, s_last_status);
}

static void task(void *arg)
{
    wh_evt_t e;
    for (;;) {
        if (xQueueReceive(s_q, &e, portMAX_DELAY) != pdTRUE) continue;
        if (s_url[0]) post(&e);
    }
}

static void ensure_started(void)
{
    if (s_q) return;
    s_q = xQueueCreate(QUEUE_LEN, sizeof(wh_evt_t));
    if (!s_q) return;
    if (xTaskCreate(task, "webhook", TASK_STACK, NULL, 3, NULL) != pdPASS) {
        vQueueDelete(s_q);
        s_q = NULL;
        ESP_LOGW(TAG, "no memory for the sender task");
    }
}

void webhook_init(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t n = sizeof(s_url);
        if (nvs_get_str(h, "url", s_url, &n) != ESP_OK) s_url[0] = '\0';
        nvs_close(h);
    }
    if (s_url[0]) ensure_started();
}

void webhook_set_tap(webhook_tap_t tap) { s_tap = tap; }

void webhook_event(const char *event, int line, const char *detail)
{
    /* Settings -> General -> Features: Home Assistant unticked. */
    if (board_feature_get("feat_ha") == 0) return;
    webhook_tap_t tap = s_tap;
    if (tap) tap(event, line, detail ? detail : "");
    if (!s_url[0] || !s_q) return;
    wh_evt_t e = { .line = (int8_t)line };
    strlcpy(e.event, event, sizeof(e.event));
    strlcpy(e.detail, detail ? detail : "", sizeof(e.detail));
    time_t now = time(NULL);
    e.time = now > 1600000000 ? (int64_t)now : 0;   /* 0 until SNTP has set the clock */
    if (xQueueSend(s_q, &e, 0) != pdTRUE) s_dropped++;
}

void webhook_eventf(const char *event, int line, const char *fmt, ...)
{
    if (!s_tap && (!s_url[0] || !s_q)) return;
    char d[72];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(d, sizeof(d), fmt, ap);
    va_end(ap);
    webhook_event(event, line, d);
}

void webhook_url_get(char *out, size_t len) { strlcpy(out, s_url, len); }

esp_err_t webhook_url_set(const char *url)
{
    if (url[0] && (strncmp(url, "http://", 7) != 0 || strlen(url) >= sizeof(s_url)))
        return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = url[0] ? nvs_set_str(h, "url", url) : nvs_erase_key(h, "url");
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) return err;
    strlcpy(s_url, url, sizeof(s_url));
    if (s_url[0]) ensure_started();
    s_last_status = 0;
    return ESP_OK;
}

int webhook_last_status(void)      { return s_last_status; }
unsigned webhook_sent_count(void)  { return s_sent; }
unsigned webhook_dropped_count(void) { return s_dropped; }

#include "mqtt_ha.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "activity.h"
#include "ag1170.h"
#include "auxpins.h"
#include "board.h"
#include "dialplan.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "nvs.h"
#include "telephony.h"
#include "webhook.h"

static const char *TAG = "mqtt_ha";

#define NVS_NS        "mqttha"
#define ID_LEN        16
#define TOPIC_LEN     80
#define BUF_LEN       896
#define MAX_LINES     2
#define TASK_STACK    3584
#define DIAG_EVERY_S  60

static mqtt_ha_cfg_t s_cfg;
static bool s_loaded;

static SemaphoreHandle_t s_lock;          /* guards s_client */
static esp_mqtt_client_handle_t s_client; /* NULL while off */
static volatile bool s_connected;
static volatile bool s_run;
static TaskHandle_t s_task;
static unsigned s_heap_cost;
static size_t s_heap_before;
static bool s_measure;   /* cost is only meaningful for a start after boot */
static char s_id[ID_LEN];                 /* "redial_c0b65c" */
static char s_uri[MQTT_HA_HOST_MAX + 16];
static char s_clientid[ID_LEN];

/* One timer rings a line for a command; a second ring command restarts it. */
static esp_timer_handle_t s_ring_timer;
static int s_ring_line;

/* ---- settings --------------------------------------------------------- */

static void load(void)
{
    if (s_loaded) return;
    s_loaded = true;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    uint8_t en = 0;
    nvs_get_u8(h, "en", &en);
    s_cfg.enabled = en != 0;
    size_t n = sizeof(s_cfg.host); nvs_get_str(h, "host", s_cfg.host, &n);
    n = sizeof(s_cfg.user);        nvs_get_str(h, "user", s_cfg.user, &n);
    n = sizeof(s_cfg.pass);        nvs_get_str(h, "pass", s_cfg.pass, &n);
    nvs_close(h);
}

static void put_str(nvs_handle_t h, const char *k, const char *v)
{
    if (v[0]) nvs_set_str(h, k, v); else nvs_erase_key(h, k);
}

mqtt_ha_cfg_t mqtt_ha_get(void) { load(); return s_cfg; }

/* ---- topics ----------------------------------------------------------- */

static void make_id(void)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_id, sizeof(s_id), "redial_%02x%02x%02x", mac[3], mac[4], mac[5]);
    snprintf(s_clientid, sizeof(s_clientid), "%s", s_id);
}

static int topic(char *out, size_t len, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static int topic(char *out, size_t len, const char *fmt, ...)
{
    int n = snprintf(out, len, "redial/%s/", s_id);
    if (n < 0 || (size_t)n >= len) return -1;
    va_list ap;
    va_start(ap, fmt);
    int m = vsnprintf(out + n, len - (size_t)n, fmt, ap);
    va_end(ap);
    return m < 0 ? -1 : n + m;
}

static void pub(esp_mqtt_client_handle_t c, const char *t, const char *payload, int qos, int retain)
{
    esp_mqtt_client_publish(c, t, payload, 0, qos, retain);
}

/* Minimal JSON string escape into out (no quotes). */
static void jesc(const char *s, char *out, size_t len)
{
    size_t o = 0;
    for (; s && *s && o + 2 < len; s++) {
        unsigned char ch = (unsigned char)*s;
        if (ch == '"' || ch == '\\') { out[o++] = '\\'; out[o++] = (char)ch; }
        else if (ch >= 0x20)         { out[o++] = (char)ch; }
    }
    out[o] = '\0';
}

/* ---- Home Assistant discovery ------------------------------------------ */

static void discovery(esp_mqtt_client_handle_t c, int nlines)
{
    char name[40], ename[80], hw[40], ehw[80];
    board_name_get(name, sizeof(name));
    board_hardware_str(hw, sizeof(hw));
    jesc(name, ename, sizeof(ename));
    jesc(hw, ehw, sizeof(ehw));

    char dev[256], avail[TOPIC_LEN], t[TOPIC_LEN], ct[TOPIC_LEN], st[TOPIC_LEN], at[TOPIC_LEN];
    snprintf(dev, sizeof(dev),
             "\"device\":{\"identifiers\":[\"%s\"],\"name\":\"%s\",\"manufacturer\":\"Analog Redial\",\"model\":\"%s\"}",
             s_id, ename, ehw);
    topic(avail, sizeof(avail), "availability");
    char body[BUF_LEN];

    for (int l = 0; l < nlines; l++) {
        topic(st, sizeof(st), "line/%d/state", l);
        topic(at, sizeof(at), "line/%d/attr", l);
        snprintf(t, sizeof(t), "homeassistant/sensor/%s/line%d/config", s_id, l);
        snprintf(body, sizeof(body),
                 "{\"name\":\"Line %d\",\"unique_id\":\"%s_line%d\",\"has_entity_name\":true,"
                 "\"icon\":\"mdi:phone-classic\",\"state_topic\":\"%s\",\"json_attributes_topic\":\"%s\","
                 "\"availability_topic\":\"%s\",%s}", l + 1, s_id, l, st, at, avail, dev);
        pub(c, t, body, 1, 1);

        topic(ct, sizeof(ct), "cmd/ring/%d", l);
        snprintf(t, sizeof(t), "homeassistant/button/%s/ring%d/config", s_id, l);
        snprintf(body, sizeof(body),
                 "{\"name\":\"Ring line %d\",\"unique_id\":\"%s_ring%d\",\"has_entity_name\":true,"
                 "\"icon\":\"mdi:bell-ring\",\"command_topic\":\"%s\",\"payload_press\":\"4\","
                 "\"availability_topic\":\"%s\",%s}", l + 1, s_id, l, ct, avail, dev);
        pub(c, t, body, 1, 1);
    }

    topic(st, sizeof(st), "event");
    snprintf(t, sizeof(t), "homeassistant/event/%s/phone/config", s_id);
    snprintf(body, sizeof(body),
             "{\"name\":\"Phone event\",\"unique_id\":\"%s_event\",\"has_entity_name\":true,"
             "\"state_topic\":\"%s\",\"event_types\":[\"offhook\",\"onhook\",\"dial\",\"wrong_number\","
             "\"ring\",\"answering_machine\",\"recording\"],\"availability_topic\":\"%s\",%s}",
             s_id, st, avail, dev);
    pub(c, t, body, 1, 1);

    topic(st, sizeof(st), "diag/rssi");
    snprintf(t, sizeof(t), "homeassistant/sensor/%s/rssi/config", s_id);
    snprintf(body, sizeof(body),
             "{\"name\":\"Wi-Fi signal\",\"unique_id\":\"%s_rssi\",\"has_entity_name\":true,"
             "\"device_class\":\"signal_strength\",\"unit_of_measurement\":\"dBm\","
             "\"entity_category\":\"diagnostic\",\"state_topic\":\"%s\",\"availability_topic\":\"%s\",%s}",
             s_id, st, avail, dev);
    pub(c, t, body, 1, 1);

    topic(st, sizeof(st), "diag/heap");
    snprintf(t, sizeof(t), "homeassistant/sensor/%s/heap/config", s_id);
    snprintf(body, sizeof(body),
             "{\"name\":\"Free memory\",\"unique_id\":\"%s_heap\",\"has_entity_name\":true,"
             "\"unit_of_measurement\":\"B\",\"entity_category\":\"diagnostic\","
             "\"state_topic\":\"%s\",\"availability_topic\":\"%s\",%s}", s_id, st, avail, dev);
    pub(c, t, body, 1, 1);

    /* One button per pin dial code (Settings -> Dial Codes -> Pin outputs). */
    dialplan_pin_entry_t pins[DIALPLAN_MAX_PIN_ACTIONS];
    size_t np = dialplan_list_pin_entries(pins, DIALPLAN_MAX_PIN_ACTIONS);
    for (size_t i = 0; i < np; i++) {
        char edesc[96];
        jesc(pins[i].desc[0] ? pins[i].desc : "Pin output", edesc, sizeof(edesc));
        topic(ct, sizeof(ct), "cmd/pin/%s", pins[i].code);
        snprintf(t, sizeof(t), "homeassistant/button/%s/pin%s/config", s_id, pins[i].code);
        snprintf(body, sizeof(body),
                 "{\"name\":\"%s (GPIO%d)\",\"unique_id\":\"%s_pin%s\",\"has_entity_name\":true,"
                 "\"icon\":\"mdi:electric-switch\",\"command_topic\":\"%s\",\"payload_press\":\"1\","
                 "\"availability_topic\":\"%s\",%s}", edesc, pins[i].gpio, s_id, pins[i].code, ct, avail, dev);
        pub(c, t, body, 1, 1);
    }
}

/* ---- commands --------------------------------------------------------- */

static void ring_stop_cb(void *arg)
{
    (void)arg;
    ag1170_ring_stop(s_ring_line);
}

static void cmd_ring(int line, int secs)
{
    int nlines = board_hw_config_get().num_lines;
    if (line < 0 || line >= nlines) return;
    if (secs < 1 || secs > 15) secs = 4;
    /* Same rule as the web UI's test ring: never ring a lifted handset. */
    if (ag1170_is_offhook(line) || telephony_state(line) != TEL_IDLE) {
        ESP_LOGI(TAG, "ring line %d refused: not idle", line);
        return;
    }
    if (!s_ring_timer) {
        const esp_timer_create_args_t a = { .callback = ring_stop_cb, .name = "mqttring" };
        if (esp_timer_create(&a, &s_ring_timer) != ESP_OK) return;
    }
    esp_timer_stop(s_ring_timer);
    s_ring_line = line;
    ag1170_ring_start(line);
    esp_timer_start_once(s_ring_timer, (uint64_t)secs * 1000000);
    activity_log("Home Assistant rang line %d", line);
}

static void cmd_pin(const char *code)
{
    dialplan_pin_entry_t pins[DIALPLAN_MAX_PIN_ACTIONS];
    size_t n = dialplan_list_pin_entries(pins, DIALPLAN_MAX_PIN_ACTIONS);
    for (size_t i = 0; i < n; i++) {
        if (strcmp(pins[i].code, code) != 0) continue;
        esp_err_t err = auxpins_pulse(pins[i].gpio, pins[i].secs);
        if (err == ESP_OK) activity_log("Home Assistant fired pin code %s (GPIO%d)", code, pins[i].gpio);
        else ESP_LOGW(TAG, "pin code %s not run: %s", code, esp_err_to_name(err));
        return;
    }
}

static void on_data(esp_mqtt_event_handle_t ev)
{
    /* Commands are tiny; ignore anything split across events. */
    if (ev->current_data_offset != 0 || ev->data_len != ev->total_data_len) return;
    char tp[TOPIC_LEN], pl[16];
    if (ev->topic_len <= 0 || ev->topic_len >= (int)sizeof(tp)) return;
    memcpy(tp, ev->topic, (size_t)ev->topic_len); tp[ev->topic_len] = '\0';
    int pn = ev->data_len < (int)sizeof(pl) - 1 ? ev->data_len : (int)sizeof(pl) - 1;
    memcpy(pl, ev->data, (size_t)pn); pl[pn] = '\0';

    char prefix[TOPIC_LEN];
    topic(prefix, sizeof(prefix), "cmd/");
    size_t pl_len = strlen(prefix);
    if (strncmp(tp, prefix, pl_len) != 0) return;
    const char *rest = tp + pl_len;
    if (strncmp(rest, "ring/", 5) == 0) cmd_ring(atoi(rest + 5), atoi(pl));
    else if (strncmp(rest, "pin/", 4) == 0) cmd_pin(rest + 4);
}

/* ---- state publishing ------------------------------------------------- */

static const char *line_state_name(int l) { return telephony_state_name(telephony_state(l)); }

typedef struct {
    tel_state_t st;
    char num[24], cname[40], cnum[24];
} line_snap_t;

static line_snap_t s_snap[MAX_LINES];
static bool s_snap_valid;

static void publish_line(esp_mqtt_client_handle_t c, int l, bool force)
{
    line_snap_t cur = { .st = telephony_state(l) };
    strlcpy(cur.num, telephony_last_number(l), sizeof(cur.num));
    strlcpy(cur.cname, telephony_caller_name(l), sizeof(cur.cname));
    strlcpy(cur.cnum, telephony_caller_number(l), sizeof(cur.cnum));
    line_snap_t *old = &s_snap[l];
    char t[TOPIC_LEN];
    if (force || !s_snap_valid || cur.st != old->st) {
        topic(t, sizeof(t), "line/%d/state", l);
        pub(c, t, line_state_name(l), 0, 1);
    }
    if (force || !s_snap_valid || strcmp(cur.num, old->num) || strcmp(cur.cname, old->cname) ||
        strcmp(cur.cnum, old->cnum)) {
        char a[288], e1[48], e2[80], e3[48];
        jesc(cur.num, e1, sizeof(e1)); jesc(cur.cname, e2, sizeof(e2)); jesc(cur.cnum, e3, sizeof(e3));
        snprintf(a, sizeof(a), "{\"last_number\":\"%s\",\"caller_name\":\"%s\",\"caller_number\":\"%s\"}",
                 e1, e2, e3);
        topic(t, sizeof(t), "line/%d/attr", l);
        pub(c, t, a, 0, 1);
    }
    *old = cur;
}

static void publish_diag(esp_mqtt_client_handle_t c)
{
    char t[TOPIC_LEN], v[16];
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        snprintf(v, sizeof(v), "%d", ap.rssi);
        topic(t, sizeof(t), "diag/rssi");
        pub(c, t, v, 0, 0);
    }
    snprintf(v, sizeof(v), "%u", (unsigned)esp_get_free_heap_size());
    topic(t, sizeof(t), "diag/heap");
    pub(c, t, v, 0, 0);
}

static void tap(const char *event, int line, const char *detail)
{
    if (!s_connected || !s_lock) return;
    if (xSemaphoreTake(s_lock, 0) != pdTRUE) return;
    if (s_client) {
        char t[TOPIC_LEN], d[96], b[160];
        jesc(detail, d, sizeof(d));
        topic(t, sizeof(t), "event");
        snprintf(b, sizeof(b), "{\"event_type\":\"%s\",\"line\":%d,\"detail\":\"%s\"}", event, line, d);
        esp_mqtt_client_enqueue(s_client, t, b, 0, 0, 0, true);
    }
    xSemaphoreGive(s_lock);
}

static void task(void *arg)
{
    (void)arg;
    unsigned tick = 0;
    while (s_run) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (!s_run) break;
        if (!s_connected) continue;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_client) {
            int nlines = board_hw_config_get().num_lines;
            for (int l = 0; l < nlines && l < MAX_LINES; l++) publish_line(s_client, l, false);
            s_snap_valid = true;
            if (++tick % DIAG_EVERY_S == 0) publish_diag(s_client);
        }
        xSemaphoreGive(s_lock);
    }
    s_task = NULL;
    vTaskDelete(NULL);
}

/* ---- client events ----------------------------------------------------- */

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;
    esp_mqtt_event_handle_t ev = data;
    esp_mqtt_client_handle_t c = ev->client;
    switch ((esp_mqtt_event_id_t)id) {
        case MQTT_EVENT_CONNECTED: {
            char t[TOPIC_LEN];
            topic(t, sizeof(t), "availability");
            pub(c, t, "online", 1, 1);
            topic(t, sizeof(t), "cmd/#");
            esp_mqtt_client_subscribe(c, t, 1);
            discovery(c, board_hw_config_get().num_lines);
            s_snap_valid = false;
            for (int l = 0; l < board_hw_config_get().num_lines && l < MAX_LINES; l++) publish_line(c, l, true);
            s_snap_valid = true;
            publish_diag(c);
            s_connected = true;
            size_t now = esp_get_free_heap_size();
            if (s_measure) {
                s_heap_cost = s_heap_before > now ? (unsigned)(s_heap_before - now) : 0;
                s_measure = false;
            }
            ESP_LOGI(TAG, "connected to %s; heap taken since start: %u B (free %u, largest block %u)",
                     s_uri, s_heap_cost, (unsigned)now,
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
            activity_log("MQTT connected");
            break;
        }
        case MQTT_EVENT_DISCONNECTED:
            if (s_connected) activity_log("MQTT disconnected");
            s_connected = false;
            break;
        case MQTT_EVENT_DATA:
            on_data(ev);
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGW(TAG, "error type %d", ev->error_handle ? ev->error_handle->error_type : -1);
            break;
        default:
            break;
    }
}

/* ---- start / stop ------------------------------------------------------ */

static void stop(void)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_mqtt_client_handle_t c = s_client;
    bool was_up = s_connected;
    s_client = NULL;
    s_connected = false;
    xSemaphoreGive(s_lock);
    webhook_set_tap(NULL);

    s_run = false;
    for (int i = 0; i < 30 && s_task; i++) vTaskDelay(pdMS_TO_TICKS(100));
    if (c) {
        /* Tell Home Assistant the entities are unavailable, then close. The
         * discovery entries stay retained: turning MQTT back on is instant. */
        if (was_up) {
            char t[TOPIC_LEN];
            topic(t, sizeof(t), "availability");
            esp_mqtt_client_publish(c, t, "offline", 0, 1, 1);
        }
        esp_mqtt_client_stop(c);
        /* esp-mqtt's task sets its "stopped" flag, which is what stop() waits
         * for, and then writes client->state once more before it exits.
         * Destroying the client straight away frees memory that write still
         * lands in (a crash when the next client reuses it). Let the task
         * finish first. */
        vTaskDelay(pdMS_TO_TICKS(300));
        esp_mqtt_client_destroy(c);
    }
    s_heap_cost = 0;
}

static esp_err_t start(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    if (s_client) return ESP_OK;
    make_id();
    snprintf(s_uri, sizeof(s_uri), "mqtt://%s", s_cfg.host);

    char avail[TOPIC_LEN];
    topic(avail, sizeof(avail), "availability");
    s_heap_before = esp_get_free_heap_size();

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = s_uri,
        .credentials.client_id = s_clientid,
        .credentials.username = s_cfg.user[0] ? s_cfg.user : NULL,
        .credentials.authentication.password = s_cfg.pass[0] ? s_cfg.pass : NULL,
        .session.keepalive = 60,
        .session.last_will = { .topic = avail, .msg = "offline", .qos = 1, .retain = 1 },
        .network.reconnect_timeout_ms = 10000,
        .task.stack_size = 5120,
        .task.priority = 3,
    };
    esp_mqtt_client_handle_t c = esp_mqtt_client_init(&cfg);
    if (!c) return ESP_ERR_NO_MEM;
    esp_mqtt_client_register_event(c, ESP_EVENT_ANY_ID, on_event, NULL);

    s_run = true;
    if (xTaskCreate(task, "mqttha", TASK_STACK, NULL, 3, &s_task) != pdPASS) {
        s_run = false;
        esp_mqtt_client_destroy(c);
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_client = c;
    xSemaphoreGive(s_lock);
    webhook_set_tap(tap);
    esp_err_t err = esp_mqtt_client_start(c);
    if (err != ESP_OK) { stop(); return err; }
    return ESP_OK;
}

void mqtt_ha_init(void)
{
    load();
    /* At boot Wi-Fi and the web server haven't allocated yet, so the heap
     * delta would blame MQTT for them: leave the cost unmeasured. */
    s_measure = false;
    if (s_cfg.enabled && s_cfg.host[0]) start();
}

esp_err_t mqtt_ha_set(const mqtt_ha_cfg_t *in)
{
    load();
    mqtt_ha_cfg_t n = *in;
    if (n.enabled && !n.host[0]) return ESP_ERR_INVALID_ARG;
    for (const char *p = n.host; *p; p++)
        if (!(isalnum((unsigned char)*p) || *p == '.' || *p == '-' || *p == '_' || *p == ':'))
            return ESP_ERR_INVALID_ARG;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_set_u8(h, "en", n.enabled ? 1 : 0);
    put_str(h, "host", n.host);
    put_str(h, "user", n.user);
    put_str(h, "pass", n.pass);
    err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) return err;

    stop();
    s_cfg = n;
    s_measure = true;
    return n.enabled ? start() : ESP_OK;
}

mqtt_ha_state_t mqtt_ha_state(void)
{
    if (!s_client) return MQTT_HA_OFF;
    return s_connected ? MQTT_HA_CONNECTED : MQTT_HA_CONNECTING;
}

unsigned mqtt_ha_heap_cost(void) { return s_heap_cost; }

#include "payphone.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "payphone";

#define NVS_NS      "payphone"
#define KEY_CFG     "cfg"
#define KEY_TOTALS  "totals"
#define CFG_VERSION 1

/* What's stored: the settings plus a version, so a later layout can tell. */
typedef struct {
    uint8_t        version;
    payphone_cfg_t cfg;
} stored_cfg_t;

static const payphone_cfg_t DEFAULTS = {
    .enabled = false,
    .tone_detect = true,
    .coin_required = false,
    .codes_free = true,
    .coin_msg = false,
    .price_cents = 10,
    .repeat_s = 8,
};

static payphone_cfg_t     s_cfg;
static payphone_totals_t  s_totals;
static payphone_log_t     s_log[PAYPHONE_LOG_N];
static size_t             s_log_head;   /* next slot to write */
static size_t             s_log_n;
static SemaphoreHandle_t  s_lock;

static void lock(void)   { if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { if (s_lock) xSemaphoreGive(s_lock); }

static void save_blob(const char *key, const void *data, size_t len)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_blob(h, key, data, len) == ESP_OK) nvs_commit(h);
    nvs_close(h);
}

void payphone_init(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    s_cfg = DEFAULTS;
    memset(&s_totals, 0, sizeof(s_totals));
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        stored_cfg_t st;
        size_t len = sizeof(st);
        if (nvs_get_blob(h, KEY_CFG, &st, &len) == ESP_OK && len == sizeof(st) &&
            st.version == CFG_VERSION) {
            s_cfg = st.cfg;
        }
        len = sizeof(s_totals);
        if (nvs_get_blob(h, KEY_TOTALS, &s_totals, &len) != ESP_OK || len != sizeof(s_totals))
            memset(&s_totals, 0, sizeof(s_totals));
        nvs_close(h);
    }
    for (int i = 0; i < PAYPHONE_COINS; i++) s_cfg.coin_clip[i][PAYPHONE_CLIP_MAX - 1] = '\0';
    ESP_LOGI(TAG, "payphone features %s", s_cfg.enabled ? "on" : "off");
}

payphone_cfg_t payphone_cfg(void)
{
    lock();
    payphone_cfg_t c = s_cfg;
    unlock();
    return c;
}

esp_err_t payphone_cfg_set(const payphone_cfg_t *cfg)
{
    stored_cfg_t st = { .version = CFG_VERSION, .cfg = *cfg };
    if (st.cfg.price_cents < 1) st.cfg.price_cents = 1;
    if (st.cfg.price_cents > 500) st.cfg.price_cents = 500;
    if (st.cfg.repeat_s < 2) st.cfg.repeat_s = 2;
    if (st.cfg.repeat_s > 60) st.cfg.repeat_s = 60;
    for (int i = 0; i < PAYPHONE_COINS; i++) st.cfg.coin_clip[i][PAYPHONE_CLIP_MAX - 1] = '\0';
    lock();
    s_cfg = st.cfg;
    unlock();
    save_blob(KEY_CFG, &st, sizeof(st));
    return ESP_OK;
}

bool payphone_enabled(void)  { return s_cfg.enabled; }
bool payphone_tones_on(void) { return s_cfg.enabled && s_cfg.tone_detect; }

int payphone_coin_index(int cents)
{
    switch (cents) {
        case 5:  return 0;
        case 10: return 1;
        case 25: return 2;
        default: return -1;
    }
}

const char *payphone_src_name(payphone_src_t src)
{
    switch (src) {
        case PAYPHONE_SRC_TONE:  return "coin tones";
        case PAYPHONE_SRC_SIM:   return "simulated";
        case PAYPHONE_SRC_BOARD: return "coin board";
        default:                 return "?";
    }
}

static void log_add(int line, const char *msg)
{
    lock();
    payphone_log_t *e = &s_log[s_log_head];
    e->when = time(NULL);
    e->up_ms = esp_timer_get_time() / 1000;
    e->line = (int8_t)line;
    strncpy(e->msg, msg, sizeof(e->msg) - 1);
    e->msg[sizeof(e->msg) - 1] = '\0';
    s_log_head = (s_log_head + 1) % PAYPHONE_LOG_N;
    if (s_log_n < PAYPHONE_LOG_N) s_log_n++;
    unlock();
}

void payphone_logf(int line, const char *fmt, ...)
{
    char msg[PAYPHONE_MSG_LEN];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    ESP_LOGI(TAG, "line %d: %s", line, msg);
    log_add(line, msg);
}

void payphone_coin_seen(int line, int cents, payphone_src_t src, int credit_after)
{
    int i = payphone_coin_index(cents);
    lock();
    if (i >= 0) s_totals.count[i]++;
    s_totals.cents += (uint32_t)cents;
    payphone_totals_t t = s_totals;
    unlock();
    save_blob(KEY_TOTALS, &t, sizeof(t));
    payphone_logf(line, "%d c coin (%s), credit %d c", cents, payphone_src_name(src), credit_after);
}

size_t payphone_log_list(payphone_log_t *out, size_t max)
{
    lock();
    size_t n = s_log_n < max ? s_log_n : max;
    for (size_t k = 0; k < n; k++) {
        size_t idx = (s_log_head + PAYPHONE_LOG_N - 1 - k) % PAYPHONE_LOG_N;
        out[k] = s_log[idx];
    }
    unlock();
    return n;
}

void payphone_log_clear(void)
{
    lock();
    s_log_head = 0;
    s_log_n = 0;
    unlock();
}

payphone_totals_t payphone_totals(void)
{
    lock();
    payphone_totals_t t = s_totals;
    unlock();
    return t;
}

void payphone_totals_reset(void)
{
    lock();
    memset(&s_totals, 0, sizeof(s_totals));
    payphone_totals_t t = s_totals;
    unlock();
    save_blob(KEY_TOTALS, &t, sizeof(t));
    payphone_logf(-1, "coin box counter reset");
}

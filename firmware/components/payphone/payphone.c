#include "payphone.h"
#include "payphone_types.h"
#include "green_box.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdarg.h>
#include <stddef.h>
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
/* New fields are only ever appended, so an older, shorter blob loads by copying its bytes over the
 * defaults (payphone_init). Blobs saved so far: 2 + 106 bytes (before the unanswered / limit fields),
 * 2 + 110 (before the phone type). */
_Static_assert(sizeof(payphone_cfg_t) == 112, "payphone_cfg_t changed size: append fields only, and check payphone_init");
#define CFG_OLDEST_SIZE 106

static const payphone_cfg_t DEFAULTS = {
    .enabled = false,
    .tone_detect = true,
    .coin_required = false,
    .codes_free = true,
    .coin_msg = false,
    .price_cents = 10,
    .repeat_s = 8,
    .coin_clip = { PAYPHONE_NICKEL_CLIP, PAYPHONE_DIME_CLIP, PAYPHONE_QUARTER_CLIP },
    .coin_limit = 8,
    .relay_method = PAYPHONE_RELAY_BOARD,   /* as before: the coin board fires the relay */
};

static payphone_cfg_t     s_cfg;
static payphone_totals_t  s_totals;
static payphone_log_t     s_log[PAYPHONE_LOG_N];
static size_t             s_log_head;   /* next slot to write */
static size_t             s_log_n;
static SemaphoreHandle_t  s_lock;
static payphone_relay_fn  s_relay;
static volatile int       s_board_line = -1;
/* Dial codes that need no coin: ",314,0,..." (commas around every code). */
#define KEY_FREE   "free"
#define FREE_MAX   200
static char               s_free[FREE_MAX] = ",";

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
        /* A version byte, a pad byte, then the config. An older, shorter config keeps the
         * defaults for the fields added since (its own fields sit at the same offsets). */
        uint8_t raw[sizeof(stored_cfg_t)];
        size_t len = sizeof(raw);
        if (nvs_get_blob(h, KEY_CFG, raw, &len) == ESP_OK && raw[0] == CFG_VERSION &&
            len >= offsetof(stored_cfg_t, cfg) + CFG_OLDEST_SIZE && len <= sizeof(raw))
            memcpy(&s_cfg, raw + offsetof(stored_cfg_t, cfg), len - offsetof(stored_cfg_t, cfg));
        len = sizeof(s_totals);
        if (nvs_get_blob(h, KEY_TOTALS, &s_totals, &len) != ESP_OK || len != sizeof(s_totals))
            memset(&s_totals, 0, sizeof(s_totals));
        size_t fl = sizeof(s_free);
        if (nvs_get_str(h, KEY_FREE, s_free, &fl) != ESP_OK || s_free[0] != ',') strcpy(s_free, ",");
        nvs_close(h);
    }
    for (int i = 0; i < PAYPHONE_COINS; i++) s_cfg.coin_clip[i][PAYPHONE_CLIP_MAX - 1] = '\0';
    if (s_cfg.coin_mode > PAYPHONE_COINS_IMMEDIATE) s_cfg.coin_mode = PAYPHONE_COINS_ON_ANSWER;
    if (s_cfg.unanswered_collect > 1) s_cfg.unanswered_collect = 0;
    if (s_cfg.limit_collect > 1) s_cfg.limit_collect = 0;
    if (s_cfg.coin_limit > PAYPHONE_COIN_LIMIT_MAX) s_cfg.coin_limit = DEFAULTS.coin_limit;
    /* The byte the phone type now uses was padding in the 110-byte layout: anything odd is "not chosen". */
    if (s_cfg.housing >= PP_HOUSING_N || s_cfg.inside >= PP_INSIDE_N) s_cfg.housing = s_cfg.inside = 0;
    if (s_cfg.relay_method > PAYPHONE_RELAY_BOTH) s_cfg.relay_method = DEFAULTS.relay_method;
    green_box_init();
    ESP_LOGI(TAG, "payphone features %s, relay method %d", s_cfg.enabled ? "on" : "off",
             s_cfg.relay_method);
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
    if (st.cfg.coin_mode > PAYPHONE_COINS_IMMEDIATE) st.cfg.coin_mode = PAYPHONE_COINS_ON_ANSWER;
    if (st.cfg.unanswered_collect > 1) st.cfg.unanswered_collect = 0;
    if (st.cfg.limit_collect > 1) st.cfg.limit_collect = 0;
    if (st.cfg.coin_limit > PAYPHONE_COIN_LIMIT_MAX) st.cfg.coin_limit = PAYPHONE_COIN_LIMIT_MAX;
    if (st.cfg.housing >= PP_HOUSING_N || st.cfg.inside >= PP_INSIDE_N) st.cfg.housing = st.cfg.inside = 0;
    lock();
    s_cfg = st.cfg;
    unlock();
    save_blob(KEY_CFG, &st, sizeof(st));
    return ESP_OK;
}

bool payphone_code_listed(const char *code)
{
    if (!code || !code[0] || strlen(code) > 16) return false;
    char key[24];
    snprintf(key, sizeof(key), ",%s,", code);
    lock();
    bool r = strstr(s_free, key) != NULL;
    unlock();
    return r;
}

bool payphone_code_free(const char *code) { return s_cfg.codes_free || payphone_code_listed(code); }

esp_err_t payphone_code_free_set(const char *code, bool is_free)
{
    if (!code || !code[0] || strlen(code) > 16) return ESP_ERR_INVALID_ARG;
    for (const char *c = code; *c; c++) if (*c != '*' && *c != '#' && (*c < '0' || *c > '9')) return ESP_ERR_INVALID_ARG;
    char key[24];
    snprintf(key, sizeof(key), ",%s,", code);
    lock();
    char *at = strstr(s_free, key);
    if (is_free && !at) {
        if (strlen(s_free) + strlen(code) + 2 > FREE_MAX) { unlock(); return ESP_ERR_NO_MEM; }
        strcat(s_free, code);
        strcat(s_free, ",");
    } else if (!is_free && at) {
        memmove(at, at + strlen(key) - 1, strlen(at + strlen(key) - 1) + 1);
    }
    char copy[FREE_MAX];
    strcpy(copy, s_free);
    unlock();
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return ESP_FAIL;
    nvs_set_str(h, KEY_FREE, copy);
    nvs_commit(h);
    nvs_close(h);
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

void payphone_set_relay_handler(payphone_relay_fn fn) { s_relay = fn; }

void payphone_relay(int line, payphone_relay_t what, int cents, const char *why)
{
    uint8_t method = s_cfg.relay_method;
    payphone_relay_fn fn = s_relay;
    bool board = fn && line == s_board_line;
    bool use_tones = (method == PAYPHONE_RELAY_TONES || method == PAYPHONE_RELAY_BOTH);
    bool use_board = (method == PAYPHONE_RELAY_BOARD || method == PAYPHONE_RELAY_BOTH) && board;

    const char *how = use_tones && use_board ? " (tones + board)" :
                      use_tones ? " (green box tones)" :
                      use_board ? " (coin board)" : " (no relay method set)";
    payphone_logf(line, "%d c %s: %s%s", cents, what == PAYPHONE_COLLECT ? "collected" : "returned",
                  why, how);

    if (use_tones) green_box_fire(line, what);
    if (use_board) fn(line, what);
}

void payphone_set_board_line(int line) { s_board_line = line; }
int  payphone_board_line(void) { return s_board_line; }

bool payphone_tones_on_line(int line)
{
    return payphone_tones_on() && line != s_board_line;
}

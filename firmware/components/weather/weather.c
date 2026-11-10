/* weather.c -- see weather.h. */
#include "weather.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "dialplan.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include "nvs.h"
#include "storage.h"
#include "wifi.h"

static const char *TAG = "weather";

#define NVS_NS           "weather"
#define CFG_VERSION      1
#define REFRESH_US       (60LL * 60 * 1000000)    /* fetch the forecast this often */
#define RETRY_US         (5LL * 60 * 1000000)     /* after a failed fetch */
#define STALE_US         (3LL * 3600 * 1000000)   /* older than this isn't read out */
#define TICK_US          (60LL * 1000000)
#define MIN_FREE_HEAP    (48 * 1024)              /* a TLS connection needs about 40 KB */
#define MIN_BLOCK        (20 * 1024)
#define JSON_MAX         4096
#define INDEX_MAX        8192
#define CHUNK            2048
#define INTERNAL_MIN_CAP (4u * 1024 * 1024)       /* below this (8 MB boards) the pack goes on a card */
#define PACK_MIN         (100u * 1024)
#define PACK_MAX         (4u * 1024 * 1024)

/* Pauses after each piece, so the joins sound like speech. */
#define GAP_WORD     30
#define GAP_COMMA   150
#define GAP_SENTENCE 450

typedef struct {
    uint8_t version;
    weather_cfg_t cfg;
} saved_cfg_t;

typedef struct {
    bool    valid;
    bool    celsius;
    int     cur_temp, cur_code, hour;
    struct { int code, hi, lo, pop; } day[2];
    time_t  fetched;
    int64_t fetched_us;
} forecast_t;

static portMUX_TYPE  s_lock = portMUX_INITIALIZER_UNLOCKED;
static weather_cfg_t s_cfg = { .parts = WEATHER_PART_NOW | WEATHER_PART_TODAY | WEATHER_PART_TOMORROW };
static forecast_t    s_fc;
static weather_status_t s_st;
static volatile bool s_task_running, s_want_install, s_want_fetch;
static char          s_install_base[160];
static int64_t       s_last_try_us;

/* ---- settings ------------------------------------------------------------ */

static void cfg_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    saved_cfg_t sc;
    size_t len = sizeof(sc);
    if (nvs_get_blob(h, "cfg", &sc, &len) == ESP_OK && len == sizeof(sc) && sc.version == CFG_VERSION) {
        sc.cfg.place[sizeof(sc.cfg.place) - 1] = '\0';
        s_cfg = sc.cfg;
    }
    uint8_t sd = 0;
    if (nvs_get_u8(h, "pack_sd", &sd) == ESP_OK) s_st.pack_on_sd = sd;
    nvs_close(h);
}

static bool have_place(const weather_cfg_t *c) { return c->lat != 0.0 || c->lon != 0.0; }

static void update_dialplan(void)
{
    dialplan_set_weather(s_cfg.enabled && s_st.pack == WEATHER_PACK_READY);
}

weather_cfg_t weather_cfg(void)
{
    taskENTER_CRITICAL(&s_lock);
    weather_cfg_t c = s_cfg;
    taskEXIT_CRITICAL(&s_lock);
    return c;
}

esp_err_t weather_set_cfg(const weather_cfg_t *c)
{
    weather_cfg_t n = *c;
    n.place[sizeof(n.place) - 1] = '\0';
    if (n.lat < -90 || n.lat > 90 || n.lon < -180 || n.lon > 180) return ESP_ERR_INVALID_ARG;
    if (!(n.parts & (WEATHER_PART_NOW | WEATHER_PART_TODAY | WEATHER_PART_TOMORROW))) n.parts = WEATHER_PART_TODAY;
    weather_cfg_t old = weather_cfg();
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    saved_cfg_t sc = { .version = CFG_VERSION, .cfg = n };
    err = nvs_set_blob(h, "cfg", &sc, sizeof(sc));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) return err;
    taskENTER_CRITICAL(&s_lock);
    s_cfg = n;
    bool refetch = n.lat != old.lat || n.lon != old.lon || n.celsius != old.celsius || (n.enabled && !old.enabled);
    if (refetch) s_fc.valid = false;
    taskEXIT_CRITICAL(&s_lock);
    update_dialplan();
    if (refetch && n.enabled) weather_refresh();
    return ESP_OK;
}

weather_status_t weather_status(void)
{
    taskENTER_CRITICAL(&s_lock);
    weather_status_t st = s_st;
    st.have_forecast = s_fc.valid;
    st.fetched = s_fc.valid ? s_fc.fetched : 0;
    taskEXIT_CRITICAL(&s_lock);
    return st;
}

static void set_msg(char *dst, size_t cap, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void set_msg(char *dst, size_t cap, const char *fmt, ...)
{
    char tmp[112];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    taskENTER_CRITICAL(&s_lock);
    snprintf(dst, cap, "%s", tmp);
    taskEXIT_CRITICAL(&s_lock);
}

/* ---- the voice pack ------------------------------------------------------ */

static uint32_t rd_u32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* The pack's "wxix" chunk as a NUL-terminated string (caller frees), or NULL. */
static char *read_index(void)
{
    storage_reader_t rd = { 0 };
    if (storage_read_open(WEATHER_PACK_NAME, &rd) != ESP_OK) return NULL;
    char *idx = NULL;
    uint8_t h[12];
    if (storage_read_chunk(&rd, h, 12) == 12 && !memcmp(h, "RIFF", 4) && !memcmp(h + 8, "WAVE", 4)) {
        size_t pos = 12;
        for (int guard = 0; guard < 8; guard++) {
            if (storage_read_chunk(&rd, h, 8) != 8) break;
            uint32_t sz = rd_u32(h + 4);
            pos += 8;
            if (!memcmp(h, "data", 4)) break;
            if (!memcmp(h, "wxix", 4)) {
                if (sz < 8 || sz > INDEX_MAX) break;
                idx = malloc(sz + 2);
                if (idx && storage_read_chunk(&rd, idx + 1, sz) == sz) {
                    idx[0] = '\n';          /* so every entry, the first too, follows a newline */
                    idx[sz + 1] = '\0';
                    if (strncmp(idx + 1, "AR-WX ", 6) != 0) { free(idx); idx = NULL; }
                } else {
                    free(idx);
                    idx = NULL;
                }
                break;
            }
            pos += sz + (sz & 1);
            if (storage_read_seek(&rd, pos) != ESP_OK) break;
        }
    }
    storage_read_close(&rd);
    return idx;
}

static bool index_lookup(const char *idx, const char *name, uint32_t *off, uint32_t *len)
{
    char key[24];
    snprintf(key, sizeof(key), "\n%s ", name);
    const char *p = strstr(idx, key);
    if (!p) return false;
    char *end;
    p += strlen(key);
    *off = (uint32_t)strtoul(p, &end, 10);
    if (end == p) return false;
    *len = (uint32_t)strtoul(end, NULL, 10);
    return *len > 0;
}

static void pack_check(void)
{
    char *idx = read_index();
    uint32_t size = 0;
    if (idx) {
        storage_reader_t rd = { 0 };
        if (storage_read_open(WEATHER_PACK_NAME, &rd) == ESP_OK) {
            size = (uint32_t)storage_read_size(&rd);
            storage_read_close(&rd);
        }
    }
    taskENTER_CRITICAL(&s_lock);
    if (s_st.pack != WEATHER_PACK_INSTALLING) {
        s_st.pack = idx ? WEATHER_PACK_READY : WEATHER_PACK_MISSING;
        s_st.pack_bytes = size;
    }
    taskEXIT_CRITICAL(&s_lock);
    free(idx);
    update_dialplan();
}

const char *weather_pack_blocker(void)
{
    if (storage_cap_bytes_internal() >= INTERNAL_MIN_CAP) return NULL;
    if (storage_sd_present()) return NULL;
    return "This board's internal storage is too small for the voice pack: insert a microSD card "
           "(set up on the Hardware tab).";
}

static esp_http_client_handle_t open_url(const char *url, int *status)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 20000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 1024,
        .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return NULL;
    if (esp_http_client_open(c, 0) != ESP_OK) { esp_http_client_cleanup(c); return NULL; }
    esp_http_client_fetch_headers(c);
    *status = esp_http_client_get_status_code(c);
    return c;
}

static void close_url(esp_http_client_handle_t c)
{
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
}

/* GETs a small body into a malloc'd NUL-terminated buffer (caller frees). */
static char *get_small(const char *url, int *status)
{
    *status = 0;
    esp_http_client_handle_t c = open_url(url, status);
    if (!c) return NULL;
    char *buf = malloc(JSON_MAX);
    int got = 0, n;
    if (buf && *status == 200) {
        while (got < JSON_MAX - 1 && (n = esp_http_client_read(c, buf + got, JSON_MAX - 1 - got)) > 0) got += n;
        buf[got] = '\0';
    }
    close_url(c);
    if (!buf || *status != 200 || got == 0) { free(buf); return NULL; }
    return buf;
}

static void remove_all_copies(void)
{
    for (int i = 0; i < 3 && storage_delete(WEATHER_PACK_NAME) == ESP_OK; i++) { }
}

/* Returns an error text, or NULL once the pack is installed. */
static const char *install(const char *base)
{
    const char *why = weather_pack_blocker();
    if (why) return why;
    bool to_sd = storage_cap_bytes_internal() < INTERNAL_MIN_CAP;

    char url[200];
    snprintf(url, sizeof(url), "%sweather_en.json", base);
    int status;
    char *mj = get_small(url, &status);
    if (!mj) {
        static char e[64];
        snprintf(e, sizeof(e), "couldn't get the pack's description (HTTP %d)", status);
        return e;
    }
    cJSON *j = cJSON_Parse(mj);
    free(mj);
    cJSON *jsize = cJSON_GetObjectItem(j, "size"), *jsha = cJSON_GetObjectItem(j, "sha256");
    uint32_t size = cJSON_IsNumber(jsize) ? (uint32_t)jsize->valuedouble : 0;
    char want_sha[65] = "";
    if (cJSON_IsString(jsha) && strlen(jsha->valuestring) == 64) snprintf(want_sha, sizeof(want_sha), "%s", jsha->valuestring);
    cJSON_Delete(j);
    if (size < PACK_MIN || size > PACK_MAX || !want_sha[0]) return "the pack's description looks wrong";

    if (!to_sd) {
        /* Never make room by evicting recordings: say so instead. */
        size_t mine = 0;
        storage_reader_t rd = { 0 };
        if (!s_st.pack_on_sd && storage_read_open(WEATHER_PACK_NAME, &rd) == ESP_OK) {
            mine = storage_read_size(&rd);
            storage_read_close(&rd);
        }
        if (storage_free_bytes_internal() + mine < size + 128 * 1024)
            return "not enough free space in internal storage: delete some recordings first";
    }

    taskENTER_CRITICAL(&s_lock);
    s_st.pack = WEATHER_PACK_INSTALLING;
    s_st.pack_bytes = size;
    s_st.pack_done = 0;
    taskEXIT_CRITICAL(&s_lock);
    dialplan_set_weather(false);
    remove_all_copies();

    storage_writer_t w;
    esp_err_t werr = to_sd ? storage_write_begin(WEATHER_PACK_NAME, size, &w)
                           : storage_write_begin_local(WEATHER_PACK_NAME, size, &w);
    if (werr != ESP_OK) return "couldn't start writing the pack to storage";

    snprintf(url, sizeof(url), "%sweather_en.pak", base);
    esp_http_client_handle_t c = open_url(url, &status);
    if (!c || status != 200) {
        if (c) close_url(c);
        storage_write_abort(&w);
        static char e[64];
        snprintf(e, sizeof(e), "couldn't download the pack (HTTP %d)", status);
        return e;
    }
    uint8_t *buf = malloc(CHUNK);
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    uint32_t total = 0;
    const char *err = buf ? NULL : "out of memory";
    int quiet = 0;
    while (!err && total < size) {
        int n = esp_http_client_read(c, (char *)buf, (int)(size - total < CHUNK ? size - total : CHUNK));
        if (n > 0) {
            quiet = 0;
            mbedtls_sha256_update(&sha, buf, (size_t)n);
            if (storage_write_chunk(&w, buf, (size_t)n) != ESP_OK) { err = "writing to storage failed"; break; }
            total += (uint32_t)n;
            taskENTER_CRITICAL(&s_lock);
            s_st.pack_done = total;
            taskEXIT_CRITICAL(&s_lock);
        } else if (esp_http_client_is_complete_data_received(c) || ++quiet > 6) {
            break;
        }
    }
    close_url(c);
    free(buf);
    uint8_t digest[32];
    mbedtls_sha256_finish(&sha, digest);
    mbedtls_sha256_free(&sha);
    if (!err && total != size) err = "the download stopped early: try again";
    if (err) { storage_write_abort(&w); remove_all_copies(); return err; }
    if (storage_write_end(&w) != ESP_OK) { remove_all_copies(); return "saving the pack failed"; }
    char got_sha[65];
    for (int i = 0; i < 32; i++) sprintf(got_sha + i * 2, "%02x", digest[i]);
    if (strcmp(got_sha, want_sha) != 0) { remove_all_copies(); return "the download was damaged (checksum): try again"; }

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "pack_sd", to_sd);
        nvs_commit(h);
        nvs_close(h);
    }
    taskENTER_CRITICAL(&s_lock);
    s_st.pack_on_sd = to_sd;
    taskEXIT_CRITICAL(&s_lock);
    return NULL;
}

esp_err_t weather_remove_pack(void)
{
    if (s_st.pack == WEATHER_PACK_INSTALLING) return ESP_ERR_INVALID_STATE;
    remove_all_copies();
    pack_check();
    return ESP_OK;
}

/* ---- the forecast -------------------------------------------------------- */

static int arr_int(cJSON *obj, const char *key, int i, int dflt)
{
    cJSON *a = cJSON_GetObjectItem(obj, key);
    cJSON *v = cJSON_IsArray(a) ? cJSON_GetArrayItem(a, i) : NULL;
    return cJSON_IsNumber(v) ? (int)lround(v->valuedouble) : dflt;
}

/* Returns an error text, or NULL with the new forecast in s_fc. */
static const char *fetch(void)
{
    weather_cfg_t c = weather_cfg();
    if (!have_place(&c)) return "no place picked yet";
    char url[360];
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
             "&current=temperature_2m,weather_code"
             "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max"
             "&temperature_unit=%s&timezone=auto&forecast_days=2",
             c.lat, c.lon, c.celsius ? "celsius" : "fahrenheit");
    int status;
    char *body = get_small(url, &status);
    if (!body) {
        static char e[64];
        snprintf(e, sizeof(e), status ? "Open-Meteo answered HTTP %d" : "couldn't reach Open-Meteo", status);
        return e;
    }
    cJSON *j = cJSON_Parse(body);
    free(body);
    cJSON *cur = cJSON_GetObjectItem(j, "current"), *daily = cJSON_GetObjectItem(j, "daily");
    cJSON *ct = cJSON_GetObjectItem(cur, "temperature_2m"), *cc = cJSON_GetObjectItem(cur, "weather_code");
    cJSON *ctime = cJSON_GetObjectItem(cur, "time");
    forecast_t f = { 0 };
    if (cJSON_IsNumber(ct) && cJSON_IsNumber(cc) && cJSON_IsObject(daily) &&
        arr_int(daily, "temperature_2m_max", 1, -999) != -999) {
        f.valid = true;
        f.celsius = c.celsius;
        f.cur_temp = (int)lround(ct->valuedouble);
        f.cur_code = cc->valueint;
        f.hour = (cJSON_IsString(ctime) && strlen(ctime->valuestring) >= 13) ? atoi(ctime->valuestring + 11) : 12;
        for (int d = 0; d < 2; d++) {
            f.day[d].code = arr_int(daily, "weather_code", d, 0);
            f.day[d].hi   = arr_int(daily, "temperature_2m_max", d, 0);
            f.day[d].lo   = arr_int(daily, "temperature_2m_min", d, 0);
            f.day[d].pop  = arr_int(daily, "precipitation_probability_max", d, 0);
        }
        f.fetched = time(NULL);
        f.fetched_us = esp_timer_get_time();
    }
    cJSON_Delete(j);
    if (!f.valid) return "Open-Meteo's answer was missing something";
    taskENTER_CRITICAL(&s_lock);
    s_fc = f;
    taskEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "forecast: now %d (code %d), today %d/%d %d%%, tomorrow %d/%d %d%%", f.cur_temp, f.cur_code,
             f.day[0].hi, f.day[0].lo, f.day[0].pop, f.day[1].hi, f.day[1].lo, f.day[1].pop);
    return NULL;
}

/* ---- the background worker ----------------------------------------------- */

static bool kick(void);

static bool heap_ok(void)
{
    return heap_caps_get_free_size(MALLOC_CAP_8BIT) >= MIN_FREE_HEAP &&
           heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) >= MIN_BLOCK;
}

static void worker(void *arg)
{
    (void)arg;
    if (s_want_install) {
        s_want_install = false;
        const char *err = heap_ok() ? install(s_install_base) : "the board is short of memory right now: try again";
        taskENTER_CRITICAL(&s_lock);
        if (s_st.pack == WEATHER_PACK_INSTALLING) s_st.pack = WEATHER_PACK_MISSING;
        taskEXIT_CRITICAL(&s_lock);
        if (err) {
            ESP_LOGW(TAG, "voice pack: %s", err);
            set_msg(s_st.pack_msg, sizeof(s_st.pack_msg), "Failed: %s", err);
        } else {
            set_msg(s_st.pack_msg, sizeof(s_st.pack_msg), "Installed");
        }
        pack_check();
        if (!err && s_cfg.enabled) s_want_fetch = true;
    }
    if (s_want_fetch) {
        s_want_fetch = false;
        s_last_try_us = esp_timer_get_time();
        const char *err = heap_ok() ? fetch() : "short of memory, will retry";
        if (err) ESP_LOGW(TAG, "forecast: %s", err);
        set_msg(s_st.fetch_msg, sizeof(s_st.fetch_msg), "%s", err ? err : "");
    }
    s_task_running = false;
    if (s_want_install || s_want_fetch) kick();   /* asked for while this run was busy */
    vTaskDelete(NULL);
}

/* Starts the worker unless it's running (it picks up any new request before it ends). */
static bool kick(void)
{
    if (s_task_running) return true;
    s_task_running = true;
    if (xTaskCreate(worker, "weather", 6144, NULL, tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
        s_task_running = false;
        ESP_LOGW(TAG, "couldn't start the worker task");
        return false;
    }
    return true;
}

static void tick(void *arg)
{
    (void)arg;
    /* A pack copied on by hand, or on a card inserted since: notice it. */
    if (s_st.pack == WEATHER_PACK_MISSING && !s_task_running) pack_check();
    weather_cfg_t c = weather_cfg();
    if (!c.enabled || !have_place(&c) || !wifi_is_connected()) return;
    int64_t now = esp_timer_get_time();
    taskENTER_CRITICAL(&s_lock);
    bool due = !s_fc.valid || now - s_fc.fetched_us >= REFRESH_US;
    taskEXIT_CRITICAL(&s_lock);
    if (due && (s_last_try_us == 0 || now - s_last_try_us >= RETRY_US)) {
        s_want_fetch = true;
        kick();
    }
}

void weather_refresh(void)
{
    s_last_try_us = 0;
    s_want_fetch = true;
    kick();
}

esp_err_t weather_install(const char *base)
{
    if (s_st.pack == WEATHER_PACK_INSTALLING || s_want_install) return ESP_ERR_INVALID_STATE;
    snprintf(s_install_base, sizeof(s_install_base), "%s", base ? base : WEATHER_PACK_BASE);
    set_msg(s_st.pack_msg, sizeof(s_st.pack_msg), "Downloading...");
    taskENTER_CRITICAL(&s_lock);
    s_st.pack = WEATHER_PACK_INSTALLING;   /* shown at once; install() sets the size */
    s_st.pack_done = 0;
    taskEXIT_CRITICAL(&s_lock);
    s_want_install = true;
    if (!kick()) {
        s_want_install = false;
        taskENTER_CRITICAL(&s_lock);
        s_st.pack = WEATHER_PACK_MISSING;
        taskEXIT_CRITICAL(&s_lock);
        set_msg(s_st.pack_msg, sizeof(s_st.pack_msg), "Failed: the board is short of memory right now: try again");
        pack_check();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void weather_init(void)
{
    cfg_load();
    pack_check();
    const esp_timer_create_args_t ta = { .callback = tick, .name = "weather" };
    esp_timer_handle_t t;
    if (esp_timer_create(&ta, &t) == ESP_OK) esp_timer_start_periodic(t, TICK_US);
}

/* ---- the report ---------------------------------------------------------- */

#define PLAN_MAX 48

typedef struct {
    char     name[PLAN_MAX][12];
    uint16_t gap[PLAN_MAX];
    int      n;
} plan_t;

static void add(plan_t *p, const char *name, uint16_t gap)
{
    if (p->n >= PLAN_MAX) return;
    snprintf(p->name[p->n], sizeof(p->name[0]), "%s", name);
    p->gap[p->n++] = gap;
}

static void add_num(plan_t *p, int v, uint16_t gap)
{
    if (v < 0) { add(p, "minus", GAP_WORD); v = -v; }
    if (v > 110) v = 110;
    char n[8];
    snprintf(n, sizeof(n), "n%d", v);
    add(p, n, gap);
}

/* Open-Meteo's WMO weather code -> the pack's phrase for it. */
static const char *cond(int code)
{
    switch (code) {
        case 0:  return "c_clear";
        case 1:  return "c_mostly_clear";
        case 2:  return "c_partly_cloudy";
        case 3:  return "c_cloudy";
        case 45: case 48: return "c_fog";
        case 51: case 53: case 55: return "c_drizzle";
        case 56: case 57: return "c_freezing_drizzle";
        case 61: return "c_light_rain";
        case 63: return "c_rain";
        case 65: return "c_heavy_rain";
        case 66: case 67: return "c_freezing_rain";
        case 71: case 77: return "c_light_snow";
        case 73: return "c_snow";
        case 75: return "c_heavy_snow";
        case 80: case 81: return "c_showers";
        case 82: return "c_heavy_showers";
        case 85: case 86: return "c_snow_showers";
        case 95: return "c_thunder";
        case 96: case 99: return "c_thunder_hail";
        default: return code < 3 ? "c_clear" : "c_cloudy";
    }
}

static bool snowy(int code) { return (code >= 71 && code <= 77) || code == 85 || code == 86; }

static void add_chance(plan_t *p, int code, int pop)
{
    if (pop < 20) return;
    add_num(p, (pop + 5) / 10 * 10, GAP_WORD);
    add(p, snowy(code) ? "pct_snow" : "pct_rain", GAP_SENTENCE);
}

static void add_day(plan_t *p, const char *label, int code, int hi, int lo, int pop)
{
    add(p, label, GAP_COMMA);
    add(p, cond(code), GAP_COMMA);
    add(p, "high", GAP_WORD);
    add_num(p, hi, GAP_WORD);
    add(p, "low", GAP_WORD);
    add_num(p, lo, GAP_SENTENCE);
    add_chance(p, code, pop);
}

static void build(plan_t *p, const forecast_t *f, uint8_t parts)
{
    p->n = 0;
    add(p, "intro", GAP_SENTENCE);
    if (parts & WEATHER_PART_NOW) {
        add(p, "now", GAP_COMMA);
        add(p, cond(f->cur_code), GAP_COMMA);
        add(p, "and", GAP_WORD);
        add_num(p, f->cur_temp, GAP_WORD);
        add(p, "degrees", GAP_SENTENCE);
    }
    if (parts & WEATHER_PART_TODAY) {
        if (f->hour < 17) {
            add_day(p, "today", f->day[0].code, f->day[0].hi, f->day[0].lo, f->day[0].pop);
        } else {
            /* Evening: today's high is past; tomorrow's low is the coming night's. */
            add(p, "tonight", GAP_COMMA);
            add(p, "low_only", GAP_WORD);
            add_num(p, f->day[1].lo, GAP_SENTENCE);
        }
    }
    if (parts & WEATHER_PART_TOMORROW)
        add_day(p, "tomorrow", f->day[1].code, f->day[1].hi, f->day[1].lo, f->day[1].pop);
}

static bool fresh(forecast_t *out)
{
    taskENTER_CRITICAL(&s_lock);
    *out = s_fc;
    taskEXIT_CRITICAL(&s_lock);
    return out->valid && esp_timer_get_time() - out->fetched_us < STALE_US;
}

/* Words for the text version (numbers are written as digits). */
static const struct { const char *name, *text; } WORDS[] = {
    { "intro", "Here's the weather." }, { "now", "Right now," }, { "and", "and" }, { "degrees", "degrees." },
    { "today", "Today," }, { "tonight", "Tonight," }, { "tomorrow", "Tomorrow," },
    { "high", "with a high of" }, { "low", "and a low of" }, { "low_only", "a low of" },
    { "pct_rain", "percent chance of rain." }, { "pct_snow", "percent chance of snow." }, { "minus", "minus" },
    { "c_clear", "clear skies," }, { "c_mostly_clear", "mostly clear," }, { "c_partly_cloudy", "partly cloudy," },
    { "c_cloudy", "cloudy," }, { "c_fog", "foggy," }, { "c_drizzle", "drizzle," },
    { "c_freezing_drizzle", "freezing drizzle," }, { "c_light_rain", "light rain," }, { "c_rain", "rain," },
    { "c_heavy_rain", "heavy rain," }, { "c_freezing_rain", "freezing rain," }, { "c_light_snow", "light snow," },
    { "c_snow", "snow," }, { "c_heavy_snow", "heavy snow," }, { "c_showers", "rain showers," },
    { "c_heavy_showers", "heavy rain showers," }, { "c_snow_showers", "snow showers," },
    { "c_thunder", "thunderstorms," }, { "c_thunder_hail", "thunderstorms with hail," },
};

size_t weather_report_text(char *buf, size_t cap)
{
    if (!cap) return 0;
    buf[0] = '\0';
    forecast_t f;
    if (!fresh(&f)) return snprintf(buf, cap, "(no forecast yet)");
    plan_t *p = malloc(sizeof(*p));
    if (!p) return 0;
    build(p, &f, weather_cfg().parts);
    size_t off = 0;
    for (int i = 0; i < p->n && off + 1 < cap; i++) {
        const char *t = NULL;
        char num[8];
        if (p->name[i][0] == 'n' && p->name[i][1] >= '0' && p->name[i][1] <= '9') {
            snprintf(num, sizeof(num), "%s", p->name[i] + 1);
            t = num;
        }
        for (size_t k = 0; !t && k < sizeof(WORDS) / sizeof(WORDS[0]); k++)
            if (!strcmp(WORDS[k].name, p->name[i])) t = WORDS[k].text;
        const char *txt = t ? t : p->name[i];
        bool stop = p->gap[i] >= GAP_SENTENCE && txt[strlen(txt) - 1] != '.';
        int w = snprintf(buf + off, cap - off, "%s%s%s", off ? " " : "", txt, stop ? "." : "");
        if (w > 0) off += (size_t)w < cap - off ? (size_t)w : cap - off - 1;
    }
    free(p);
    /* A capital after each full stop (the clips' texts are lower-case where they join). */
    for (size_t i = 0; i < off; i++)
        if (buf[i] >= 'a' && buf[i] <= 'z' && (i == 0 || (i >= 2 && buf[i - 1] == ' ' && buf[i - 2] == '.')))
            buf[i] -= 32;
    return off;
}

esp_err_t weather_play(int line, player_done_cb_t done_cb, void *ctx)
{
    char *idx = read_index();
    if (!idx) return ESP_ERR_NOT_FOUND;
    plan_t *p = malloc(sizeof(*p));
    player_seg_t *segs = malloc(PLAN_MAX * sizeof(*segs));
    esp_err_t err = ESP_ERR_NO_MEM;
    if (p && segs) {
        forecast_t f;
        if (fresh(&f)) {
            build(p, &f, weather_cfg().parts);
        } else {
            p->n = 0;
            add(p, "unavailable", 0);
            weather_refresh();
        }
        size_t n = 0;
        for (int i = 0; i < p->n; i++) {
            uint32_t off, len;
            if (!index_lookup(idx, p->name[i], &off, &len)) {
                ESP_LOGW(TAG, "voice pack has no '%s'", p->name[i]);
                continue;
            }
            segs[n++] = (player_seg_t){ .off = off, .len = len, .gap_ms = p->gap[i] };
        }
        err = n ? player_play_segments(line, WEATHER_PACK_NAME, segs, n, done_cb, ctx) : ESP_ERR_NOT_FOUND;
    }
    free(segs);
    free(p);
    free(idx);
    return err;
}

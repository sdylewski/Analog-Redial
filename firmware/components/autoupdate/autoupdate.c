/* autoupdate.c -- see autoupdate.h. */
#include "autoupdate.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "activity.h"
#include "board.h"
#include "board_config.h"
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include "nvs.h"
#include "telephony.h"
#include "wifi.h"

static const char *TAG = "autoupdate";

#define NVS_NS        "autoupd"
#define TICK_MS       30000
#define RETRY_S       300
#define MAX_TRIES     6              /* per night */
#define CHUNK         2048
#define MANIFEST_MAX  3072
#define MIN_FREE_HEAP (48 * 1024)
#define MIN_BLOCK     (20 * 1024)
/* Images may only come from the project's own firmware branch. */
#define TRUSTED_PREFIX "https://raw.githubusercontent.com/sdylewski/Analog-Redial/firmware/"

volatile uint32_t g_autoupdate_progress;

static autoupdate_cfg_t s_cfg = { .enabled = false, .hour = 2 };
static char             s_status[224];
static volatile bool    s_busy;
static volatile bool    s_now;
static volatile bool    s_slot_taken;
static TaskHandle_t     s_task;
static int              s_done_day = -1;     /* tm_year*400 + tm_yday of the last finished night */
static int              s_tries;

/* ---- the spare slot ----------------------------------------------------- */

static portMUX_TYPE s_slot_lock = portMUX_INITIALIZER_UNLOCKED;

bool autoupdate_claim_slot(void)
{
    bool ok;
    portENTER_CRITICAL(&s_slot_lock);
    ok = !s_slot_taken;
    if (ok) s_slot_taken = true;
    portEXIT_CRITICAL(&s_slot_lock);
    return ok;
}

void autoupdate_release_slot(void) { s_slot_taken = false; }

/* ---- config / status ----------------------------------------------------- */

autoupdate_cfg_t autoupdate_get(void) { return s_cfg; }
const char *autoupdate_status(void) { return s_status; }
bool autoupdate_busy(void) { return s_busy; }

static void set_status(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void set_status(const char *fmt, ...)
{
    char msg[sizeof(s_status) - 24];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    time_t now = time(NULL);
    struct tm lt;
    char stamp[24] = "";
    if (now > 1700000000) {
        localtime_r(&now, &lt);
        strftime(stamp, sizeof(stamp), "%b %d %H:%M", &lt);
    }
    size_t w = (size_t)snprintf(s_status, sizeof(s_status), "%s%s", stamp, stamp[0] ? ": " : "");
    if (w < sizeof(s_status)) strlcat(s_status, msg, sizeof(s_status));
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "last", s_status);
        nvs_commit(h);
        nvs_close(h);
    }
    activity_log("Automatic update: %s", msg);
    ESP_LOGI(TAG, "%s", msg);
}

esp_err_t autoupdate_set(bool enabled, int hour)
{
    if (hour < 0 || hour > 23) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, "en", enabled ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_u8(h, "hour", (uint8_t)hour);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) return err;
    s_cfg.enabled = enabled;
    s_cfg.hour = hour;
    s_done_day = -1;      /* a changed hour counts from now */
    s_tries = 0;
    ESP_LOGI(TAG, "automatic updates %s, at %02d:00", enabled ? "on" : "off", hour);
    return ESP_OK;
}

void autoupdate_check_now(void)
{
    s_now = true;
    if (s_task) xTaskNotifyGive(s_task);
}

/* ---- version / manifest -------------------------------------------------- */

/* "v1.0.4" -> 1,0,4. false for anything with a suffix: a development build. */
static bool parse_release(const char *v, int out[3])
{
    if (*v == 'v') v++;
    int n = 0;
    char tail = 0;
    if (sscanf(v, "%d.%d.%d%c", &out[0], &out[1], &out[2], &tail) < 3) return false;
    (void)n;
    return tail == 0;
}

static bool newer(const int a[3], const int b[3])
{
    for (int i = 0; i < 3; i++) if (a[i] != b[i]) return a[i] > b[i];
    return false;
}

typedef struct {
    char version[24];
    char url[200];
    char sha256[65];
    uint32_t size;
} release_t;

static esp_http_client_handle_t open_url(const char *url, int timeout_ms, int64_t *len, int *status)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = timeout_ms,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 1024,
        .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return NULL;
    if (esp_http_client_open(c, 0) != ESP_OK) { esp_http_client_cleanup(c); return NULL; }
    *len = esp_http_client_fetch_headers(c);
    *status = esp_http_client_get_status_code(c);
    return c;
}

/* 0 = got the manifest; fills rel (url/sha/size empty if nothing for this board). */
static int fetch_manifest(release_t *rel)
{
    memset(rel, 0, sizeof(*rel));
    int64_t len = 0;
    int status = 0;
    esp_http_client_handle_t c = open_url(AUTOUPDATE_MANIFEST_URL, 15000, &len, &status);
    if (!c) return -1;
    char *buf = malloc(MANIFEST_MAX);
    int got = 0, n;
    if (buf && status == 200) {
        while (got < MANIFEST_MAX - 1 && (n = esp_http_client_read(c, buf + got, MANIFEST_MAX - 1 - got)) > 0) got += n;
        buf[got] = '\0';
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    int rc = -2;
    if (buf && status == 200 && got > 0) {
        cJSON *j = cJSON_Parse(buf);
        cJSON *ver = cJSON_GetObjectItem(j, "version");
        cJSON *base = cJSON_GetObjectItem(j, "base");
        cJSON *img = cJSON_GetObjectItem(cJSON_GetObjectItem(j, "images"), BOARD_IMAGE_ID);
        if (cJSON_IsString(ver)) {
            snprintf(rel->version, sizeof(rel->version), "%s", ver->valuestring);
            rc = 0;
            cJSON *file = cJSON_GetObjectItem(img, "file");
            cJSON *sha = cJSON_GetObjectItem(img, "sha256");
            cJSON *size = cJSON_GetObjectItem(img, "size");
            if (cJSON_IsString(file) && cJSON_IsString(sha) && cJSON_IsNumber(size) &&
                strlen(sha->valuestring) == 64) {
                snprintf(rel->url, sizeof(rel->url), "%s%s",
                         cJSON_IsString(base) ? base->valuestring : TRUSTED_PREFIX, file->valuestring);
                snprintf(rel->sha256, sizeof(rel->sha256), "%s", sha->valuestring);
                rel->size = (uint32_t)size->valuedouble;
            }
        }
        cJSON_Delete(j);
    } else if (status != 200 && status != 0) {
        rc = -3;
    }
    free(buf);
    return rc;
}

/* ---- download + install ---------------------------------------------------- */

static bool lines_idle(void)
{
    int n = board_hw_config_get().num_lines;
    for (int l = 0; l < n; l++) if (telephony_state(l) != TEL_IDLE) return false;
    return true;
}

/* Finds board_image_marker's id in the streamed image (same check as the web upload). */
typedef struct { uint8_t carry[64]; size_t carry_len; char found[32]; } id_scan_t;

static void id_scan(id_scan_t *s, const uint8_t *data, size_t n)
{
    if (s->found[0]) return;
    const char *m = board_image_marker;
    size_t plen = (size_t)(strchr(m, '\x02') - m) + 1;
    while (n > 0) {
        uint8_t win[sizeof(s->carry) + CHUNK];
        size_t wn = s->carry_len;
        memcpy(win, s->carry, wn);
        size_t take = n < sizeof(win) - wn ? n : sizeof(win) - wn;
        memcpy(win + wn, data, take);
        wn += take; data += take; n -= take;
        size_t keep_from = wn > sizeof(s->carry) ? wn - sizeof(s->carry) : 0;
        for (size_t i = 0; i + plen <= wn; i++) {
            if (win[i] != 0x01 || memcmp(win + i, m, plen) != 0) continue;
            size_t j = i + plen, k = 0;
            while (j < wn && (isalnum(win[j]) || win[j] == '_' || win[j] == '/') && k < sizeof(s->found) - 1)
                s->found[k++] = (char)win[j++];
            if (j < wn && win[j] == 0x03) { s->found[k] = '\0'; return; }
            s->found[0] = '\0';
            keep_from = i;
            break;
        }
        s->carry_len = wn - keep_from;
        memcpy(s->carry, win + keep_from, s->carry_len);
    }
}

/* A ranged request for bytes [from, to]. The download is fetched in pieces so a
 * dropped connection (a busy or marginal Wi-Fi link, a board short of memory)
 * costs one piece, not the whole image: the piece is simply asked for again from
 * where it stopped. */
#define SEGMENT (256 * 1024)

static esp_http_client_handle_t open_range(const char *url, uint32_t from, uint32_t to, int *status)
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
    char range[40];
    snprintf(range, sizeof(range), "bytes=%u-%u", (unsigned)from, (unsigned)to);
    esp_http_client_set_header(c, "Range", range);
    if (esp_http_client_open(c, 0) != ESP_OK) { esp_http_client_cleanup(c); return NULL; }
    esp_http_client_fetch_headers(c);
    *status = esp_http_client_get_status_code(c);
    return c;
}

/* Returns an error text, or NULL once the image is written and set as the next boot. */
static const char *install(const release_t *rel)
{
    if (strncmp(rel->url, TRUSTED_PREFIX, strlen(TRUSTED_PREFIX)) != 0) return "the release points somewhere unexpected";
    if (rel->size < 400000 || rel->size > 0x300000) return "the release has an odd size";
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (!target || target->size < rel->size) return "no spare firmware slot big enough";

    uint8_t *chunk = malloc(CHUNK);
    if (!chunk) return "out of memory";
    esp_ota_handle_t oh = 0;
    const char *err = NULL;
    if (esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &oh) != ESP_OK) {
        free(chunk);
        return "couldn't start writing to flash";
    }
    board_led_set_state(BOARD_LED_OTA_UPDATING);
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    id_scan_t scan = {0};
    uint32_t total = 0;
    int fails = 0, pieces = 0;
    int64_t t0 = esp_timer_get_time();

    while (!err && total < rel->size) {
        if (esp_timer_get_time() - t0 > 20LL * 60 * 1000000) { err = "the download took too long"; break; }
        uint32_t end = total + SEGMENT < rel->size ? total + SEGMENT : rel->size;   /* exclusive */
        int status = 0;
        esp_http_client_handle_t c = open_range(rel->url, total, end - 1, &status);
        if (c && status != 206 && !(status == 200 && total == 0)) {
            esp_http_client_close(c); esp_http_client_cleanup(c);
            c = NULL;
        }
        uint32_t before = total;
        if (c) {
            pieces++;
            int n, quiet = 0;
            while (total < end) {
                n = esp_http_client_read(c, (char *)chunk, (int)(end - total < CHUNK ? end - total : CHUNK));
                if (n > 0) {
                    quiet = 0;
                    mbedtls_sha256_update(&sha, chunk, (size_t)n);
                    id_scan(&scan, chunk, (size_t)n);
                    if (esp_ota_write(oh, chunk, (size_t)n) != ESP_OK) { err = "writing to flash failed"; break; }
                    total += (uint32_t)n;
                    g_autoupdate_progress++;
                } else if (esp_http_client_is_complete_data_received(c) || ++quiet > 6) {
                    break;   /* a timed-out read (n < 0) gets a few more tries before the piece is redone */
                }
            }
            esp_http_client_close(c);
            esp_http_client_cleanup(c);
        }
        if (err) break;
        if (total > before) {
            fails = 0;
        } else if (++fails > 10) {
            err = "the connection kept dropping";
        } else {
            ESP_LOGW(TAG, "no data at byte %u (piece %d, free heap %u) -- trying again", (unsigned)total, pieces,
                     (unsigned)esp_get_free_heap_size());
            vTaskDelay(pdMS_TO_TICKS(3000));
        }
        vTaskDelay(1);   /* let Wi-Fi and the web server breathe */
    }

    uint8_t digest[32];
    mbedtls_sha256_finish(&sha, digest);
    mbedtls_sha256_free(&sha);
    free(chunk);
    if (!err && total != rel->size) err = "the download stopped early";
    if (!err) {
        char hexd[65];
        for (int i = 0; i < 32; i++) sprintf(hexd + i * 2, "%02x", digest[i]);
        if (strcasecmp(hexd, rel->sha256) != 0) err = "the download's checksum doesn't match";
    }
    if (err) { esp_ota_abort(oh); board_led_set_state(BOARD_LED_OK); return err; }
    ESP_LOGI(TAG, "downloaded %u bytes in %d piece(s), %d s", (unsigned)total, pieces,
             (int)((esp_timer_get_time() - t0) / 1000000));
    if (esp_ota_end(oh) != ESP_OK) err = "that isn't a valid firmware image";
    else if (strcmp(scan.found, BOARD_IMAGE_ID) != 0) err = "that image is for a different board";
    else if (esp_ota_set_boot_partition(target) != ESP_OK) err = "couldn't set it as the next boot";
    if (err) board_led_set_state(BOARD_LED_OK);
    return err;
}

/* ---- one pass ------------------------------------------------------------- */

/* true = this night is finished (done, or nothing more worth trying); false = try again later. */
static bool run_once(bool manual)
{
    const esp_app_desc_t *app = esp_app_get_description();
    int cur[3];
    bool is_release = parse_release(app->version, cur);
    if (!wifi_is_connected()) return false;
    if (!lines_idle()) { if (manual) set_status("a phone is in use: try again in a few minutes"); return false; }
    if (heap_caps_get_free_size(MALLOC_CAP_8BIT) < MIN_FREE_HEAP ||
        heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < MIN_BLOCK) {
        if (manual) set_status("not enough free memory right now: try again later");
        return false;
    }
    if (!autoupdate_claim_slot()) { if (manual) set_status("an upload is in progress"); return false; }

    bool finished = true;
    release_t rel;
    int rc = fetch_manifest(&rel);
    int latest[3];
    if (rc != 0) {
        set_status("couldn't read the release list from GitHub (%s)", rc == -3 ? "server error" : "no connection");
        finished = false;
    } else if (!parse_release(rel.version, latest)) {
        set_status("the release list has an odd version (%s)", rel.version);
    } else if (!is_release) {
        set_status("latest release is %s; this is a development build (%s), so it isn't replaced "
                   "automatically. Install a release once with Check for updates to start nightly updates",
                   rel.version, app->version);
    } else if (!newer(latest, cur)) {
        set_status("up to date (%s)", app->version);
    } else if (!rel.url[0]) {
        set_status("%s is out, but not for this board (%s)", rel.version, BOARD_IMAGE_ID);
    } else {
        set_status("installing %s over %s...", rel.version, app->version);
        const char *err = install(&rel);
        if (err) {
            set_status("%s failed: %s", rel.version, err);
            finished = false;
        } else {
            set_status("installed %s, restarting to use it", rel.version);
            for (int waited = 0; waited < 1800 && !lines_idle(); waited += 10) vTaskDelay(pdMS_TO_TICKS(10000));
            vTaskDelay(pdMS_TO_TICKS(1500));
            fflush(stdout);
            esp_restart();
        }
    }
    autoupdate_release_slot();
    return finished;
}

static void autoupdate_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(TICK_MS));
        if (s_now) {
            s_now = false;
            s_busy = true;
            run_once(true);
            s_busy = false;
            continue;
        }
        if (!s_cfg.enabled) continue;
        time_t now = time(NULL);
        if (now < 1700000000) continue;            /* clock not set yet */
        struct tm lt;
        localtime_r(&now, &lt);
        int day = lt.tm_year * 400 + lt.tm_yday;
        if (lt.tm_hour != s_cfg.hour || day == s_done_day) {
            if (lt.tm_hour != s_cfg.hour) s_tries = 0;
            continue;
        }
        static int64_t next_try_us;
        if (esp_timer_get_time() < next_try_us) continue;
        s_busy = true;
        bool finished = run_once(false);
        s_busy = false;
        if (finished || ++s_tries >= MAX_TRIES) s_done_day = day;
        else next_try_us = esp_timer_get_time() + (int64_t)RETRY_S * 1000000;
    }
}

void autoupdate_init(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t en = 0, hour = 2;
        nvs_get_u8(h, "en", &en);
        if (nvs_get_u8(h, "hour", &hour) == ESP_OK && hour > 23) hour = 2;
        s_cfg.enabled = en != 0;
        s_cfg.hour = hour;
        size_t sz = sizeof(s_status);
        nvs_get_str(h, "last", s_status, &sz);
        nvs_close(h);
    }
    xTaskCreate(autoupdate_task, "autoupd", 6144, NULL, tskIDLE_PRIORITY + 1, &s_task);
    if (s_cfg.enabled) ESP_LOGI(TAG, "automatic updates on, at %02d:00 local time", s_cfg.hour);
}

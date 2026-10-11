/*
 * webauth.c — see webauth.h.
 */
#include "webauth.h"

#include <string.h>
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "mbedtls/md.h"
#include "mbedtls/pkcs5.h"
#include "nvs.h"

static const char *TAG = "webauth";

#define NVS_NS        "webauth"
#define NVS_KEY_HASH  "hash"     /* blob: salt[16] + hash[32] */
#define SALT_LEN      16
#define HASH_LEN      32
/* ~0.1 s per check on this chip: cheap for a sign-in, slow for anyone who
 * copied the NVS hash and tries a word list against it. */
#define PBKDF2_ITERS  4096

#define MAX_SESSIONS  6
#define FREE_TRIES    5
#define LOCK_FIRST_S  30
#define LOCK_MAX_S    600

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static bool    s_enabled;
static uint8_t s_salt[SALT_LEN];
static uint8_t s_hash[HASH_LEN];

typedef struct {
    char    token[WEBAUTH_TOKEN_LEN + 1];
    int64_t started_us;
} session_t;
static session_t s_sessions[MAX_SESSIONS];

static int     s_fails;
static int64_t s_locked_until_us;

#define NVS_KEY_DAYS  "sess_days"   /* u16 */
#define NVS_KEY_HINT  "hint"        /* string */
static uint16_t s_session_days = WEBAUTH_SESSION_DAYS_DEFAULT;
static char     s_hint[WEBAUTH_HINT_BYTES + 1];

static int64_t now_us(void) { return esp_timer_get_time(); }

static void derive(const char *password, const uint8_t salt[SALT_LEN], uint8_t out[HASH_LEN])
{
    mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256,
                                  (const unsigned char *)password, strlen(password),
                                  salt, SALT_LEN, PBKDF2_ITERS, HASH_LEN, out);
}

static bool equal_ct(const uint8_t *a, const uint8_t *b, size_t n)
{
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++) d |= a[i] ^ b[i];
    return d == 0;
}

static void sessions_clear(void)
{
    memset(s_sessions, 0, sizeof(s_sessions));
}

static bool days_allowed(uint16_t d)
{
    return d == 0 || d == 1 || d == 7 || d == 30 || d == 90 || d == 365;
}

void webauth_init(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    uint8_t blob[SALT_LEN + HASH_LEN];
    size_t len = sizeof(blob);
    if (nvs_get_blob(h, NVS_KEY_HASH, blob, &len) == ESP_OK && len == sizeof(blob)) {
        memcpy(s_salt, blob, SALT_LEN);
        memcpy(s_hash, blob + SALT_LEN, HASH_LEN);
        s_enabled = true;
    }
    uint16_t days;
    if (nvs_get_u16(h, NVS_KEY_DAYS, &days) == ESP_OK && days_allowed(days)) s_session_days = days;
    size_t hl = sizeof(s_hint);
    if (nvs_get_str(h, NVS_KEY_HINT, s_hint, &hl) != ESP_OK) s_hint[0] = '\0';
    s_hint[WEBAUTH_HINT_MAX] = '\0';
    nvs_close(h);
    if (s_enabled) ESP_LOGI(TAG, "web UI password is set");
}

bool webauth_enabled(void) { return s_enabled; }

esp_err_t webauth_set_password(const char *password)
{
    size_t n = strlen(password);
    if (n == 0 || n > WEBAUTH_PASSWORD_MAX) return ESP_ERR_INVALID_ARG;
    uint8_t blob[SALT_LEN + HASH_LEN];
    esp_fill_random(blob, SALT_LEN);
    derive(password, blob, blob + SALT_LEN);

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(h, NVS_KEY_HASH, blob, sizeof(blob));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) return err;

    portENTER_CRITICAL(&s_mux);
    memcpy(s_salt, blob, SALT_LEN);
    memcpy(s_hash, blob + SALT_LEN, HASH_LEN);
    s_enabled = true;
    sessions_clear();
    s_fails = 0;
    s_locked_until_us = 0;
    portEXIT_CRITICAL(&s_mux);
    ESP_LOGI(TAG, "web UI password set");
    return ESP_OK;
}

esp_err_t webauth_clear(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_erase_key(h, NVS_KEY_HASH);
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    {
        nvs_handle_t hh;
        if (nvs_open(NVS_NS, NVS_READWRITE, &hh) == ESP_OK) {
            nvs_erase_key(hh, NVS_KEY_HINT);
            nvs_commit(hh);
            nvs_close(hh);
        }
    }
    portENTER_CRITICAL(&s_mux);
    s_enabled = false;
    s_hint[0] = '\0';
    memset(s_hash, 0, sizeof(s_hash));
    sessions_clear();
    s_fails = 0;
    s_locked_until_us = 0;
    portEXIT_CRITICAL(&s_mux);
    ESP_LOGW(TAG, "web UI password removed");
    return err;
}

webauth_result_t webauth_check(const char *password, int *retry_secs)
{
    if (!s_enabled) return WEBAUTH_OK;
    int64_t now = now_us();
    if (now < s_locked_until_us) {
        if (retry_secs) *retry_secs = (int)((s_locked_until_us - now) / 1000000) + 1;
        return WEBAUTH_LOCKED;
    }
    uint8_t got[HASH_LEN];
    derive(password, s_salt, got);
    if (equal_ct(got, s_hash, HASH_LEN)) {
        s_fails = 0;
        return WEBAUTH_OK;
    }
    if (++s_fails >= FREE_TRIES) {
        int secs = LOCK_FIRST_S;
        for (int i = FREE_TRIES; i < s_fails && secs < LOCK_MAX_S; i++) secs *= 2;
        if (secs > LOCK_MAX_S) secs = LOCK_MAX_S;
        s_locked_until_us = now + (int64_t)secs * 1000000;
        ESP_LOGW(TAG, "%d wrong passwords in a row: refusing sign-ins for %d s", s_fails, secs);
    }
    return WEBAUTH_WRONG;
}

void webauth_session_new(char out[WEBAUTH_TOKEN_LEN + 1])
{
    uint8_t raw[WEBAUTH_TOKEN_LEN / 2];
    esp_fill_random(raw, sizeof(raw));
    for (size_t i = 0; i < sizeof(raw); i++) {
        static const char hex[] = "0123456789abcdef";
        out[2 * i]     = hex[raw[i] >> 4];
        out[2 * i + 1] = hex[raw[i] & 15];
    }
    out[WEBAUTH_TOKEN_LEN] = '\0';

    /* An empty slot, else the oldest. */
    int slot = 0;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (!s_sessions[i].token[0]) { slot = i; break; }
        if (s_sessions[i].started_us < s_sessions[slot].started_us) slot = i;
    }
    portENTER_CRITICAL(&s_mux);
    memcpy(s_sessions[slot].token, out, WEBAUTH_TOKEN_LEN + 1);
    s_sessions[slot].started_us = now_us();
    portEXIT_CRITICAL(&s_mux);
}

bool webauth_session_valid(const char *token)
{
    if (!token || strlen(token) != WEBAUTH_TOKEN_LEN) return false;
    int64_t now = now_us();
    bool ok = false;
    portENTER_CRITICAL(&s_mux);
    for (int i = 0; i < MAX_SESSIONS; i++) {
        session_t *s = &s_sessions[i];
        if (!s->token[0]) continue;
        int64_t limit_s = (int64_t)(s_session_days ? s_session_days : 1) * 24 * 3600;
        if (now - s->started_us > limit_s * 1000000) {
            memset(s, 0, sizeof(*s));
            continue;
        }
        if (equal_ct((const uint8_t *)s->token, (const uint8_t *)token, WEBAUTH_TOKEN_LEN)) ok = true;
    }
    portEXIT_CRITICAL(&s_mux);
    return ok;
}

void webauth_session_end(const char *token)
{
    if (!token || strlen(token) != WEBAUTH_TOKEN_LEN) return;
    portENTER_CRITICAL(&s_mux);
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (strcmp(s_sessions[i].token, token) == 0) memset(&s_sessions[i], 0, sizeof(s_sessions[i]));
    }
    portEXIT_CRITICAL(&s_mux);
}

/* ---- sign-in duration and hint -------------------------------------------- */

uint16_t webauth_session_days(void) { return s_session_days; }

esp_err_t webauth_set_session_days(uint16_t days)
{
    if (!days_allowed(days)) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u16(h, NVS_KEY_DAYS, days);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) s_session_days = days;
    return err;
}

int webauth_session_cookie_secs(void)
{
    return s_session_days ? (int)s_session_days * 24 * 3600 : -1;
}

void webauth_hint_get(char *out, size_t out_len)
{
    if (!out_len) return;
    strlcpy(out, s_hint, out_len);
}

esp_err_t webauth_hint_set(const char *hint)
{
    /* Trim, and reject control characters (it ends up in a web page). */
    while (*hint == ' ') hint++;
    char buf[WEBAUTH_HINT_BYTES + 1];
    size_t n = strlen(hint);
    if (n > WEBAUTH_HINT_BYTES) return ESP_ERR_INVALID_ARG;
    size_t chars = 0;                       /* characters, not bytes */
    for (size_t i = 0; i < n; i++) if (((unsigned char)hint[i] & 0xC0) != 0x80) chars++;
    if (chars > WEBAUTH_HINT_MAX) return ESP_ERR_INVALID_ARG;
    memcpy(buf, hint, n + 1);
    while (n && buf[n - 1] == ' ') buf[--n] = '\0';
    for (size_t i = 0; i < n; i++) {
        if ((unsigned char)buf[i] < 0x20) return ESP_ERR_INVALID_ARG;
    }
    /* A hint that is the password defeats the point: anyone on the network
     * could read it. Compared the same way a sign-in is, without counting
     * as a failed attempt. */
    if (n && s_enabled) {
        uint8_t got[HASH_LEN];
        derive(buf, s_salt, got);
        if (equal_ct(got, s_hash, HASH_LEN)) return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = n ? nvs_set_str(h, NVS_KEY_HINT, buf) : nvs_erase_key(h, NVS_KEY_HINT);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) memcpy(s_hint, buf, n + 1);
    return err;
}


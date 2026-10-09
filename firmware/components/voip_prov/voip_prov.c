#include "voip_prov.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "board.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "mbedtls/md.h"
#include "mbedtls/sha256.h"
#include "mdns.h"
#include "nvs.h"
#include "voip.h"

static const char *TAG = "voip_prov";

#define NVS_NS      "voipprov"
#define URL_LEN     96
#define KEY_LEN     80
#define TOKEN_LEN   72
#define RESP_MAX    1024
#define TIMEOUT_MS  8000

static bool s_loaded;
static char s_url[URL_LEN];
static char s_key[KEY_LEN];
static char s_token[TOKEN_LEN];

/* ---- storage ---------------------------------------------------------- */

static void load(void)
{
    if (s_loaded) return;
    s_loaded = true;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t n = sizeof(s_url);   nvs_get_str(h, "url", s_url, &n);
    n = sizeof(s_key);          nvs_get_str(h, "key", s_key, &n);
    n = sizeof(s_token);        nvs_get_str(h, "token", s_token, &n);
    nvs_close(h);
}

static esp_err_t save_str(const char *k, const char *v, char *cache, size_t cache_len)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = v[0] ? nvs_set_str(h, k, v) : nvs_erase_key(h, k);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) snprintf(cache, cache_len, "%s", v);
    return err;
}

const char *voip_prov_url(void) { load(); return s_url; }
bool voip_prov_has_key(void)    { load(); return s_key[0] != '\0'; }
bool voip_prov_enrolled(void)   { load(); return s_token[0] != '\0'; }

esp_err_t voip_prov_set_url(const char *url)
{
    load();
    char clean[URL_LEN];
    snprintf(clean, sizeof(clean), "%s", url);
    size_t n = strlen(clean);
    while (n && (clean[n - 1] == '/' || isspace((unsigned char)clean[n - 1]))) clean[--n] = '\0';
    if (n && strncmp(clean, "http://", 7) != 0 && strncmp(clean, "https://", 8) != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strcmp(clean, s_url) != 0) save_str("token", "", s_token, sizeof(s_token));
    return save_str("url", clean, s_url, sizeof(s_url));
}

esp_err_t voip_prov_set_key(const char *key)
{
    load();
    const char *dot = strchr(key, '.');
    if (!dot || dot == key || strlen(dot + 1) < 16) return ESP_ERR_INVALID_ARG;
    return save_str("key", key, s_key, sizeof(s_key));
}

esp_err_t voip_prov_forget(void)
{
    load();
    save_str("token", "", s_token, sizeof(s_token));
    save_str("key", "", s_key, sizeof(s_key));
    return save_str("url", "", s_url, sizeof(s_url));
}

/* ---- discovery -------------------------------------------------------- */

esp_err_t voip_prov_discover(char *url, size_t url_len, char *name, size_t name_len)
{
    mdns_result_t *res = NULL;
    esp_err_t err = mdns_query_ptr("_redial-prov", "_tcp", 3000, 4, &res);
    if (err != ESP_OK) return err;
    err = ESP_ERR_NOT_FOUND;
    for (mdns_result_t *r = res; r; r = r->next) {
        if (!r->hostname || !r->port) continue;
        snprintf(url, url_len, "http://%s.local:%u", r->hostname, r->port);
        if (name) snprintf(name, name_len, "%s", r->instance_name ? r->instance_name : r->hostname);
        err = ESP_OK;
        break;
    }
    mdns_query_results_free(res);
    return err;
}

/* ---- HTTP + signing --------------------------------------------------- */

static void hex(const uint8_t *in, size_t n, char *out)
{
    for (size_t i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", in[i]);
    out[2 * n] = '\0';
}

/* Sends one request; returns the HTTP status (or -1 when the server can't
 * be reached) and the response body, NUL-terminated, in resp. */
static int http_call(const char *method, const char *path, const char *body,
                     const char *auth_hdr, char *resp, size_t resp_len)
{
    char url[URL_LEN + 32];
    snprintf(url, sizeof(url), "%s%s", s_url, path);
    esp_http_client_config_t cfg = {
        .url = url,
        .method = strcmp(method, "POST") == 0 ? HTTP_METHOD_POST : HTTP_METHOD_GET,
        .timeout_ms = TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return -1;
    int blen = body ? (int)strlen(body) : 0;
    if (body) esp_http_client_set_header(c, "Content-Type", "application/json");
    if (auth_hdr) esp_http_client_set_header(c, "Authorization", auth_hdr);

    int status = -1;
    resp[0] = '\0';
    if (esp_http_client_open(c, blen) == ESP_OK) {
        if (blen == 0 || esp_http_client_write(c, body, blen) == blen) {
            esp_http_client_fetch_headers(c);
            status = esp_http_client_get_status_code(c);
            int got = 0, n;
            while (got < (int)resp_len - 1 &&
                   (n = esp_http_client_read(c, resp + got, resp_len - 1 - got)) > 0) {
                got += n;
            }
            resp[got] = '\0';
        }
    }
    esp_http_client_cleanup(c);
    return status;
}

/* Server time minus ours, from the unauthenticated info endpoint: lets a
 * board with no clock set still produce timestamps the server accepts. */
static esp_err_t server_offset(long *offset, char *resp, size_t resp_len)
{
    if (http_call("GET", "/api/v1/info", NULL, NULL, resp, resp_len) != 200) return ESP_FAIL;
    cJSON *j = cJSON_Parse(resp);
    cJSON *t = cJSON_GetObjectItem(j, "server_time");
    esp_err_t err = cJSON_IsNumber(t) ? ESP_OK : ESP_FAIL;
    if (err == ESP_OK) *offset = (long)(t->valuedouble - (double)time(NULL));
    cJSON_Delete(j);
    return err;
}

/* Signs and POSTs body to path, as the board (token) if it has one and
 * use_token, otherwise with the enrollment key. */
static int signed_post(const char *path, const char *body, bool use_token, long offset,
                       char *resp, size_t resp_len)
{
    char ident[48];
    const char *secret;
    size_t secret_len;
    if (use_token && s_token[0]) {
        char serial[16];
        board_serial_str(serial, sizeof(serial));
        for (char *p = serial; *p; p++) *p = (char)tolower((unsigned char)*p);
        snprintf(ident, sizeof(ident), "board:%s", serial);
        secret = s_token;
        secret_len = strlen(s_token);
    } else {
        const char *dot = strchr(s_key, '.');
        if (!dot) return -2;
        snprintf(ident, sizeof(ident), "key:%.*s", (int)(dot - s_key), s_key);
        secret = dot + 1;
        secret_len = strlen(secret);
    }

    uint8_t digest[32], rnd[16];
    char body_hash[65], nonce[33], sig[65], ts[16];
    mbedtls_sha256((const unsigned char *)body, strlen(body), digest, 0);
    hex(digest, 32, body_hash);
    esp_fill_random(rnd, sizeof(rnd));
    hex(rnd, sizeof(rnd), nonce);
    snprintf(ts, sizeof(ts), "%ld", (long)time(NULL) + offset);

    char msg[192];
    int mlen = snprintf(msg, sizeof(msg), "POST\n%s\n%s\n%s\n%s", path, ts, nonce, body_hash);
    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                    (const unsigned char *)secret, secret_len,
                    (const unsigned char *)msg, mlen, digest);
    hex(digest, 32, sig);

    char auth[256];
    snprintf(auth, sizeof(auth), "Redial-HMAC id=\"%s\", ts=\"%s\", nonce=\"%s\", sig=\"%s\"",
             ident, ts, nonce, sig);
    return http_call("POST", path, body, auth, resp, resp_len);
}

/* Copies the server's own "message" (written for people) into msg, or a
 * generic line if the reply had none. */
static void server_message(const char *resp, int status, char *msg, size_t msg_len)
{
    cJSON *j = cJSON_Parse(resp);
    cJSON *m = cJSON_GetObjectItem(j, "message");
    if (cJSON_IsString(m)) snprintf(msg, msg_len, "%s", m->valuestring);
    else if (status < 0) snprintf(msg, msg_len, "Can't reach the provisioner at %s", s_url);
    else snprintf(msg, msg_len, "The provisioner answered HTTP %d", status);
    cJSON_Delete(j);
}

static bool is_auth_rejection(const char *resp)
{
    cJSON *j = cJSON_Parse(resp);
    cJSON *e = cJSON_GetObjectItem(j, "error");
    bool r = cJSON_IsString(e) && (strcmp(e->valuestring, "unknown_id") == 0 ||
                                   strcmp(e->valuestring, "bad_signature") == 0);
    cJSON_Delete(j);
    return r;
}

/* Common front half of every operation: config present, server reachable,
 * clock offset known. Returns a response buffer the caller frees. */
static char *begin(long *offset, char *msg, size_t msg_len)
{
    load();
    if (!s_url[0]) { snprintf(msg, msg_len, "Set the provisioner address first"); return NULL; }
    if (!s_key[0] && !s_token[0]) { snprintf(msg, msg_len, "Enter the enrollment key first"); return NULL; }
    char *resp = malloc(RESP_MAX);
    if (!resp) { snprintf(msg, msg_len, "Out of memory"); return NULL; }
    if (server_offset(offset, resp, RESP_MAX) != ESP_OK) {
        snprintf(msg, msg_len, "Can't reach the provisioner at %s", s_url);
        free(resp);
        return NULL;
    }
    return resp;
}

/* Posts with the board token, falling back once to the enrollment key if
 * the server no longer knows the token (state reset, board revoked). */
static int post_with_fallback(const char *path, const char *body, long offset, char *resp)
{
    int st = signed_post(path, body, true, offset, resp, RESP_MAX);
    if (st == 401 && s_token[0] && s_key[0] && is_auth_rejection(resp)) {
        st = signed_post(path, body, false, offset, resp, RESP_MAX);
    }
    return st;
}

static bool digits_only(const char *s)
{
    if (!s[0] || strlen(s) > 10) return false;
    for (; *s; s++) if (!isdigit((unsigned char)*s)) return false;
    return true;
}

/* ---- operations ------------------------------------------------------- */

esp_err_t voip_prov_check(const char *number, bool *available, char *msg, size_t msg_len)
{
    *available = false;
    if (!digits_only(number)) { snprintf(msg, msg_len, "Numbers are digits only"); return ESP_ERR_INVALID_ARG; }
    long offset;
    char *resp = begin(&offset, msg, msg_len);
    if (!resp) return ESP_FAIL;

    char body[48];
    snprintf(body, sizeof(body), "{\"number\":\"%s\"}", number);
    int st = post_with_fallback("/api/v1/check", body, offset, resp);
    esp_err_t err = ESP_FAIL;
    if (st == 200) {
        cJSON *j = cJSON_Parse(resp);
        cJSON *reason = cJSON_GetObjectItem(j, "reason");
        *available = cJSON_IsTrue(cJSON_GetObjectItem(j, "available"));
        const char *r = cJSON_IsString(reason) ? reason->valuestring : "";
        snprintf(msg, msg_len, "%s %s", number,
                 strcmp(r, "yours") == 0 ? "is already yours" : *available ? "is free" : "is taken");
        cJSON_Delete(j);
        err = ESP_OK;
    } else {
        server_message(resp, st, msg, msg_len);
    }
    free(resp);
    return err;
}

esp_err_t voip_prov_register(int line, const char *number, char *msg, size_t msg_len)
{
    if (line < 0 || line > 1) return ESP_ERR_INVALID_ARG;
    if (!digits_only(number)) { snprintf(msg, msg_len, "Numbers are digits only"); return ESP_ERR_INVALID_ARG; }
    long offset;
    char *resp = begin(&offset, msg, msg_len);
    if (!resp) return ESP_FAIL;

    char serial[16], name[32], clean[32];
    board_serial_str(serial, sizeof(serial));
    for (char *p = serial; *p; p++) *p = (char)tolower((unsigned char)*p);
    board_name_get(name, sizeof(name));
    size_t k = 0;
    for (const char *p = name; *p && k < sizeof(clean) - 1; p++) {
        if (isalnum((unsigned char)*p) || *p == ' ' || *p == '-' || *p == '_') clean[k++] = *p;
    }
    clean[k] = '\0';

    char body[160];
    snprintf(body, sizeof(body),
             "{\"board_id\":\"%s\",\"board_name\":\"%s\",\"line\":%d,\"number\":\"%s\"}",
             serial, clean, line, number);
    int st = post_with_fallback("/api/v1/register", body, offset, resp);

    esp_err_t err = ESP_FAIL;
    if (st == 200) {
        cJSON *j = cJSON_Parse(resp);
        cJSON *server = cJSON_GetObjectItem(j, "sip_server");
        cJSON *user = cJSON_GetObjectItem(j, "username");
        cJSON *pass = cJSON_GetObjectItem(j, "password");
        cJSON *token = cJSON_GetObjectItem(j, "board_token");
        if (cJSON_IsString(server) && cJSON_IsString(user) && cJSON_IsString(pass)) {
            if (cJSON_IsString(token)) save_str("token", token->valuestring, s_token, sizeof(s_token));
            if (line == 0) voip_set_config(server->valuestring, user->valuestring, pass->valuestring, NULL, NULL);
            else           voip_set_config(server->valuestring, NULL, NULL, user->valuestring, pass->valuestring);
            voip_set_enabled(true);
            snprintf(msg, msg_len, "Line %d is now %s", line + 1, user->valuestring);
            ESP_LOGI(TAG, "line %d registered as %s on %s", line, user->valuestring, server->valuestring);
            err = ESP_OK;
        } else {
            snprintf(msg, msg_len, "The provisioner's reply was missing the account details");
        }
        cJSON_Delete(j);
    } else {
        server_message(resp, st, msg, msg_len);
    }
    free(resp);
    return err;
}

esp_err_t voip_prov_release(int line, char *msg, size_t msg_len)
{
    if (line < 0 || line > 1) return ESP_ERR_INVALID_ARG;
    long offset;
    char *resp = begin(&offset, msg, msg_len);
    if (!resp) return ESP_FAIL;
    if (!s_token[0]) {
        snprintf(msg, msg_len, "This board hasn't registered a number yet");
        free(resp);
        return ESP_ERR_INVALID_STATE;
    }

    char body[16];
    snprintf(body, sizeof(body), "{\"line\":%d}", line);
    int st = signed_post("/api/v1/release", body, true, offset, resp, RESP_MAX);
    esp_err_t err = ESP_FAIL;
    if (st == 200) {
        if (line == 0) voip_set_config(NULL, "", "", NULL, NULL);
        else           voip_set_config(NULL, NULL, NULL, "", "");
        snprintf(msg, msg_len, "Released line %d's number", line + 1);
        err = ESP_OK;
    } else {
        server_message(resp, st, msg, msg_len);
    }
    free(resp);
    return err;
}

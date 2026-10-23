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
#include "storage.h"
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
static int32_t s_nmin = -1, s_nmax = -1;

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
    nvs_get_i32(h, "nmin", &s_nmin);
    nvs_get_i32(h, "nmax", &s_nmax);
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

bool voip_prov_number_range(int *min, int *max)
{
    load();
    if (s_nmin < 0 || s_nmax < s_nmin) return false;
    *min = (int)s_nmin;
    *max = (int)s_nmax;
    return true;
}

/* Saves number_min/number_max from any provisioner reply that carries them. */
static void note_range(const cJSON *j)
{
    cJSON *lo = cJSON_GetObjectItem(j, "number_min");
    cJSON *hi = cJSON_GetObjectItem(j, "number_max");
    if (!cJSON_IsNumber(lo) || !cJSON_IsNumber(hi)) return;
    int32_t a = (int32_t)lo->valuedouble, b = (int32_t)hi->valuedouble;
    if (a == s_nmin && b == s_nmax) return;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i32(h, "nmin", a);
    nvs_set_i32(h, "nmax", b);
    nvs_commit(h);
    nvs_close(h);
    s_nmin = a;
    s_nmax = b;
}
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
    if (strcmp(clean, s_url) != 0) {
        save_str("token", "", s_token, sizeof(s_token));
        s_nmin = s_nmax = -1;
    }
    return save_str("url", clean, s_url, sizeof(s_url));
}

esp_err_t voip_prov_set_key(const char *key)
{
    load();
    const char *dot = strchr(key, '.');
    if (!dot || dot == key || strlen(dot + 1) < 16) return ESP_ERR_INVALID_ARG;
    return save_str("key", key, s_key, sizeof(s_key));
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
static int s_tts = -1;   /* the server offers text-to-speech: -1 not known yet, 0 no, 1 yes */
static char s_tts_voice[33];

int voip_prov_tts_state(void) { return s_tts; }
const char *voip_prov_tts_voice(void) { return s_tts_voice; }

void voip_prov_tts_tag(const char *voice, const char *text, char *out, size_t out_len)
{
    /* Whitespace collapsed to single spaces, ends trimmed: Python's " ".join(text.split()). */
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    bool started = false, pending_space = false;
    for (const char *p = text; *p; p++) {
        if (isspace((unsigned char)*p)) { pending_space = started; continue; }
        if (pending_space) mbedtls_sha256_update(&ctx, (const unsigned char *)" ", 1);
        mbedtls_sha256_update(&ctx, (const unsigned char *)p, 1);
        started = true;
        pending_space = false;
    }
    uint8_t digest[32];
    mbedtls_sha256_finish(&ctx, digest);
    mbedtls_sha256_free(&ctx);
    char hexd[17];
    hex(digest, 8, hexd);
    snprintf(out, out_len, "redial-tts voice=%s text=%s", voice, hexd);
}

static esp_err_t server_offset(long *offset, char *resp, size_t resp_len)
{
    if (http_call("GET", "/api/v1/info", NULL, NULL, resp, resp_len) != 200) return ESP_FAIL;
    cJSON *j = cJSON_Parse(resp);
    cJSON *tts = cJSON_GetObjectItem(j, "tts");
    s_tts = cJSON_IsBool(tts) ? cJSON_IsTrue(tts) : 0;
    cJSON *voice = cJSON_GetObjectItem(j, "tts_voice");
    snprintf(s_tts_voice, sizeof(s_tts_voice), "%s", cJSON_IsString(voice) ? voice->valuestring : "");
    cJSON *t = cJSON_GetObjectItem(j, "server_time");
    esp_err_t err = cJSON_IsNumber(t) ? ESP_OK : ESP_FAIL;
    if (err == ESP_OK) *offset = (long)(t->valuedouble - (double)time(NULL));
    cJSON_Delete(j);
    return err;
}

/* Builds the Authorization header for POSTing body to path, as the board
 * (token) if it has one and use_token, otherwise with the enrollment key. */
static bool make_auth(const char *path, const char *body, bool use_token, long offset,
                      char *auth, size_t auth_len)
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
        if (!dot) return false;
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

    snprintf(auth, auth_len, "Redial-HMAC id=\"%s\", ts=\"%s\", nonce=\"%s\", sig=\"%s\"",
             ident, ts, nonce, sig);
    return true;
}

static int signed_post(const char *path, const char *body, bool use_token, long offset,
                       char *resp, size_t resp_len)
{
    char auth[256];
    if (!make_auth(path, body, use_token, offset, auth, sizeof(auth))) return -2;
    return http_call("POST", path, body, auth, resp, resp_len);
}

/* Copies the server's own "message" (written for people) into msg, or a
 * generic line if the reply had none. */
static void server_message(const char *resp, int status, char *msg, size_t msg_len)
{
    cJSON *j = cJSON_Parse(resp);
    note_range(j);
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
        note_range(j);
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
        note_range(j);
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

esp_err_t voip_prov_refresh(char *msg, size_t msg_len)
{
    long offset;
    char *resp = begin(&offset, msg, msg_len);
    if (!resp) return ESP_FAIL;
    int st = post_with_fallback("/api/v1/status", "{}", offset, resp);
    esp_err_t err = ESP_FAIL;
    if (st == 200) {
        cJSON *j = cJSON_Parse(resp);
        note_range(j);
        cJSON_Delete(j);
        int lo, hi;
        if (voip_prov_number_range(&lo, &hi)) {
            snprintf(msg, msg_len, "Connected -- this key can register numbers %d to %d", lo, hi);
        } else {
            snprintf(msg, msg_len, "Connected");
        }
        err = ESP_OK;
    } else {
        server_message(resp, st, msg, msg_len);
    }
    free(resp);
    return err;
}

/* ---- text-to-speech ---------------------------------------------------- */

#define TTS_TIMEOUT_MS  90000             /* the server may queue behind other renders */
#define TTS_MAX_BYTES   (1024 * 1024)

/* One signed POST to /api/v1/tts. On 200 the WAV is streamed into storage as
 * `name`; otherwise the error reply lands in resp. Returns the HTTP status. */
static int tts_post(const char *body, bool use_token, long offset, const char *name,
                    char *resp, size_t resp_len, size_t *saved)
{
    char auth[256];
    if (!make_auth("/api/v1/tts", body, use_token, offset, auth, sizeof(auth))) return -2;
    char url[URL_LEN + 32];
    snprintf(url, sizeof(url), "%s/api/v1/tts", s_url);
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = TTS_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = true,
        .buffer_size = 1024,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return -1;
    esp_http_client_set_header(c, "Content-Type", "application/json");
    esp_http_client_set_header(c, "Authorization", auth);

    int status = -1, blen = (int)strlen(body);
    resp[0] = '\0';
    *saved = 0;
    if (esp_http_client_open(c, blen) == ESP_OK && esp_http_client_write(c, body, blen) == blen) {
        int64_t len = esp_http_client_fetch_headers(c);
        status = esp_http_client_get_status_code(c);
        if (status == 200) {
            storage_writer_t w;
            char *chunk = malloc(1024);
            bool ok = chunk && len > 44 && len <= TTS_MAX_BYTES &&
                      storage_write_begin_local(name, (size_t)len, &w) == ESP_OK;
            if (ok) {
                int n;
                while ((n = esp_http_client_read(c, chunk, 1024)) > 0) {
                    if (storage_write_chunk(&w, chunk, (size_t)n) != ESP_OK) { ok = false; break; }
                    *saved += (size_t)n;
                }
                if (ok && *saved == (size_t)len) storage_write_end(&w);
                else { storage_write_abort(&w); ok = false; }
            }
            free(chunk);
            if (!ok) {
                snprintf(resp, resp_len, "{\"message\":\"Couldn't save the audio on this board "
                                         "(%u of %lld bytes)\"}", (unsigned)*saved, (long long)len);
                status = -3;
            }
        } else {
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

esp_err_t voip_prov_tts(const char *text, const char *name, char *msg, size_t msg_len)
{
    if (!text || !text[0]) { snprintf(msg, msg_len, "Type the text to speak first"); return ESP_ERR_INVALID_ARG; }
    long offset;
    char *resp = begin(&offset, msg, msg_len);
    if (!resp) return ESP_FAIL;
    if (s_tts == 0) {
        snprintf(msg, msg_len, "This provisioner doesn't offer text-to-speech");
        free(resp);
        return ESP_ERR_NOT_SUPPORTED;
    }
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "text", text);
    char *body = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!body) { free(resp); snprintf(msg, msg_len, "Out of memory"); return ESP_ERR_NO_MEM; }

    size_t saved = 0;
    int st = tts_post(body, true, offset, name, resp, RESP_MAX, &saved);
    if (st == 401 && s_token[0] && s_key[0] && is_auth_rejection(resp)) {
        st = tts_post(body, false, offset, name, resp, RESP_MAX, &saved);
    }
    free(body);
    esp_err_t err = ESP_FAIL;
    if (st == 200) {
        snprintf(msg, msg_len, "Made on the server and saved (%u KB)", (unsigned)((saved + 512) / 1024));
        err = ESP_OK;
    } else {
        server_message(resp, st < 0 && resp[0] ? 500 : st, msg, msg_len);
    }
    ESP_LOGI(TAG, "tts '%s': %s", name, msg);
    free(resp);
    return err;
}

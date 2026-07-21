#include "webui.h"
#include "esp_http_server.h"
#include "cJSON.h"
#include "wifi.h"
#include "storage.h"
#include "logbuf.h"
#include "telephony.h"
#include "dialplan.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>

static const char *TAG = "webui";

/* Best-effort categorization of an ESP_LOG tag/message into the Pi UI's
 * .k.digit / .k.hook / .k.action / .k.debug color classes -- this project's
 * tags don't map 1:1 onto the Pi's original categories, so this is a
 * reasonable approximation, not a faithful port of that logic. */
static const char *log_kind_class(const char *tag, const char *msg)
{
    if (strcmp(tag, "ag1170") == 0) {
        if (strstr(msg, "digit") || strstr(msg, "DIGIT") || strstr(msg, "pulse")) return "digit";
        if (strstr(msg, "hook") || strstr(msg, "SHK") || strstr(msg, "ring")) return "hook";
    }
    if (strcmp(tag, "telephony") == 0 || strcmp(tag, "dialplan") == 0) return "action";
    return "debug";
}

/* Parses one "X (12345) tag: rest of message" ESP_LOG line (X = level
 * letter) into .t/.k/.m spans matching the Pi UI's .logline structure. Log
 * content here is entirely our own ESP_LOG calls (not user-supplied text),
 * so this intentionally skips HTML-escaping -- fine for a LAN bring-up
 * tool, would need escaping if log messages could ever include user input. */
static int append_log_line(char *out, size_t out_len, int off, const char *line)
{
    int ms = 0;
    char tag[32] = "";
    const char *msg = line;

    if (line[0] && line[1] == ' ' && line[2] == '(') {
        const char *p = line + 3;
        int n = 0;
        while (*p >= '0' && *p <= '9') { n = n * 10 + (*p - '0'); p++; }
        if (*p == ')') {
            p++;
            if (*p == ' ') p++;
            const char *tag_start = p;
            const char *colon = strchr(p, ':');
            if (colon && (size_t)(colon - tag_start) < sizeof(tag)) {
                size_t tlen = colon - tag_start;
                memcpy(tag, tag_start, tlen);
                tag[tlen] = 0;
                msg = colon + 1;
                if (*msg == ' ') msg++;
                ms = n;
            }
        }
    }
    const char *kind = tag[0] ? log_kind_class(tag, msg) : "debug";
    return off + snprintf(out + off, ((size_t)off < out_len) ? out_len - off : 0,
        "<div class=\"logline\"><span class=\"t\">%d</span>"
        "<span class=\"k %s\">%s</span><span class=\"m\">%s</span></div>",
        ms, kind, tag[0] ? tag : "-", msg);
}

/* Renders the whole logbuf snapshot as a .logpanel, newest line first (like
 * the Pi UI) so you don't have to scroll after every 5s auto-refresh --
 * logbuf_snapshot() itself returns oldest-first, so the line pointers are
 * collected up front and walked in reverse. Returns the new offset. */
static int append_log_panel(char *out, size_t out_len, int off, char *log_snapshot)
{
    off += snprintf(out + off, ((size_t)off < out_len) ? out_len - off : 0,
        "<div class=\"logpanel\" style=\"margin-top:16px\"><h2>Recent log</h2><div class=\"logbody\">");

    size_t max_lines = 600;
    char **lines = malloc(max_lines * sizeof(char *));
    size_t nlines = 0;
    if (lines) {
        char *saveptr = NULL;
        char *line = strtok_r(log_snapshot, "\n", &saveptr);
        while (line && nlines < max_lines) {
            if (*line) lines[nlines++] = line;
            line = strtok_r(NULL, "\n", &saveptr);
        }
        for (size_t i = nlines; i > 0; i--) {
            off = append_log_line(out, out_len, off, lines[i - 1]);
        }
        free(lines);
    }

    off += snprintf(out + off, ((size_t)off < out_len) ? out_len - off : 0, "</div></div>");
    return off;
}

static cJSON *build_status_json(void)
{
    cJSON *root = cJSON_CreateObject();
    char ip[16];
    wifi_get_ip_str(ip, sizeof(ip));
    cJSON_AddBoolToObject(root, "wifi_connected", wifi_is_connected());
    cJSON_AddStringToObject(root, "wifi_ssid", wifi_get_ssid());
    cJSON_AddStringToObject(root, "wifi_ip", ip);
    cJSON_AddNumberToObject(root, "wifi_rssi", wifi_rssi());
    cJSON_AddNumberToObject(root, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddNumberToObject(root, "heap_free", (double)esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "heap_min_free", (double)esp_get_minimum_free_heap_size());
    cJSON_AddNumberToObject(root, "storage_used", (double)storage_used_bytes());
    cJSON_AddNumberToObject(root, "storage_cap", (double)storage_cap_bytes());
    cJSON_AddBoolToObject(root, "storage_on_sd", storage_using_sd());

    cJSON *lines = cJSON_AddArrayToObject(root, "lines");
    for (int l = 0; l < 2; l++) {
        cJSON *jl = cJSON_CreateObject();
        cJSON_AddNumberToObject(jl, "line", l);
        cJSON_AddStringToObject(jl, "state", telephony_state_name(telephony_state(l)));
        cJSON_AddStringToObject(jl, "last_number", telephony_last_number(l));
        cJSON_AddItemToArray(lines, jl);
    }
    return root;
}

static esp_err_t status_json_handler(httpd_req_t *req)
{
    cJSON *root = build_status_json();
    char *text = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, text ? text : "{}");
    cJSON_Delete(root);
    free(text);
    return ESP_OK;
}

extern const uint8_t style_css_start[] asm("_binary_style_css_start");
extern const uint8_t style_css_end[]   asm("_binary_style_css_end");

static esp_err_t static_css_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/css");
    httpd_resp_send(req, (const char *)style_css_start,
                     style_css_end - style_css_start);
    return ESP_OK;
}

/* Same green/gray/amber "pill" classes the Pi UI uses for line state. */
static const char *pill_class_for(tel_state_t s)
{
    switch (s) {
    case TEL_IDLE: return "off";
    case TEL_BUSY:
    case TEL_REORDER: return "busy";
    default: return "on"; /* dialtone/dialing/ringback/ringing/incall */
    }
}

/* Nav tabs mirror the Pi UI's exactly (see pi_rotary_phone/web/templates/
 * index.html): Status / Recordings / Dial Guide / Settings. Shared across
 * every page handler so the tab set and .active highlighting can't drift
 * out of sync between pages. */
typedef struct { const char *href; const char *label; } nav_tab_t;
static const nav_tab_t NAV_TABS[] = {
    { "/",           "Status" },
    { "/recordings", "Recordings" },
    { "/guide",      "Dial Guide" },
    { "/settings",   "Settings" },
};
#define NAV_TABS_N (sizeof(NAV_TABS) / sizeof(NAV_TABS[0]))

static int append_page_head(char *buf, size_t cap, int off, const char *page_title,
                             const char *active_href, bool auto_refresh)
{
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<!doctype html><html><head><title>%s</title>"
        "%s"
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
        "<link rel=\"stylesheet\" href=\"/static/style.css\"></head><body>"
        "<header><h1>Rotary Phone Gateway</h1><nav>",
        page_title, auto_refresh ? "<meta http-equiv=\"refresh\" content=\"5\">" : "");
    for (size_t i = 0; i < NAV_TABS_N; i++) {
        bool active = strcmp(NAV_TABS[i].href, active_href) == 0;
        off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
            "<a%s href=\"%s\">%s</a>", active ? " class=\"active\"" : "",
            NAV_TABS[i].href, NAV_TABS[i].label);
    }
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0, "</nav>");
    return off;
}

static esp_err_t index_handler(httpd_req_t *req)
{
    char ip[16];
    wifi_get_ip_str(ip, sizeof(ip));
    int64_t uptime_s = esp_timer_get_time() / 1000000;

    /* Fetched up front so its size is known before sizing the page buffer --
     * each raw log line can roughly triple in size once wrapped in
     * .logline/.t/.k/.m markup. */
    size_t log_cap = 8200;
    char *log_snapshot = malloc(log_cap);
    if (log_snapshot) logbuf_snapshot(log_snapshot, log_cap);

    /* Heap, not stack -- this page + log panel can run tens of KB. */
    size_t cap = 6144 + log_cap * 3;
    char *buf = malloc(cap);
    if (!buf) { free(log_snapshot); httpd_resp_send_500(req); return ESP_FAIL; }

    int off = append_page_head(buf, cap, 0, "Rotary Phone Gateway", "/", true);
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<span class=\"dot\">%s%s%s</span></header>"
        "<main>"
        "<div class=\"cards\">"
        "<div class=\"card\"><h2>System</h2>"
        "<div class=\"statusrow\"><span class=\"label\">WiFi</span><span>%s, %d dBm</span></div>"
        "<div class=\"statusrow\"><span class=\"label\">IP</span><span>%s</span></div>"
        "<div class=\"statusrow\"><span class=\"label\">Uptime</span><span>%lld s</span></div>"
        "<div class=\"statusrow\"><span class=\"label\">Free heap</span><span>%u bytes</span></div>"
        "<div class=\"statusrow\"><span class=\"label\">Storage</span><span>%u / %u bytes%s</span></div>"
        "</div>",
        wifi_is_connected() ? "connected" : "disconnected",
        wifi_is_connected() ? " \xc2\xb7 " : "", wifi_is_connected() ? ip : "",
        wifi_get_ssid()[0] ? wifi_get_ssid() : "(not configured)", wifi_rssi(),
        ip, (long long)uptime_s,
        (unsigned)esp_get_free_heap_size(),
        (unsigned)storage_used_bytes(), (unsigned)storage_cap_bytes(),
        storage_using_sd() ? " (SD)" : " (internal flash)");

    for (int l = 0; l < 2 && off > 0 && (size_t)off < cap; l++) {
        tel_state_t st = telephony_state(l);
        off += snprintf(buf + off, cap - off,
            "<div class=\"card\"><h2>Line %d</h2>"
            "<div class=\"statusrow\"><span class=\"label\">State</span>"
            "<span class=\"pill %s\">%s</span></div>"
            "<div class=\"statusrow\"><span class=\"label\">Last number</span><span>%s</span></div>"
            "</div>",
            l, pill_class_for(st), telephony_state_name(st), telephony_last_number(l));
    }
    if (off > 0 && (size_t)off < cap) {
        off += snprintf(buf + off, cap - off, "</div>" /* .cards */);
    }
    if (off > 0 && (size_t)off < cap && log_snapshot) {
        off = append_log_panel(buf, cap, off, log_snapshot);
    }
    if (off > 0 && (size_t)off < cap) {
        off += snprintf(buf + off, cap - off,
            "<div class=\"toolbar sub\">"
            "<a href=\"/api/log\">raw log</a>"
            "<a href=\"/api/status\">status JSON</a>"
            "<a href=\"/api/recordings\">recordings JSON</a>"
            "</div></main></body></html>");
    }

    free(log_snapshot);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, buf, (off > 0) ? off : 0);
    free(buf);
    return ESP_OK;
}

/* Dedicated Recordings page (Pi's /recordings) -- same table that used to
 * be inlined on the dashboard, now its own tab so the dashboard stays
 * focused on live status. */
static esp_err_t recordings_page_handler(httpd_req_t *req)
{
    storage_entry_t entries[32];
    size_t n = storage_list(entries, 32);

    size_t cap = 2048 + n * 160;
    char *buf = malloc(cap);
    if (!buf) { httpd_resp_send_500(req); return ESP_FAIL; }

    int off = append_page_head(buf, cap, 0, "Recordings - Rotary Phone Gateway", "/recordings", false);
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "</header><main>"
        "<div class=\"card\"><h2>Recordings (%u)</h2>", (unsigned)n);

    if (n == 0) {
        off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
            "<div class=\"empty\">none yet</div>");
    } else {
        off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
            "<table><tr><th>Name</th><th>Size</th></tr>");
        for (size_t i = 0; i < n && off > 0 && (size_t)off < cap; i++) {
            off += snprintf(buf + off, cap - off,
                "<tr><td><a href=\"/media/%s\">%s</a></td><td>%u bytes</td></tr>",
                entries[i].name, entries[i].name, (unsigned)entries[i].size);
        }
        off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0, "</table>");
    }
    if (off > 0 && (size_t)off < cap) {
        off += snprintf(buf + off, cap - off,
            "</div>"
            "<p class=\"hint\">Storage: %u / %u bytes%s. Raw list: "
            "<a href=\"/api/recordings\">recordings JSON</a>.</p>"
            "</main></body></html>",
            (unsigned)storage_used_bytes(), (unsigned)storage_cap_bytes(),
            storage_using_sd() ? " (SD)" : " (internal flash)");
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, buf, (off > 0) ? off : 0);
    free(buf);
    return ESP_OK;
}

/* Placeholder -- no configurable settings wired up yet (WiFi creds are set
 * via the console's `wifi_set` for now). Real content lands once there's
 * something here worth exposing over HTTP. */
static esp_err_t settings_page_handler(httpd_req_t *req)
{
    char buf[1024];
    int off = append_page_head(buf, sizeof(buf), 0, "Settings - Rotary Phone Gateway", "/settings", false);
    off += snprintf(buf + off, ((size_t)off < sizeof(buf)) ? sizeof(buf) - off : 0,
        "</header><main>"
        "<div class=\"card\"><h2>Settings</h2>"
        "<p class=\"hint\">Not built yet. WiFi credentials can be set from the device "
        "console with <code>wifi_set &lt;ssid&gt; &lt;password&gt;</code> for now.</p>"
        "</div></main></body></html>");
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, buf, (off > 0) ? off : 0);
    return ESP_OK;
}

/* Lists dialplan.c's real table -- not a hand-copied duplicate -- and one
 * synthetic row for the 555xxxx VoIP prefix, which is matched directly in
 * dialplan_feed() rather than living in the table itself. Working codes get
 * the same green .pill.on badge the line-state cards use; anything
 * telephony.c still just stubs with a confirm tone gets .pill.off (already
 * styled muted/gray in the Pi CSS) plus a dimmed row so it reads as "not
 * live yet" at a glance, per your "grey it out" ask. */
static esp_err_t guide_handler(httpd_req_t *req)
{
    dialplan_entry_info_t entries[40];
    size_t n = dialplan_list(entries, 40);

    size_t cap = 4096 + n * 200;
    char *buf = malloc(cap);
    if (!buf) { httpd_resp_send_500(req); return ESP_FAIL; }

    int off = append_page_head(buf, cap, 0, "Dial Guide - Rotary Phone Gateway", "/guide", false);
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "</header><main>"
        "<div class=\"card\"><h2>Dial codes</h2>"
        "<table><tr><th>Code</th><th>Does</th><th>Status</th></tr>");

    for (size_t i = 0; i < n && off > 0 && (size_t)off < cap; i++) {
        off += snprintf(buf + off, cap - off,
            "<tr%s><td><code>%s</code></td><td>%s</td>"
            "<td><span class=\"pill %s\">%s</span></td></tr>",
            entries[i].implemented ? "" : " style=\"opacity:.55\"",
            entries[i].code, entries[i].desc,
            entries[i].implemented ? "on" : "off",
            entries[i].implemented ? "working" : "not yet");
    }
    if (off > 0 && (size_t)off < cap) {
        off += snprintf(buf + off, cap - off,
            "<tr style=\"opacity:.55\"><td><code>555xxxx</code></td>"
            "<td>VoIP dial (7 digits after 555)</td>"
            "<td><span class=\"pill off\">not yet</span></td></tr>"
            "</table></div>"
            "<p class=\"hint\">Source of truth: <code>components/dialplan/dialplan.c</code> -- "
            "this list is generated from the same table the phone actually matches against, "
            "so it can't drift out of sync with real behavior.</p>"
            "</main></body></html>");
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, buf, (off > 0) ? off : 0);
    free(buf);
    return ESP_OK;
}

/* Old route name -- keep working as a redirect in case it's bookmarked. */
static esp_err_t dialplan_redirect_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/guide");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t log_handler(httpd_req_t *req)
{
    size_t cap = 8200;
    char *buf = malloc(cap);
    if (!buf) { httpd_resp_send_500(req); return ESP_FAIL; }
    logbuf_snapshot(buf, cap);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, buf);
    free(buf);
    return ESP_OK;
}

static esp_err_t recordings_json_handler(httpd_req_t *req)
{
    storage_entry_t entries[32];
    size_t n = storage_list(entries, 32);
    cJSON *arr = cJSON_CreateArray();
    for (size_t i = 0; i < n; i++) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "name", entries[i].name);
        cJSON_AddNumberToObject(e, "size", (double)entries[i].size);
        cJSON_AddItemToArray(arr, e);
    }
    char *text = cJSON_PrintUnformatted(arr);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, text ? text : "[]");
    cJSON_Delete(arr);
    free(text);
    return ESP_OK;
}

static esp_err_t media_handler(httpd_req_t *req)
{
    const char *prefix = "/media/";
    if (strncmp(req->uri, prefix, strlen(prefix)) != 0) {
        httpd_resp_send_404(req);
        return ESP_OK;
    }
    const char *name = req->uri + strlen(prefix);

    storage_entry_t entries[32];
    size_t n = storage_list(entries, 32);
    size_t size = 0;
    bool found = false;
    for (size_t i = 0; i < n; i++) {
        if (strcmp(entries[i].name, name) == 0) { size = entries[i].size; found = true; break; }
    }
    if (!found) {
        httpd_resp_send_404(req);
        return ESP_OK;
    }

    char *buf = malloc(size);
    if (!buf) { httpd_resp_send_500(req); return ESP_FAIL; }
    size_t got = 0;
    if (storage_read(name, buf, size, &got) != ESP_OK) {
        free(buf);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    char disp[64];
    snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", name);
    httpd_resp_set_hdr(req, "Content-Disposition", disp);
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_send(req, buf, got);
    free(buf);
    return ESP_OK;
}

/* Upload path for host-generated prompt/clip WAVs (see tools/gen_prompt.py) --
 * PUT/POST the file body to /media/<name> and it lands directly in the
 * storage layer, same place `record`-produced clips live. No auth -- this
 * is a LAN bring-up tool, not a public deployment. */
#define MEDIA_UPLOAD_MAX_BYTES (2 * 1024 * 1024) /* generous for short prompt/clip WAVs */

static esp_err_t media_upload_handler(httpd_req_t *req)
{
    const char *prefix = "/media/";
    if (strncmp(req->uri, prefix, strlen(prefix)) != 0) {
        httpd_resp_send_404(req);
        return ESP_OK;
    }
    const char *name = req->uri + strlen(prefix);
    if (!name[0] || strlen(name) >= STORAGE_MAX_NAME) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing/too-long filename");
        return ESP_OK;
    }

    size_t total = req->content_len;
    if (total == 0 || total > MEDIA_UPLOAD_MAX_BYTES) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing/oversized Content-Length");
        return ESP_OK;
    }

    /* Streamed straight to flash a few KB at a time -- a whole-file malloc()
     * here doesn't work for real prompt clips (a few seconds of 48kHz 16-bit
     * mono is already a few hundred KB, more than fits alongside WiFi/httpd
     * in free heap). */
    storage_writer_t w;
    esp_err_t err = storage_write_begin(name, total, &w);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "upload '%s': storage_write_begin failed: %s", name, esp_err_to_name(err));
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    /* Heap, not stack -- the httpd worker task only has a 4KB stack. */
    size_t chunk_cap = 2048;
    char *chunk = malloc(chunk_cap);
    if (!chunk) {
        storage_write_abort(&w);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    size_t received = 0;
    while (received < total) {
        int r = httpd_req_recv(req, chunk, (total - received < chunk_cap) ? total - received : chunk_cap);
        if (r <= 0) {
            free(chunk);
            storage_write_abort(&w);
            if (r == HTTPD_SOCK_ERR_TIMEOUT) {
                httpd_resp_send_err(req, HTTPD_408_REQ_TIMEOUT, "upload timed out");
            } else {
                httpd_resp_send_500(req);
            }
            return ESP_FAIL;
        }
        if (storage_write_chunk(&w, chunk, (size_t)r) != ESP_OK) {
            free(chunk);
            storage_write_abort(&w);
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        received += (size_t)r;
    }
    free(chunk);
    storage_write_end(&w);

    ESP_LOGI(TAG, "uploaded '%s' (%u bytes)", name, (unsigned)received);
    httpd_resp_sendstr(req, "ok\n");
    return ESP_OK;
}

esp_err_t webui_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    /* Default is 8 -- we register 11 routes below (two share the media
     * path, one GET, one POST, counted separately). Past the cap,
     * httpd_register_uri_handler() just fails per-route, which used to go
     * unchecked below, silently dropping the last couple of routes. */
    config.max_uri_handlers = 16;

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }

    static const httpd_uri_t routes[] = {
        { .uri = "/",                .method = HTTP_GET, .handler = index_handler },
        { .uri = "/recordings",      .method = HTTP_GET, .handler = recordings_page_handler },
        { .uri = "/guide",           .method = HTTP_GET, .handler = guide_handler },
        { .uri = "/settings",        .method = HTTP_GET, .handler = settings_page_handler },
        { .uri = "/dialplan",        .method = HTTP_GET, .handler = dialplan_redirect_handler },
        { .uri = "/static/style.css",.method = HTTP_GET, .handler = static_css_handler },
        { .uri = "/api/status",      .method = HTTP_GET, .handler = status_json_handler },
        { .uri = "/api/log",         .method = HTTP_GET, .handler = log_handler },
        { .uri = "/api/recordings",  .method = HTTP_GET, .handler = recordings_json_handler },
        { .uri = "/media/*",         .method = HTTP_GET, .handler = media_handler },
        { .uri = "/media/*",         .method = HTTP_POST, .handler = media_upload_handler },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        esp_err_t rerr = httpd_register_uri_handler(server, &routes[i]);
        if (rerr != ESP_OK) {
            ESP_LOGE(TAG, "route %s (method %d) failed to register: %s",
                     routes[i].uri, routes[i].method, esp_err_to_name(rerr));
        }
    }
    ESP_LOGI(TAG, "web UI listening on port %d", config.server_port);
    return ESP_OK;
}

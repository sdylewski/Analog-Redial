#include "webui.h"
#include "sdkconfig.h"
#include "esp_http_server.h"
#include "cJSON.h"
#include "wifi.h"
#include "board.h"
#include "esp_idf_version.h"
#include "storage.h"
#include "logbuf.h"
#include "telephony.h"
#include "dialplan.h"
#include "voip.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <ctype.h>

static const char *TAG = "webui";

/* Human-readable size: "4.19 MB", "188.7 kB", "0 B" (decimal units, matching
 * how flash/card sizes are marketed). Writes into `out` and returns it so it
 * can be used inline as an snprintf argument. Caller supplies one buffer per
 * value shown (they're all live at once inside a single snprintf). */
static const char *fmt_bytes(uint64_t n, char *out, size_t outsz)
{
    if (n >= 1000000ULL)   snprintf(out, outsz, "%.2f MB", (double)n / 1e6);
    else if (n >= 1000ULL) snprintf(out, outsz, "%.1f kB", (double)n / 1e3);
    else                   snprintf(out, outsz, "%u B", (unsigned)n);
    return out;
}

/* Defined below (near guide_handler) -- forward-declared here so the
 * settings-tab renderers, which come first in this file, can reuse the same
 * dial-code-row markup instead of duplicating it. */
static int append_guide_row(char *buf, size_t cap, int off, const char *code,
                             const char *desc, bool implemented);

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

/* Removes ANSI colour escape sequences (ESC '[' ... final-byte) in place.
 * ESP-IDF's log macros wrap every line in colour codes when CONFIG_LOG_COLORS
 * is on; those are meaningless in HTML and render as little boxes, so strip
 * them before the line is parsed/displayed. */
static void strip_ansi(char *s)
{
    char *w = s;
    for (const char *r = s; *r; ) {
        if (*r == 0x1b) {           /* ESC */
            r++;
            if (*r == '[') {
                r++;
                while (*r && !(*r >= '@' && *r <= '~')) r++; /* CSI params */
                if (*r) r++;         /* the final byte (e.g. 'm') */
            }
            continue;
        }
        *w++ = *r++;
    }
    *w = '\0';
}

/* Parses one "X (12345) tag: rest of message" ESP_LOG line (X = level
 * letter) into .t/.k/.m spans matching the Pi UI's .logline structure. Log
 * content here is entirely our own ESP_LOG calls (not user-supplied text),
 * so this intentionally skips HTML-escaping -- fine for a LAN bring-up
 * tool, would need escaping if log messages could ever include user input. */
static int append_log_line(char *out, size_t out_len, int off, const char *raw)
{
    char line[256];
    strncpy(line, raw, sizeof(line) - 1);
    line[sizeof(line) - 1] = '\0';
    strip_ansi(line); /* drop ESC[...m colour codes before parsing */

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

    /* Board identity up top so a script polling every board's /api/status can
     * tell them apart (name), group by hardware, or key on the serial. */
    char name[32], hw[64], serial[16];
    board_name_get(name, sizeof(name));
    board_hardware_str(hw, sizeof(hw));
    board_serial_str(serial, sizeof(serial));
    cJSON_AddStringToObject(root, "board_name", name);
    cJSON_AddStringToObject(root, "hardware", hw);
    cJSON_AddStringToObject(root, "serial", serial);

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
    case TEL_REORDER:
    case TEL_RECORDING: return "busy";
    default: return "on"; /* dialtone/dialing/ringback/ringing/incall/playing */
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
    /* Board name leads the <title> and the header so a browser tab / glance
     * tells several boards on one LAN apart (that's the whole point of the
     * name). "Rotary Phone Gateway" drops to a subtitle. */
    char name[32];
    board_name_get(name, sizeof(name));
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<!doctype html><html><head><meta charset=\"utf-8\"><title>%s \xc2\xb7 %s</title>"
        "%s"
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
        "<link rel=\"stylesheet\" href=\"/static/style.css\"></head><body>"
        "<header><h1>%s <span class=\"dot\">Rotary Phone Gateway</span></h1><nav>",
        name, page_title, auto_refresh ? "<meta http-equiv=\"refresh\" content=\"5\">" : "",
        name);
    for (size_t i = 0; i < NAV_TABS_N; i++) {
        bool active = strcmp(NAV_TABS[i].href, active_href) == 0;
        off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
            "<a%s href=\"%s\">%s</a>", active ? " class=\"active\"" : "",
            NAV_TABS[i].href, NAV_TABS[i].label);
    }
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0, "</nav>");
    return off;
}

/* ---- WiFi provisioning (setup SoftAP) UI ---- */

static void reboot_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1500)); /* let the HTTP response flush first */
    esp_restart();
}

/* Setup page served on the SoftAP: a plain SSID/password form. Kept minimal
 * and self-contained (its own <form>, the shared stylesheet) so it works
 * before any network config exists. */
static esp_err_t wifi_setup_page(httpd_req_t *req)
{
    static const char *page =
        "<!doctype html><html><head><meta charset=\"utf-8\">"
        "<title>Rotary Phone \xc2\xb7 WiFi setup</title>"
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
        "<link rel=\"stylesheet\" href=\"/static/style.css\"></head><body>"
        "<header><h1>Rotary Phone <span class=\"dot\">WiFi setup</span></h1></header>"
        "<main class=\"auth-page\"><div class=\"auth-card\">"
        "<h1>Connect to your WiFi</h1>"
        "<p class=\"hint\">Enter your home network so the phone can join it. "
        "It'll reboot and connect; this setup network then disappears.</p>"
        "<form method=\"POST\" action=\"/wifi_set\" class=\"settings-form\">"
        "<label>Network name (SSID) <input name=\"ssid\" required autofocus></label>"
        "<label>Password <input type=\"password\" name=\"password\" "
        "placeholder=\"leave blank for an open network\"></label>"
        "<div class=\"toolbar sub\"><button type=\"submit\" class=\"primary\">"
        "Save &amp; connect</button></div>"
        "</form></div></main></body></html>";

    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, page);
    return ESP_OK;
}

/* Catch-all for the captive portal: any unmatched URL while provisioning
 * bounces to the setup page, which makes the OS "sign in to network" sheet
 * appear. Registered last so real routes match first. Outside provisioning
 * it's just a normal 404. */
static esp_err_t catchall_handler(httpd_req_t *req)
{
    if (wifi_is_provisioning()) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }
    httpd_resp_send_404(req);
    return ESP_OK;
}

static esp_err_t index_handler(httpd_req_t *req)
{
    if (wifi_is_provisioning()) return wifi_setup_page(req);

    char ip[16];
    wifi_get_ip_str(ip, sizeof(ip));
    int64_t uptime_s = esp_timer_get_time() / 1000000;
    char hb[16], su[16], sc[16];

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
        "<div class=\"statusrow\"><span class=\"label\">Free heap</span><span>%s</span></div>"
        "<div class=\"statusrow\"><span class=\"label\">Storage used</span><span>%s / %s%s</span></div>"
        "</div>",
        wifi_is_connected() ? "connected" : "disconnected",
        wifi_is_connected() ? " \xc2\xb7 " : "", wifi_is_connected() ? ip : "",
        wifi_get_ssid()[0] ? wifi_get_ssid() : "(not configured)", wifi_rssi(),
        ip, (long long)uptime_s,
        fmt_bytes(esp_get_free_heap_size(), hb, sizeof(hb)),
        fmt_bytes(storage_used_bytes(), su, sizeof(su)),
        fmt_bytes(storage_cap_bytes(), sc, sizeof(sc)),
        storage_using_sd() ? " (SD)" : " (internal flash)");

    for (int l = 0; l < CONFIG_BOARD_NUM_LINES && off > 0 && (size_t)off < cap; l++) {
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

/* A "recording" is something the user recorded from a handset (211 "leave a
 * message" archives, named rec_*.wav). Everything else in the storage
 * partition -- operator.wav, clip_*.wav fun clips, cb_*.wav callback temps --
 * is a built-in/uploaded asset, managed under Settings, not shown here. */
static bool is_user_recording(const char *name)
{
    return strncmp(name, "rec_", 4) == 0;
}

/* Dedicated Recordings page (Pi's /recordings): only user-recorded messages,
 * so it isn't cluttered with the operator/clip assets that live in the same
 * flat storage partition. Manage those under Settings -> Storage files. */
static esp_err_t recordings_page_handler(httpd_req_t *req)
{
    storage_entry_t entries[32];
    size_t n = storage_list(entries, 32);
    size_t shown = 0;
    for (size_t i = 0; i < n; i++) if (is_user_recording(entries[i].name)) shown++;

    size_t cap = 2048 + n * 160;
    char *buf = malloc(cap);
    if (!buf) { httpd_resp_send_500(req); return ESP_FAIL; }

    int off = append_page_head(buf, cap, 0, "Recordings - Rotary Phone Gateway", "/recordings", false);
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "</header><main>"
        "<div class=\"card\"><h2>Recordings (%u)</h2>"
        "<p class=\"hint\">Messages left with dial code 211. Built-in audio "
        "(operator, clips) is under <a href=\"/settings\">Settings</a>.</p>",
        (unsigned)shown);

    if (shown == 0) {
        off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
            "<div class=\"empty\">none yet \xe2\x80\x94 dial 211, leave a message, hang up</div>");
    } else {
        off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
            "<table><tr><th>Name</th><th>Size</th></tr>");
        for (size_t i = 0; i < n && off > 0 && (size_t)off < cap; i++) {
            if (!is_user_recording(entries[i].name)) continue;
            char sz[16];
            off += snprintf(buf + off, cap - off,
                "<tr><td><a href=\"/media/%s\">%s</a></td><td>%s</td></tr>",
                entries[i].name, entries[i].name, fmt_bytes(entries[i].size, sz, sizeof(sz)));
        }
        off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0, "</table>");
    }
    if (off > 0 && (size_t)off < cap) {
        char su[16], sc[16];
        off += snprintf(buf + off, cap - off,
            "</div>"
            "<p class=\"hint\">Storage: %s / %s%s. Raw list: "
            "<a href=\"/api/recordings\">recordings JSON</a>.</p>"
            "</main></body></html>",
            fmt_bytes(storage_used_bytes(), su, sizeof(su)),
            fmt_bytes(storage_cap_bytes(), sc, sizeof(sc)),
            storage_using_sd() ? " (SD)" : " (internal flash)");
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, buf, (off > 0) ? off : 0);
    free(buf);
    return ESP_OK;
}

/* Tab set mirrors the Pi's settings.html exactly (General / Operator / VOIP /
 * Inter-phone / Record message / Record & callback / Hardware) so the shape
 * previews where each section will land, even though most of them are still
 * placeholders here -- radio, recording, and VoIP aren't wired up on the
 * ESP32 yet (see ESP32_PORT_PLAN.md). Server-rendered via ?tab=, not client
 * JS: every other page in this component is a plain server-rendered form,
 * and a full reload per tab click keeps this page consistent with that
 * instead of introducing the only script tag in the whole UI. */
typedef struct { const char *id; const char *label; } settings_tab_t;
static const settings_tab_t SETTINGS_TABS[] = {
    { "general",    "General" },
    { "operator",   "Operator" },
    { "voip",       "VOIP" },
#if CONFIG_BOARD_NUM_LINES > 1
    { "interphone", "Inter-phone" }, /* 2-line only: no other line to call on a 1-line build */
#endif
    { "record",     "Record message" },
    { "callback",   "Record & callback" },
    { "hardware",   "Hardware" },
};
#define SETTINGS_TABS_N (sizeof(SETTINGS_TABS) / sizeof(SETTINGS_TABS[0]))

static int append_settings_tabs(char *buf, size_t cap, int off, const char *active)
{
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<nav class=\"settings-tabs\">");
    for (size_t i = 0; i < SETTINGS_TABS_N; i++) {
        bool is_active = strcmp(SETTINGS_TABS[i].id, active) == 0;
        off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
            "<a class=\"tab-btn%s\" href=\"/settings?tab=%s\">%s</a>",
            is_active ? " active" : "", SETTINGS_TABS[i].id, SETTINGS_TABS[i].label);
    }
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0, "</nav>");
    return off;
}

static void mode_set_url(char *out, size_t out_len, const char *code, int line)
{
    if (line >= 0) snprintf(out, out_len, "/mode_set/%s/%d", code, line);
    else snprintf(out, out_len, "/mode_set/%s", code);
}

/* One card: a live pill for the mode's current value plus the two buttons
 * that set it explicitly. The phone's own dial codes (931/932, 921/922)
 * always set state outright rather than toggling it, so these buttons
 * mirror that instead of offering a single flip. */
static int append_mode_card(char *buf, size_t cap, int off, const char *title,
                             const char *pill_class, const char *state_label,
                             const char *opt_a_label, const char *opt_a_code,
                             const char *opt_b_label, const char *opt_b_code, int line)
{
    char url_a[32], url_b[32];
    mode_set_url(url_a, sizeof(url_a), opt_a_code, line);
    mode_set_url(url_b, sizeof(url_b), opt_b_code, line);
    return off + snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<div class=\"card\"><h2>%s</h2>"
        "<div class=\"statusrow\"><span class=\"label\">Current</span>"
        "<span class=\"pill %s\">%s</span></div>"
        "<div class=\"toolbar sub\">"
        "<form method=\"POST\" action=\"%s\" style=\"display:inline\">"
        "<button type=\"submit\">%s</button></form>"
        "<form method=\"POST\" action=\"%s\" style=\"display:inline\">"
        "<button type=\"submit\">%s</button></form>"
        "</div></div>",
        title, pill_class, state_label, url_a, opt_a_label, url_b, opt_b_label);
}

static int append_general_tab(char *buf, size_t cap, int off)
{
    char name[32], host[64];
    board_name_get(name, sizeof(name));
    board_hostname(host, sizeof(host));
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<div class=\"card\"><h2>Board name</h2>"
        "<form method=\"POST\" action=\"/board_name\" class=\"settings-form\">"
        "<label>Name <input name=\"name\" value=\"%s\" maxlength=\"31\" required></label>"
        "<div class=\"toolbar sub\"><button type=\"submit\" class=\"primary\">Save name</button></div>"
        "</form>"
        "<p class=\"hint\">Labels this board in the UI and logs when you run several on one "
        "network. Also sets the mDNS address: reachable as <code>%s.local</code>. "
        "Hardware type and serial are on the <a href=\"/settings?tab=hardware\">Hardware</a> tab.</p>"
        "</div>",
        name, host);

    dialplan_modes_t *m = dialplan_modes();
    off = append_mode_card(buf, cap, off, "Phone mode",
        m->call_only ? "off" : "on", m->call_only ? "Call-only" : "Play mode",
        "Set play mode", "931", "Set call-only mode", "932", -1);
    off = append_mode_card(buf, cap, off, "Digit readback",
        m->readback_digits ? "on" : "off", m->readback_digits ? "On" : "Off",
        "Turn off", "921", "Turn on", "922", -1);
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<p class=\"hint\">Both persist to flash (NVS) and survive a reboot.</p>");

    char lang[8];
    board_language_get(lang, sizeof(lang));
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<div class=\"card\"><h2>Language</h2>"
        "<div class=\"statusrow\"><span class=\"label\">Current</span><span>%s</span></div>"
        "<form method=\"POST\" action=\"/language_set\" class=\"settings-form\">"
        "<label>Code (e.g. en, it, es) <input name=\"lang\" value=\"%s\" maxlength=\"7\" required></label>"
        "<div class=\"toolbar sub\"><button type=\"submit\" class=\"primary\">Save language</button></div>"
        "</form>"
        "<p class=\"hint\">One language for the whole phone, set here rather than switched in "
        "use (no 901/902 dial codes) -- generate that language's prompt/number audio during "
        "setup with <code>gen_prompt.py</code>, then leave it. No prompt audio exists yet to "
        "actually be affected by this.</p></div>",
        lang, lang);

    char ssid[33];
    strncpy(ssid, wifi_get_ssid(), sizeof(ssid) - 1);
    ssid[sizeof(ssid) - 1] = '\0';
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<div class=\"card\"><h2>WiFi</h2>"
        "<div class=\"statusrow\"><span class=\"label\">Current</span><span>%s</span></div>"
        "<form method=\"POST\" action=\"/wifi_set\" class=\"settings-form\">"
        "<label>SSID <input name=\"ssid\" value=\"%s\" required></label>"
        "<label>Password <input type=\"password\" name=\"password\" "
        "placeholder=\"leave blank for an open network\"></label>"
        "<div class=\"toolbar sub\"><button type=\"submit\" class=\"primary\">Connect</button></div>"
        "</form>"
        "<p class=\"hint\">Saved to flash (NVS); the device reconnects immediately, no reboot needed.</p>"
        "</div>",
        ssid[0] ? ssid : "(not configured)", ssid);
    return off;
}

static int append_operator_tab(char *buf, size_t cap, int off,
                                dialplan_entry_info_t *entries, size_t n)
{
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<div class=\"card\"><h2>Operator help</h2>");
    for (size_t i = 0; i < n; i++) {
        if (strcmp(entries[i].code, "0") != 0) continue;
        off = append_guide_row(buf, cap, off, entries[i].code, entries[i].desc,
                                entries[i].implemented);
    }
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<p class=\"hint\">No on-device TTS or script editor yet -- upload a pre-rendered "
        "<code>operator.wav</code> with <code>firmware/bringup/gen_prompt.py</code> "
        "(see Storage files below) to change what plays.</p></div>");
    return off;
}

static int append_voip_tab(char *buf, size_t cap, int off)
{
    bool r0 = voip_is_registered(0), r1 = voip_is_registered(1);
    return off + snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<div class=\"card\"><h2>VOIP</h2>"
        "<div class=\"statusrow\"><span class=\"label\">SIP server</span><span>%s</span></div>"
        "<div class=\"statusrow\"><span class=\"label\">Line 1</span>"
        "<span class=\"pill %s\">%s</span></div>"
        "<div class=\"statusrow\"><span class=\"label\">Line 2</span>"
        "<span class=\"pill %s\">%s</span></div>"
        "<p class=\"hint\">Embedded SIP/RTP (G.711 &micro;-law). Dial <code>555</code> + a "
        "4-digit extension to place a call. Configure from the device console: "
        "<code>sip_set &lt;server&gt; &lt;user0&gt; &lt;pass0&gt; [user1] [pass1]</code>, "
        "then reboot. (A web form for this is a TODO.)</p></div>",
        voip_server()[0] ? voip_server() : "(not set)",
        r0 ? "on" : "off", r0 ? "registered" : "not registered",
        r1 ? "on" : "off", r1 ? "registered" : "not registered");
}

static int append_interphone_tab(char *buf, size_t cap, int off,
                                  dialplan_entry_info_t *entries, size_t n)
{
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<div class=\"card\"><h2>Inter-phone calls</h2>");
    for (size_t i = 0; i < n; i++) {
        if (entries[i].code[0] != '3') continue;
        off = append_guide_row(buf, cap, off, entries[i].code, entries[i].desc,
                                entries[i].implemented);
    }
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<p class=\"hint\">Codes are fixed in firmware "
        "(<code>components/dialplan/dialplan.c</code>) -- not yet editable from here, "
        "unlike the Pi's per-line dial-code editor.</p></div>");
    return off;
}

static int append_record_tab(char *buf, size_t cap, int off,
                              dialplan_entry_info_t *entries, size_t n)
{
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<div class=\"card\"><h2>Record message</h2>");
    for (size_t i = 0; i < n; i++) {
        if (strcmp(entries[i].code, "211") != 0) continue;
        off = append_guide_row(buf, cap, off, entries[i].code, entries[i].desc,
                                entries[i].implemented);
    }
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<p class=\"hint\">Dial 211, speak, then hang up (or pause ~2 s / hit 30 s). The message "
        "is saved as an 8 kHz mono WAV in <a href=\"/recordings\">Recordings</a> / Storage below. "
        "Editable max-length and warning beeps aren't exposed here yet.</p>"
        "</div>");
    return off;
}

static int append_callback_tab(char *buf, size_t cap, int off,
                                dialplan_entry_info_t *entries, size_t n)
{
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<div class=\"card\"><h2>Record &amp; call back</h2>");
    for (size_t i = 0; i < n; i++) {
        if (entries[i].code[0] != '2' || strcmp(entries[i].code, "211") == 0) continue;
        off = append_guide_row(buf, cap, off, entries[i].code, entries[i].desc,
                                entries[i].implemented);
    }
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<p class=\"hint\">Dial one of these, speak, then pause (or wait out 12 s) and it plays "
        "straight back on the handset. High/low shift pitch; robot (Dalek) plays flat for now "
        "(ring-mod TODO). This is the ESP32 take on the Pi's hang-up-and-ring-back callback.</p></div>");
    return off;
}

static int append_hardware_tab(char *buf, size_t cap, int off)
{
    char hw[64], serial[16];
    board_hardware_str(hw, sizeof(hw));
    board_serial_str(serial, sizeof(serial));
    board_codec_t detected = board_detected_codec();

    uint32_t flash_bytes = 0;
    esp_flash_get_size(NULL, &flash_bytes);
    size_t ram_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    size_t ram_free  = esp_get_free_heap_size();
    size_t ram_used  = (ram_total > ram_free) ? ram_total - ram_free : 0;
    size_t st_cap  = storage_cap_bytes();
    size_t st_used = storage_used_bytes();
    size_t st_free = (st_cap > st_used) ? st_cap - st_used : 0;

    char fl[16], ru[16], rf[16], su[16], sf[16];
    return off + snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<div class=\"card\"><h2>Hardware</h2>"
        "<div class=\"statusrow\"><span class=\"label\">Type</span><span>%s</span></div>"
        "<div class=\"statusrow\"><span class=\"label\">Codec (detected)</span><span>%s</span></div>"
        "<div class=\"statusrow\"><span class=\"label\">Serial (chip MAC)</span>"
        "<span><code>%s</code></span></div>"
        "<div class=\"statusrow\"><span class=\"label\">Firmware</span><span>ESP-IDF %s</span></div>"
        "<div class=\"statusrow\"><span class=\"label\">Flash (total)</span><span>%s</span></div>"
        "<div class=\"statusrow\"><span class=\"label\">RAM used / free</span><span>%s / %s</span></div>"
        "<div class=\"statusrow\"><span class=\"label\">Storage used / free</span>"
        "<span>%s / %s%s</span></div>"
        "<p class=\"hint\">Type is fixed at build time (<code>CONFIG_BOARD_TYPE_*</code>) since "
        "GPIO routing is per-PCB; the detected codec is probed live on the I2C bus and warns in "
        "the log if it disagrees. Serial is the factory MAC -- unique per chip, survives reflash. "
        "Flash is the whole chip; Storage is the file partition (recordings + uploaded clips); "
        "RAM is live internal heap. Uptime/WiFi are on the <a href=\"/\">Status</a> page.</p>"
        "</div>",
        hw, board_codec_name(detected), serial, esp_get_idf_version(),
        fmt_bytes(flash_bytes, fl, sizeof(fl)),
        fmt_bytes(ram_used, ru, sizeof(ru)), fmt_bytes(ram_free, rf, sizeof(rf)),
        fmt_bytes(st_used, su, sizeof(su)), fmt_bytes(st_free, sf, sizeof(sf)),
        storage_using_sd() ? " (SD)" : "");
}

/* Storage file manager -- lists every file in the storage layer (recordings,
 * uploaded prompt/clip WAVs, anything else that landed there) with size,
 * download, and delete. There's no type distinction in storage.c itself --
 * it's all just files in the same SPIFFS-backed partition -- so this stays
 * outside the tabs (clip uploads, operator.wav, and recordings all land
 * here together; no single tab owns it). */
static int append_storage_section(char *buf, size_t cap, int off)
{
    storage_entry_t entries[32];
    size_t n = storage_list(entries, 32);
    char ip[16];
    wifi_get_ip_str(ip, sizeof(ip));

    char su[16], sc[16];
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<div class=\"card\" style=\"margin-top:16px\"><h2>Storage files (%u)</h2>"
        "<p class=\"hint\">%s / %s used. Everything in the storage partition -- recordings, "
        "uploaded prompt/clip WAVs, anything else. Upload with <code>curl -X POST "
        "--data-binary @file.wav http://%s/media/&lt;name&gt;</code> or "
        "<code>firmware/bringup/gen_prompt.py</code>.</p>",
        (unsigned)n, fmt_bytes(storage_used_bytes(), su, sizeof(su)),
        fmt_bytes(storage_cap_bytes(), sc, sizeof(sc)), ip);

    if (n == 0) {
        off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
            "<div class=\"empty\">none yet</div>");
    } else {
        off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
            "<table><tr><th>Name</th><th>Size</th><th></th><th></th></tr>");
        for (size_t i = 0; i < n && off > 0 && (size_t)off < cap; i++) {
            char sz[16];
            off += snprintf(buf + off, cap - off,
                "<tr><td><code>%s</code></td><td>%s</td>"
                "<td><a href=\"/media/%s\">download</a></td>"
                "<td><form method=\"POST\" action=\"/media_delete/%s\" "
                "onsubmit=\"return confirm('Delete %s?')\" style=\"display:inline\">"
                "<button type=\"submit\">delete</button></form></td></tr>",
                entries[i].name, fmt_bytes(entries[i].size, sz, sizeof(sz)),
                entries[i].name, entries[i].name, entries[i].name);
        }
        off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0, "</table>");
    }
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0, "</div>");
    return off;
}

static esp_err_t settings_page_handler(httpd_req_t *req)
{
    char tab[16] = "general";
    char query[64];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "tab", tab, sizeof(tab));
    }
    bool known_tab = false;
    for (size_t i = 0; i < SETTINGS_TABS_N; i++) {
        if (strcmp(SETTINGS_TABS[i].id, tab) == 0) { known_tab = true; break; }
    }
    if (!known_tab) strncpy(tab, "general", sizeof(tab));

    dialplan_entry_info_t entries[40];
    size_t n = dialplan_list(entries, 40);

    /* Sized for the busiest tab (all n entries as guide rows) plus the storage
     * section (up to 32 files, listed there). Don't storage_list() here just
     * to count -- that's a deep SPIFFS+flash walk, and doing it on top of this
     * handler's stack arrays is what overflowed the httpd task's stack. */
    size_t cap = 6144 + n * 220 + 32 * 220;
    char *buf = malloc(cap);
    if (!buf) { httpd_resp_send_500(req); return ESP_FAIL; }

    int off = append_page_head(buf, cap, 0, "Settings - Rotary Phone Gateway", "/settings", false);
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "</header><main class=\"settings-main\">");
    off = append_settings_tabs(buf, cap, off, tab);
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0, "<div class=\"cards\">");

    if (strcmp(tab, "general") == 0) off = append_general_tab(buf, cap, off);
    else if (strcmp(tab, "operator") == 0) off = append_operator_tab(buf, cap, off, entries, n);
    else if (strcmp(tab, "voip") == 0) off = append_voip_tab(buf, cap, off);
    else if (strcmp(tab, "interphone") == 0) off = append_interphone_tab(buf, cap, off, entries, n);
    else if (strcmp(tab, "record") == 0) off = append_record_tab(buf, cap, off, entries, n);
    else if (strcmp(tab, "callback") == 0) off = append_callback_tab(buf, cap, off, entries, n);
    else off = append_hardware_tab(buf, cap, off);

    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0, "</div>" /* .cards */);
    off = append_storage_section(buf, cap, off);

    if (off > 0 && (size_t)off < cap) {
        off += snprintf(buf + off, cap - off, "</main></body></html>");
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, buf, (off > 0) ? off : 0);
    free(buf);
    return ESP_OK;
}

/* POST /mode_set/<code>[/<line>] -- web-UI equivalent of dialing a 9xx mode
 * code, routed through the same TABLE dialplan_feed() matches against (see
 * dialplan_set_mode_by_code()) so the buttons can't drift from what dialing
 * the code actually does. No current mode is per-line, so <line> is unused,
 * but the URL shape stays in case one ever is. */
static esp_err_t mode_set_handler(httpd_req_t *req)
{
    const char *prefix = "/mode_set/";
    const char *rest = req->uri + strlen(prefix);
    char code[8] = {0};
    int line = -1;

    const char *slash = strchr(rest, '/');
    if (slash) {
        size_t clen = (size_t)(slash - rest);
        if (clen >= sizeof(code)) clen = sizeof(code) - 1;
        memcpy(code, rest, clen);
        code[clen] = '\0';
        line = atoi(slash + 1);
    } else {
        strncpy(code, rest, sizeof(code) - 1);
    }

    if (!dialplan_set_mode_by_code(code, line)) {
        ESP_LOGW(TAG, "mode_set: unknown or non-mode code '%s'", code);
    }

    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/settings");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* Minimal application/x-www-form-urlencoded decoder -- '+' and %XX only.
 * Good enough for the couple of short text fields this UI posts (WiFi
 * SSID/password); not a general MIME parser. */
static void url_decode(char *s)
{
    char *o = s;
    while (*s) {
        if (*s == '+') { *o++ = ' '; s++; }
        else if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char hex[3] = { s[1], s[2], 0 };
            *o++ = (char)strtol(hex, NULL, 16);
            s += 3;
        } else {
            *o++ = *s++;
        }
    }
    *o = '\0';
}

static bool form_field(const char *body, const char *key, char *out, size_t out_len)
{
    size_t klen = strlen(key);
    const char *p = body;
    while (p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            p += klen + 1;
            const char *amp = strchr(p, '&');
            size_t vlen = amp ? (size_t)(amp - p) : strlen(p);
            if (vlen >= out_len) vlen = out_len - 1;
            memcpy(out, p, vlen);
            out[vlen] = '\0';
            url_decode(out);
            return true;
        }
        p = strchr(p, '&');
        if (p) p++;
    }
    return false;
}

/* POST /wifi_set -- form body ssid=...&password=.... Blank password means
 * an open network (matches do_connect()'s own authmode choice in wifi.c),
 * not "keep the old one". */
#define WIFI_FORM_MAX_BODY 256

static esp_err_t wifi_set_handler(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len >= WIFI_FORM_MAX_BODY) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad form body");
        return ESP_OK;
    }
    char body[WIFI_FORM_MAX_BODY];
    int r = httpd_req_recv(req, body, req->content_len);
    if (r <= 0) { httpd_resp_send_500(req); return ESP_FAIL; }
    body[r] = '\0';

    char ssid[33] = {0}, pass[65] = {0};
    form_field(body, "ssid", ssid, sizeof(ssid));
    form_field(body, "password", pass, sizeof(pass));

    if (!ssid[0]) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid required");
        return ESP_OK;
    }

    if (wifi_is_provisioning()) {
        /* From the setup AP: we're not on the target network, so save the
         * creds and reboot into station mode rather than trying to connect
         * live (which would drop the AP the phone is still using). */
        esp_err_t err = wifi_save_credentials(ssid, pass);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "wifi_save_credentials failed: %s", esp_err_to_name(err));
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        httpd_resp_set_type(req, "text/html");
        httpd_resp_sendstr(req,
            "<!doctype html><meta charset=\"utf-8\">"
            "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
            "<link rel=\"stylesheet\" href=\"/static/style.css\">"
            "<main class=\"auth-page\"><div class=\"auth-card\">"
            "<h1>Saved \xe2\x80\x94 connecting\xe2\x80\xa6</h1>"
            "<p class=\"hint\">The phone is rebooting to join your network. This setup "
            "network will disappear. Find it back at its <code>.local</code> name or on "
            "your router's device list.</p></div></main>");
        xTaskCreate(reboot_task, "reboot", 2048, NULL, tskIDLE_PRIORITY + 1, NULL);
        return ESP_OK;
    }

    esp_err_t err = wifi_set_credentials(ssid, pass);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wifi_set_credentials failed: %s", esp_err_to_name(err));
    }

    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/settings");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* POST /board_name -- form body name=.... Persists the name (board_name_set)
 * and re-pushes it to mDNS live (wifi_apply_hostname) so the <name>.local
 * address updates without a reboot. */
static esp_err_t board_name_handler(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len >= WIFI_FORM_MAX_BODY) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad form body");
        return ESP_OK;
    }
    char body[WIFI_FORM_MAX_BODY];
    int r = httpd_req_recv(req, body, req->content_len);
    if (r <= 0) { httpd_resp_send_500(req); return ESP_FAIL; }
    body[r] = '\0';

    char name[32] = {0};
    form_field(body, "name", name, sizeof(name));
    if (!name[0]) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "name required");
        return ESP_OK;
    }
    if (board_name_set(name) == ESP_OK) {
        wifi_apply_hostname();
    }

    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/settings?tab=general");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* POST /language_set -- form body lang=.... Config-time only: no dial code
 * changes this, no per-line flag -- one language for the whole phone. */
static esp_err_t language_set_handler(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len >= WIFI_FORM_MAX_BODY) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad form body");
        return ESP_OK;
    }
    char body[WIFI_FORM_MAX_BODY];
    int r = httpd_req_recv(req, body, req->content_len);
    if (r <= 0) { httpd_resp_send_500(req); return ESP_FAIL; }
    body[r] = '\0';

    char lang[8] = {0};
    form_field(body, "lang", lang, sizeof(lang));
    if (!lang[0]) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "lang required");
        return ESP_OK;
    }
    board_language_set(lang);

    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/settings?tab=general");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* POST /media_delete/<name> -- plain HTML-form-friendly delete (browsers
 * can't send DELETE from a <form>), redirects back to /settings so the page
 * just refreshes with the file gone. */
static esp_err_t media_delete_handler(httpd_req_t *req)
{
    const char *prefix = "/media_delete/";
    if (strncmp(req->uri, prefix, strlen(prefix)) != 0) {
        httpd_resp_send_404(req);
        return ESP_OK;
    }
    const char *name = req->uri + strlen(prefix);

    esp_err_t err = storage_delete(name);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "delete '%s' failed: %s", name, esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "deleted '%s' via web UI", name);
    }

    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/settings");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* Cards grouped by code prefix -- matches the layout of the Pi's
 * web/templates/guide.html (4xx / 2xx / call-other-phone / 8xx / 9xx cards)
 * instead of one flat table. Grouping is by first digit, not a hand-kept
 * category field, so a new dialplan.c entry lands in the right card purely
 * by its code -- there's still only one table to keep in sync. */
typedef struct { char prefix; const char *heading; } guide_cat_t;
static const guide_cat_t GUIDE_CATS[] = {
    { '4', "4xx -- silly audio" },
    { '2', "2xx -- calling" },
    { '3', "Call other phone" },
    { '8', "8xx -- radio" },
    { '9', "9xx -- settings" },
};
#define GUIDE_CATS_N (sizeof(GUIDE_CATS) / sizeof(GUIDE_CATS[0]))

/* "401" -> "4-0-1", matching how the Pi guide and DIAL_CODES.md write codes. */
static void hyphenate_code(const char *code, char *out, size_t outsz)
{
    size_t o = 0;
    for (size_t i = 0; code[i] != '\0' && o + 2 < outsz; i++) {
        if (i) out[o++] = '-';
        out[o++] = code[i];
    }
    out[o] = '\0';
}

static int append_guide_row(char *buf, size_t cap, int off, const char *code,
                             const char *desc, bool implemented)
{
    char hy[24];
    hyphenate_code(code, hy, sizeof(hy));
    return off + snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<div class=\"code-row\"><span class=\"code\">%s</span>"
        "<span class=\"desc\">%s</span>"
        "<span class=\"pill %s\">%s</span></div>",
        hy, desc, implemented ? "on" : "off", implemented ? "working" : "not yet");
}

static esp_err_t guide_handler(httpd_req_t *req)
{
    dialplan_entry_info_t entries[40];
    size_t n = dialplan_list(entries, 40);

    size_t cap = 4096 + n * 220;
    char *buf = malloc(cap);
    if (!buf) { httpd_resp_send_500(req); return ESP_FAIL; }

    int off = append_page_head(buf, cap, 0, "Dial Guide - Rotary Phone Gateway", "/guide", false);
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "</header><main><div class=\"cards\">");

    for (size_t g = 0; g < GUIDE_CATS_N && off > 0 && (size_t)off < cap; g++) {
        /* Skip a category with no live codes (e.g. "Call other phone" on a
         * single-line build, where the 3xx entries are compiled out). */
        size_t in_cat = 0;
        for (size_t i = 0; i < n; i++) if (entries[i].code[0] == GUIDE_CATS[g].prefix) in_cat++;
        if (in_cat == 0) continue;

        off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
            "<div class=\"card\"><h2>%s</h2>", GUIDE_CATS[g].heading);
        for (size_t i = 0; i < n && (size_t)off < cap; i++) {
            if (entries[i].code[0] != GUIDE_CATS[g].prefix) continue;
            off = append_guide_row(buf, cap, off, entries[i].code, entries[i].desc,
                                    entries[i].implemented);
        }
        off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0, "</div>");
    }

    /* Operator ("0") doesn't share a prefix with anything else in GUIDE_CATS,
     * so it gets its own card, still pulled live from the table. */
    for (size_t i = 0; i < n && off > 0 && (size_t)off < cap; i++) {
        if (strcmp(entries[i].code, "0") != 0) continue;
        off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
            "<div class=\"card\"><h2>Operator</h2>");
        off = append_guide_row(buf, cap, off, entries[i].code, entries[i].desc,
                                entries[i].implemented);
        off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0, "</div>");
    }

    if (off > 0 && (size_t)off < cap) {
        /* Synthetic row: 555xxxx is matched directly in dialplan_feed(),
         * not a TABLE entry, so it isn't in dialplan_list()'s output. */
        off += snprintf(buf + off, cap - off,
            "<div class=\"card\"><h2>VoIP</h2>"
            "<div class=\"code-row\"><span class=\"code\">5-5-5-x-x-x-x</span>"
            "<span class=\"desc\">7-digit extension</span>"
            "<span class=\"pill off\">not yet</span></div></div>"
            "</div>"
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
    /* Default is 8 -- we register 17 routes below (two share the media
     * path, one GET, one POST, counted separately). Past the cap,
     * httpd_register_uri_handler() just fails per-route, which used to go
     * unchecked below, silently dropping the last couple of routes. */
    config.max_uri_handlers = 20;
    /* Default httpd task stack is 4 KB. Our page handlers stack a few
     * storage_entry_t[32]/dialplan_entry_info_t[40] arrays and then descend
     * into SPIFFS readdir -> esp_flash_read, which is itself stack-hungry;
     * that combination overflowed the 4 KB guard on the Settings page. */
    config.stack_size = 8192;

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
        { .uri = "/media_delete/*",  .method = HTTP_POST, .handler = media_delete_handler },
        { .uri = "/mode_set/*",      .method = HTTP_POST, .handler = mode_set_handler },
        { .uri = "/wifi_set",        .method = HTTP_POST, .handler = wifi_set_handler },
        { .uri = "/board_name",      .method = HTTP_POST, .handler = board_name_handler },
        { .uri = "/language_set",    .method = HTTP_POST, .handler = language_set_handler },
        /* Catch-all LAST: captive-portal redirect while provisioning, plain
         * 404 otherwise. The wildcard matcher tries handlers in registration
         * order, so every specific route above still wins. */
        { .uri = "/*",               .method = HTTP_GET, .handler = catchall_handler },
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

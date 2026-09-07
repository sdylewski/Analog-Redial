#include "webui.h"
#include "sdkconfig.h"
#include "esp_http_server.h"
#include "cJSON.h"
#include "wifi.h"
#include "board.h"
#include "board_config.h"
#include "esp_idf_version.h"
#include "storage.h"
#include "logbuf.h"
#include "activity.h"
#include "telephony.h"
#include "prompts.h"
#include "dialplan.h"
#include "voip.h"
#include "multipart.h"
#include "audio_convert.h"
#include "logctl.h"
#include "audio.h"
#include "aec.h"
#include "es8388.h"
#include "sdcard.h"
#include "idle_power.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_flash.h"
#include "esp_netif.h"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <ctype.h>
#include <strings.h>

static const char *TAG = "webui";

/* Same bound player.c's playback path uses (HEADER_SCAN_BYTES) -- covers
 * real-world files with extra chunks before "data" (macOS afconvert pads
 * with a "FLLR" filler chunk, observed pushing the data chunk out to
 * ~4096 bytes; this project's own operator.wav is generated that way).
 * Always heap-allocate this, never a stack array: 8KB inline blew the
 * httpd task's stack once already (see the "Stack protection fault" this
 * project hit building the multipart reader), and every caller here already
 * has other locals plus a caller frame on top of it. */
#define UPLOAD_HEADER_SCAN_BYTES 8192

/* Human-readable size: "4.19 MB", "188.7 kB", "0 B" (decimal units, matching
 * how flash/card sizes are marketed). Writes into `out` and returns it so it
 * can be used inline as an snprintf argument. Caller supplies one buffer per
 * value shown (they're all live at once inside a single snprintf). */
static const char *fmt_bytes(uint64_t n, char *out, size_t outsz)
{
    /* GB tier added once storage.c started reporting real microSD capacity
     * (storage.h) -- internal flash never gets remotely close to it, but an
     * SD card in the tens of GB rendering as "14900.00 MB" was silly. */
    if (n >= 1000000000ULL)      snprintf(out, outsz, "%.2f GB", (double)n / 1e9);
    else if (n >= 1000000ULL)    snprintf(out, outsz, "%.2f MB", (double)n / 1e6);
    else if (n >= 1000ULL)       snprintf(out, outsz, "%.1f kB", (double)n / 1e3);
    else                         snprintf(out, outsz, "%u B", (unsigned)n);
    return out;
}

/* Defined below (near where /guide's cards used to live) -- forward-declared
 * here so the settings-tab renderers, which come first in this file, can
 * reuse the same dial-code-row markup instead of duplicating it. */
static int append_guide_row(char *buf, size_t cap, int off, const char *code,
                             const char *desc, bool implemented);

/* Defined below, next to append_guide_row() -- the dial-guide card set
 * (formerly its own /guide page) is now rendered inline on the Status page
 * (index_handler, earlier in this file than its definition). */
static int append_dialguide_cards(char *buf, size_t cap, int off,
                                   dialplan_entry_info_t *entries, size_t n);

/* Defined below, near append_log_panel() -- same visual structure, sourced
 * from activity_get() instead of logbuf_snapshot(). */
static int append_activity_panel(char *buf, size_t cap, int off);

/* Defined below (near the Settings -> Storage section it was written for) --
 * forward-declared here so recordings_page_handler(), earlier in the file,
 * can reuse it instead of hand-rolling its own near-duplicate table. */
static int append_storage_table(char *buf, size_t cap, int off,
                                storage_entry_t *entries, size_t n,
                                const char *name_prefix, bool show_recorded,
                                const char *return_to);

/* Defined below -- forward-declared here so append_activity_panel() (which
 * needs it to escape possibly remote-supplied caller ID text, and is
 * defined earlier in the file, right after append_log_panel()) can call it. */
static void html_escape(const char *in, char *out, size_t outsz);

/* firmware/assets/ ships README.md and .gitignore alongside actual clips
 * (see that directory's own README) -- filter those out of "pick a clip to
 * play" lists so a user can't assign a dial code to a non-audio file. */
static bool is_wav_name(const char *name)
{
    size_t len = strlen(name);
    return len > 4 && strcasecmp(name + len - 4, ".wav") == 0;
}

/* CONFIG_PLAYER_MP3 clips (see player/Kconfig) -- a real, playable audio
 * asset just like a .wav one, so anywhere is_wav_name() gates "is this a
 * pickable/playable clip, not README.md/.gitignore" needs this alongside
 * it. Kept as a separate helper (not folded into is_wav_name()) because a
 * few call sites -- media_handler's Content-Type, wav_duration_str()'s
 * header parsing -- genuinely need to tell the two formats apart, not just
 * "is it audio". */
static bool is_mp3_name(const char *name)
{
    size_t len = strlen(name);
    return len > 4 && strcasecmp(name + len - 4, ".mp3") == 0;
}

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
        "<div class=\"logpanel\" style=\"margin-top:16px\"><h2>Recent log"
        "<button type=\"button\" onclick=\"copyLog(this)\">Copy log</button>"
        "</h2><div class=\"logbody\">");

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

    /* Plain execCommand fallback first because this page is normally loaded
     * over http://<name>.local, which most browsers don't treat as a secure
     * context -- navigator.clipboard is unavailable there, only on https or
     * localhost. */
    off += snprintf(out + off, ((size_t)off < out_len) ? out_len - off : 0, "</div></div>"
        "<script>function copyLog(btn){"
        "var text=btn.closest('.logpanel').querySelector('.logbody').innerText;"
        "function fallback(){"
        "var ta=document.createElement('textarea');ta.value=text;"
        "ta.style.position='fixed';ta.style.opacity='0';"
        "document.body.appendChild(ta);ta.focus();ta.select();"
        "try{document.execCommand('copy');}catch(e){}"
        "document.body.removeChild(ta);}"
        "if(navigator.clipboard&&navigator.clipboard.writeText){"
        "navigator.clipboard.writeText(text).catch(fallback);"
        "}else{fallback();}"
        "var t=btn.textContent;btn.textContent='Copied!';"
        "setTimeout(function(){btn.textContent=t;},1500);}"
        "</script>");
    return off;
}

/* Renders the activity ring buffer (activity.h's structured "what just
 * happened" log -- calls, recordings, dial errors...) as a panel visually
 * consistent with append_log_panel() above (same .logpanel/.logbody/.logline
 * classes), but simpler: no ANSI-stripping or tag/kind parsing needed since
 * activity_get() already returns clean, pre-formatted, relative-time-
 * prefixed strings, newest first. HTML-escaped per line (unlike log lines,
 * which are entirely our own ESP_LOG text): some entries embed remote-
 * supplied text (inbound caller name/number, see telephony.c's
 * on_voip_incoming()), so this can't skip escaping the way append_log_line()
 * does. */
static int append_activity_panel(char *buf, size_t cap, int off)
{
    char lines[20][96];
    size_t n = activity_get(lines, 20);

    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<div class=\"logpanel\" style=\"margin-top:16px\"><h2>Recent activity</h2>"
        "<div class=\"logbody\">");
    if (n == 0) {
        off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
            "<div class=\"empty\">nothing yet</div>");
    } else {
        for (size_t i = 0; i < n && (size_t)off < cap; i++) {
            char eline[96 * 6];
            html_escape(lines[i], eline, sizeof(eline));
            off += snprintf(buf + off, cap - off,
                "<div class=\"logline\"><span class=\"m\">%s</span></div>", eline);
        }
    }
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0, "</div></div>");
    return off;
}

static cJSON *build_status_json(void)
{
    cJSON *root = cJSON_CreateObject();
    char ip[16];
    wifi_get_ip_str(ip, sizeof(ip));
    int num_lines = board_hw_config_get().num_lines;

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

    /* Diagnostics / DVT-harness introspection. */
    cJSON_AddStringToObject(root, "log_level", logctl_level_name(logctl_get_level()));
    cJSON_AddBoolToObject(root, "test_mode", telephony_test_enabled());
    cJSON_AddBoolToObject(root, "test_loopback", audio_test_loopback());
    /* No passwords here -- server/user only, so a test harness can snapshot
     * the persisted VoIP config before poking it and restore it after. */
    cJSON_AddStringToObject(root, "voip_server", voip_server());
    cJSON_AddStringToObject(root, "voip_user0", voip_user(0));
    if (num_lines > 1) {
        cJSON_AddStringToObject(root, "voip_user1", voip_user(1));
    }

    cJSON *lines = cJSON_AddArrayToObject(root, "lines");
    for (int l = 0; l < num_lines; l++) {
        cJSON *jl = cJSON_CreateObject();
        cJSON_AddNumberToObject(jl, "line", l);
        cJSON_AddStringToObject(jl, "state", telephony_state_name(telephony_state(l)));
        cJSON_AddStringToObject(jl, "last_number", telephony_last_number(l));
        /* No escaping here (unlike the HTML page): cJSON handles JSON string
         * quoting itself, and consumers of this API get the raw value. */
        cJSON_AddStringToObject(jl, "caller_name", telephony_caller_name(l));
        cJSON_AddStringToObject(jl, "caller_number", telephony_caller_number(l));
        cJSON_AddNumberToObject(jl, "rx_level", audio_rx_level(l));
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

/* Escape text that came from off-device into HTML body text. Needed for
 * caller ID specifically: the display name and number are copied verbatim
 * out of a remote UA's SIP From header, so they're attacker-controlled
 * strings being rendered into this page -- everything else the status page
 * prints originates on this device. Truncates rather than overflowing;
 * always NUL-terminates. */
static void html_escape(const char *in, char *out, size_t outsz)
{
    size_t o = 0;
    if (outsz == 0) return;
    for (size_t i = 0; in[i] && o < outsz - 1; i++) {
        const char *rep = NULL;
        switch (in[i]) {
            case '<':  rep = "&lt;";   break;
            case '>':  rep = "&gt;";   break;
            case '&':  rep = "&amp;";  break;
            case '"':  rep = "&quot;"; break;
            case '\'': rep = "&#39;";  break;
            default: break;
        }
        if (rep) {
            size_t rl = strlen(rep);
            if (o + rl >= outsz) break; /* no room for the whole entity */
            memcpy(out + o, rep, rl);
            o += rl;
        } else {
            out[o++] = in[i];
        }
    }
    out[o] = '\0';
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

/* Status / Recordings / Settings. The Dial Guide used to be its own 4th tab
 * (see pi_rotary_phone/web/templates/index.html for the Pi UI this mirrored)
 * -- its cards are now merged onto the Status page itself
 * (append_dialguide_cards()), so there's no separate tab for it any more;
 * /guide still resolves, just as a redirect to /. Shared across every page
 * handler so the tab set and .active highlighting can't drift out of sync
 * between pages. */
typedef struct { const char *href; const char *label; } nav_tab_t;
static const nav_tab_t NAV_TABS[] = {
    { "/",           "Status" },
    { "/recordings", "Recordings" },
    { "/settings",   "Settings" },
};
#define NAV_TABS_N (sizeof(NAV_TABS) / sizeof(NAV_TABS[0]))

static int append_page_head(char *buf, size_t cap, int off, const char *page_title,
                             const char *active_href, bool auto_refresh)
{
    /* Board name leads the <title> and the header so a browser tab / glance
     * tells several boards on one LAN apart (that's the whole point of the
     * name). "Analog Redial" drops to a subtitle. */
    char name[32];
    board_name_get(name, sizeof(name));
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<!doctype html><html><head><meta charset=\"utf-8\"><title>%s \xc2\xb7 %s</title>"
        "%s"
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
        "<link rel=\"stylesheet\" href=\"/static/style.css\"></head><body>"
        "<header><h1>%s <span class=\"dot\">Analog Redial</span></h1><nav>",
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
        "<title>Analog Redial \xc2\xb7 WiFi setup</title>"
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
        "<link rel=\"stylesheet\" href=\"/static/style.css\"></head><body>"
        "<header><h1>Analog Redial <span class=\"dot\">WiFi setup</span></h1></header>"
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

    /* This page now also carries the merged Dial Guide cards (formerly
     * /guide) below the status/line cards and the activity panel -- sized
     * the same way guide_handler used to size itself, plus modest room for
     * everything above it (small: a handful of status rows + up to 20 short
     * activity entries, see append_activity_panel()). */
    dialplan_entry_info_t entries[40];
    size_t n = dialplan_list(entries, 40);
    size_t cap = 8192 + n * 220;
    char *buf = malloc(cap);
    if (!buf) { httpd_resp_send_500(req); return ESP_FAIL; }

    int off = append_page_head(buf, cap, 0, "Status", "/", true);
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "</header>"
        "<main>"
        "<p class=\"tagline\">Plain old telephone service exchange</p>"
        "<div class=\"cards\">"
        "<div class=\"card\"><h2>System</h2>"
        /* Connection status: used to live as a 3rd <header> child (a
         * "<span class=dot>" sibling of <h1>/<nav>), which broke the
         * header's flex space-between layout (nav needs to be the LAST
         * child to sit flush right) -- moved here since it's the same
         * information the System card already shows underneath. */
        "<div class=\"statusrow\"><span class=\"label\">Connection</span><span>%s%s%s</span></div>"
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
        /* True uint64_t SD getters, not the saturating primary-backend ones
         * -- see the matching comment in recordings_page_handler(). */
        fmt_bytes(storage_sd_present() ? storage_used_bytes_sd() : storage_used_bytes_internal(), su, sizeof(su)),
        fmt_bytes(storage_sd_present() ? storage_cap_bytes_sd() : storage_cap_bytes_internal(), sc, sizeof(sc)),
        storage_using_sd() ? " (SD)" : " (internal flash)");

    int num_lines = board_hw_config_get().num_lines;
    for (int l = 0; l < num_lines && off > 0 && (size_t)off < cap; l++) {
        tel_state_t st = telephony_state(l);
        /* Caller ID is remote-supplied -- escape before rendering. */
        char cname[160], cnum[128], caller[300];
        html_escape(telephony_caller_name(l), cname, sizeof(cname));
        html_escape(telephony_caller_number(l), cnum, sizeof(cnum));
        if (cnum[0] && cname[0])      snprintf(caller, sizeof(caller), "%s (%s)", cname, cnum);
        else if (cnum[0])             snprintf(caller, sizeof(caller), "%s", cnum);
        else                          snprintf(caller, sizeof(caller), "%s", "&mdash;");

        off += snprintf(buf + off, cap - off,
            "<div class=\"card\"><h2>Line %d</h2>"
            "<div class=\"statusrow\"><span class=\"label\">State</span>"
            "<span class=\"pill %s\">%s</span></div>"
            "<div class=\"statusrow\"><span class=\"label\">Last number</span><span>%s</span></div>"
            "<div class=\"statusrow\"><span class=\"label\">%s</span><span>%s</span></div>"
            "</div>",
            l, pill_class_for(st), telephony_state_name(st), telephony_last_number(l),
            st == TEL_RINGING ? "Incoming call" : "Last caller", caller);
    }
    if (off > 0 && (size_t)off < cap) {
        off += snprintf(buf + off, cap - off, "</div>" /* .cards */);
    }
    if (off > 0 && (size_t)off < cap) {
        off = append_activity_panel(buf, cap, off);
    }
    if (off > 0 && (size_t)off < cap) {
        off += snprintf(buf + off, cap - off, "<div class=\"cards\">");
    }
    off = append_dialguide_cards(buf, cap, off, entries, n);
    if (off > 0 && (size_t)off < cap) {
        off += snprintf(buf + off, cap - off,
            "<div class=\"toolbar sub\">"
            "<a href=\"/api/log\">raw log</a>"
            "<a href=\"/api/status\">status JSON</a>"
            "<a href=\"/api/recordings\">recordings JSON</a>"
            "</div></main></body></html>");
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, buf, (off > 0) ? off : 0);
    free(buf);
    return ESP_OK;
}

/* A "recording" is something the user recorded from a handset (211 "leave a
 * message" archives, named rec_*.wav). Everything else in the storage
 * partition -- operator.wav, clip_*.wav fun clips -- is a built-in/uploaded
 * asset, managed under Settings, not shown here. */
static bool is_user_recording(const char *name)
{
    return strncmp(name, "rec_", 4) == 0;
}

/* Dedicated Recordings page (Pi's /recordings): only user-recorded messages,
 * so it isn't cluttered with the operator/clip assets that live in the same
 * flat storage partition. Manage those under Settings -> Storage files. */
static esp_err_t recordings_page_handler(httpd_req_t *req)
{
    /* Both backends, not just primary: unlike Settings -> Storage files
     * (which shows microSD and internal flash as separate, clearly-labeled
     * cards for management), this is the casual "your messages" page --
     * merging avoids a 211 recording made before a card was inserted just
     * vanishing from view once the card takes over as primary. SD entries
     * first since they're the newer ones in every practical case (a card
     * added partway through this board's life, not before). */
    storage_entry_t entries[64];
    size_t n = storage_sd_present() ? storage_list_sd(entries, 32) : 0;
    n += storage_list_internal(entries + n, 64 - n);
    size_t shown = 0;
    for (size_t i = 0; i < n; i++) if (is_user_recording(entries[i].name)) shown++;

    size_t cap = 2048 + n * 400; /* each row: <audio>, download link, delete form, Recorded column */
    char *buf = malloc(cap);
    if (!buf) { httpd_resp_send_500(req); return ESP_FAIL; }

    int off = append_page_head(buf, cap, 0, "Recordings", "/recordings", false);
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
        /* Same table renderer Settings -> Storage uses, filtered to rec_*
         * names and with the "Recorded" column turned on -- see
         * append_storage_table()'s own doc comment. Gets duration, a
         * readable timestamp, download, and delete for free instead of this
         * page's old hand-rolled Name/Size/Play-only table. */
        off = append_storage_table(buf, cap, off, entries, n, "rec_", true, "/recordings");
    }
    if (off > 0 && (size_t)off < cap) {
        char su[16], sc[16];
        bool sd = storage_sd_present();
        /* True uint64_t SD getters here, not the primary-backend
         * storage_used_bytes()/storage_cap_bytes() -- those saturate at
         * SIZE_MAX (~4.29 GB) for a bigger card (see storage.c), which would
         * under-report a real card's capacity right next to the Hardware
         * tab's correct figure for the same card. */
        off += snprintf(buf + off, cap - off,
            "</div>"
            "<p class=\"hint\">Storage: %s / %s%s. Raw list: "
            "<a href=\"/api/recordings\">recordings JSON</a>.</p>"
            "</main></body></html>",
            fmt_bytes(sd ? storage_used_bytes_sd() : storage_used_bytes_internal(), su, sizeof(su)),
            fmt_bytes(sd ? storage_cap_bytes_sd() : storage_cap_bytes_internal(), sc, sizeof(sc)),
            sd ? " (SD)" : " (internal flash)");
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, buf, (off > 0) ? off : 0);
    free(buf);
    return ESP_OK;
}

/* Tab set mirrors the Pi's settings.html (General / Operator / VOIP /
 * Inter-phone / Record message / Hardware) so the shape previews where each
 * section will land, even though most of them are still placeholders here --
 * radio and VoIP aren't wired up on the ESP32 yet (see archive/ESP32_PORT_PLAN.md).
 * "Record & callback" (212-215) was part of this set too until it was
 * archived -- see docs/features.md "Archived features". Server-rendered via
 * ?tab=, not client JS: every other page in this component is a plain
 * server-rendered form, and a full reload per tab click keeps this page
 * consistent with that instead of adding another script tag. */
/* Five tabs, not the nine this once grew to: Operator and Record message
 * were each exactly one card (prompt text + audio upload) and Dial Codes is
 * "what plays for a code" already, so all three now render together under
 * "dialcodes" -- see the dispatch in settings_page_handler(). Likewise
 * Hardware (stats), the SMD line2/microSD config card, and the Storage file
 * manager all render together under "hardware" -- they're all "this board's
 * physical resources," previously split across two tabs with Hardware's own
 * text pointing you at the other one. Inter-phone was dropped outright: it
 * only ever showed the 3xx dial codes read-only with a "not editable here"
 * hint -- identical data the merged Status page's Dial Guide already shows,
 * so the tab added a click with no unique content. */
typedef struct { const char *id; const char *label; } settings_tab_t;
static const settings_tab_t SETTINGS_TABS[] = {
    { "general",    "General" },
    { "dialcodes",  "Dial Codes & Prompts" },
    { "voip",       "VOIP" },
    { "hardware",   "Hardware" },
    { "diag",       "Diagnostics" },
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
 * that set it explicitly. The phone's own dial codes (e.g. 921/922) always
 * set state outright rather than toggling it, so these buttons mirror that
 * instead of offering a single flip. */
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

    board_offhook_alert_t roh = board_offhook_alert_get();
    char roh_status[48];
    if (roh.enabled) snprintf(roh_status, sizeof(roh_status), "on, after %u s", (unsigned)roh.seconds);
    else             snprintf(roh_status, sizeof(roh_status), "off");
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<div class=\"card\"><h2>Off-hook alert</h2>"
        "<div class=\"statusrow\"><span class=\"label\">Current</span><span>%s</span></div>"
        "<form method=\"POST\" action=\"/offhook_alert\" class=\"settings-form\">"
        "<label><input type=\"checkbox\" name=\"enabled\" value=\"1\"%s> Enabled</label>"
        "<label>Seconds on dial tone before the alert "
        "<input type=\"number\" name=\"seconds\" value=\"%u\" min=\"%d\" max=\"%d\" required></label>"
        "<div class=\"toolbar sub\"><button type=\"submit\" class=\"primary\">Save</button></div>"
        "</form>"
        "<p class=\"hint\">A handset left off the hook sits on dial tone forever, and the line "
        "is unreachable the whole time (callers get busy) with nothing to draw attention to it. "
        "This plays the loud, warbling \"receiver off-hook\" alert real phone switches use "
        "(1400+2060+2450+2600&nbsp;Hz, 100&nbsp;ms on/off, per Telcordia GR-506-CORE) once dial "
        "tone has gone untouched this long. Hanging up stops it. The clock restarts on every "
        "return to dial tone, so only a genuinely forgotten handset ever reaches it. "
        "Takes effect on the next dial tone -- no reboot.</p></div>",
        roh_status,
        roh.enabled ? " checked" : "",
        (unsigned)roh.seconds, BOARD_OFFHOOK_ALERT_MIN_S, BOARD_OFFHOOK_ALERT_MAX_S);

    idle_power_cfg_t ip = idle_power_get();
    char ip_status[48];
    if (ip.enabled) snprintf(ip_status, sizeof(ip_status), "on, after %u min", (unsigned)ip.minutes);
    else            snprintf(ip_status, sizeof(ip_status), "off");
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<div class=\"card\"><h2>Low-power idle mode</h2>"
        "<div class=\"statusrow\"><span class=\"label\">Current</span><span>%s</span></div>"
        "<form method=\"POST\" action=\"/idle_power\" class=\"settings-form\">"
        "<label><input type=\"checkbox\" name=\"enabled\" value=\"1\"%s> Enabled</label>"
        "<label>Minutes idle before sleeping "
        "<input type=\"number\" name=\"minutes\" value=\"%u\" min=\"%d\" max=\"%d\" required></label>"
        "<div class=\"toolbar sub\"><button type=\"submit\" class=\"primary\">Save</button></div>"
        "</form>"
        "<p class=\"hint\">Approximately %.2f&nbsp;W of power savings, but expect up to ~300ms "
        "delay the first time you pick up the handset after %u idle minutes; fully responsive "
        "for the rest of that call and for %u minutes after.</p></div>",
        ip_status,
        ip.enabled ? " checked" : "",
        (unsigned)ip.minutes, IDLE_POWER_MINUTES_MIN, IDLE_POWER_MINUTES_MAX,
        (double)idle_power_estimated_savings_w(), (unsigned)ip.minutes, (unsigned)ip.minutes);

    dialplan_modes_t *m = dialplan_modes();
    off = append_mode_card(buf, cap, off, "Digit readback",
        m->readback_digits ? "on" : "off", m->readback_digits ? "On" : "Off",
        "Turn off", "921", "Turn on", "922", -1);
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<p class=\"hint\">Persists to flash (NVS) and survives a reboot.</p>");
    return off;
}

/* Builds a <select> of every playable name storage_read_open() can actually
 * find: writable storage (uploads, recordings) first, then bundled defaults
 * not already shadowed by a writable file of the same name. Shared by the
 * Dial Codes tab's "add new" and "replace audio" forms -- its only two call
 * sites, both assigning audio to a dial code, which always lives on internal
 * flash regardless of whether a microSD card is mounted (see
 * storage_write_begin_local()) -- so this lists storage_list_internal(), NOT
 * storage_list() (which would list the microSD card when present, making SD
 * content wrongly pickable as a dial-code clip). `selected`, if non-NULL, is
 * pre-selected (used when re-rendering "replace audio" for a specific
 * existing entry). */
static int append_file_select(char *buf, size_t cap, int off, const char *field_name,
                                const char *selected)
{
    storage_entry_t files[32], bundled[16];
    size_t nfiles = storage_list_internal(files, 32);
    size_t nbundled = storage_list_bundled(bundled, 16);

    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<select name=\"%s\" required>", field_name);
    if (nfiles == 0 && nbundled == 0) {
        off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
            "<option value=\"\">(no files uploaded yet)</option>");
    }
    for (size_t i = 0; i < nfiles && (size_t)off < cap; i++) {
        if (!is_wav_name(files[i].name) && !is_mp3_name(files[i].name)) continue;
        bool sel = selected && strcmp(selected, files[i].name) == 0;
        off += snprintf(buf + off, cap - off,
            "<option value=\"%s\"%s>%s</option>", files[i].name,
            sel ? " selected" : "", files[i].name);
    }
    for (size_t i = 0; i < nbundled && (size_t)off < cap; i++) {
        /* firmware/assets/ also ships README.md and .gitignore alongside
         * the actual clips (see that directory) -- not playable, so keep
         * them out of this "pick a clip" list. */
        if (!is_wav_name(bundled[i].name) && !is_mp3_name(bundled[i].name)) continue;
        bool shadowed = false;
        for (size_t j = 0; j < nfiles; j++) {
            if (strcmp(files[j].name, bundled[i].name) == 0) { shadowed = true; break; }
        }
        if (shadowed) continue;
        bool sel = selected && strcmp(selected, bundled[i].name) == 0;
        off += snprintf(buf + off, cap - off,
            "<option value=\"%s\"%s>%s (bundled)</option>", bundled[i].name,
            sel ? " selected" : "", bundled[i].name);
    }
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0, "</select>");
    return off;
}

/* Dial Codes tab: one mutable code<->filename table covers both the builtin
 * "silly audio" clips and anything a user adds -- see dialplan_clip_entry_t.
 * Each row's code and audio file are independently editable; builtin rows
 * can't be deleted, only recoded/re-pointed at a different file. Operator
 * help (dial 0) isn't here -- its code is fixed and its audio is replaced
 * from Settings -> Operator instead. */
/* Renders a "prompt text" form (POST /prompt_text_set) for the WAV named
 * `clip_name` -- used on the Operator, Record, and (per-row) Dial Codes
 * pages so each spoken clip's desired text lives right next to the setting
 * it belongs to, rather than on a separate "Prompts" page. Saving text here
 * doesn't generate audio: firmware/bringup/gen_prompt.py --sync reads it
 * back off the board (gated by the shared setup password, see
 * voip_setup_password()'s doc comment in voip.h), synthesizes it locally,
 * and uploads the WAV -- which is what actually clears "pending". */
/* `default_text` (NULL for none) is shown pre-filled when nothing's been
 * tracked for `clip_name` yet -- a starting point to edit, not itself
 * persisted anywhere until the user actually saves it. Used for the
 * Operator prompt specifically (a sensible greeting to edit rather than a
 * blank box); other callers pass NULL. */
static int append_prompt_field(char *buf, size_t cap, int off, const char *clip_name,
                                const char *default_text)
{
    prompt_entry_t pe;
    bool have = prompts_get(clip_name, &pe);
    const char *text = have ? pe.text : (default_text ? default_text : "");
    bool pending = have && strcmp(pe.text, pe.generated_text) != 0;

    char etext[PROMPT_TEXT_MAX * 6];
    html_escape(text, etext, sizeof(etext));
    char ename[PROMPT_NAME_MAX * 6];
    html_escape(clip_name, ename, sizeof(ename));

    const char *status = have
        ? (pending ? "Pending: text changed since last generate."
                   : "Up to date with the generated audio.")
        : (text[0] ? "Suggested default -- edit and Save to actually set it."
                   : "No text set -- this clip stays silent/skipped until generated.");

    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<form method=\"POST\" action=\"/prompt_text_set\" class=\"settings-form\">"
        "<input type=\"hidden\" name=\"name\" value=\"%s\">"
        "<label>Spoken prompt text"
        "<textarea name=\"text\" maxlength=\"%d\" rows=\"2\" placeholder="
        "\"Leave blank for no spoken prompt\">%s</textarea>"
        "<span class=\"fieldhint\">%s</span>"
        "</label>"
        "<div class=\"toolbar sub\"><button type=\"submit\">Save text</button></div>"
        "</form>",
        ename, PROMPT_TEXT_MAX - 1, etext, status);

    /* A ready-to-paste --sync command for just this one clip (--only), with
     * the real secret filled in -- at the user's request, plaintext rather
     * than the usual masked-bullets treatment (compare the VOIP tab's own
     * password field). That's a deliberate exception, not an oversight: this
     * secret already has no confidentiality left to protect once you can
     * load this settings page at all, since the VOIP tab lets that same
     * visitor read/replace it (masked only against shoulder-surfing the
     * input box, not against page access) and reuse it to hit
     * /api/prompt_requests directly. Spelling it out here just saves a trip
     * to that tab and a manual copy. */
    const char *pw = voip_setup_password();
    if (pw[0]) {
        char ip[16];
        wifi_get_ip_str(ip, sizeof(ip));
        char cmd[256];
        snprintf(cmd, sizeof(cmd),
                 "firmware/bringup/gen_prompt.py --sync %s --secret \"%s\" --only %s",
                 ip, pw, clip_name);
        char ecmd[256 * 6];
        html_escape(cmd, ecmd, sizeof(ecmd));
        off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
            "<p class=\"hint\">Generate + upload this clip's audio (run from the repo root):"
            "<br><code style=\"user-select:all\">%s</code></p>", ecmd);
    } else {
        off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
            "<p class=\"hint\">Generate + upload with "
            "<code>firmware/bringup/gen_prompt.py --sync HOST --secret ...</code> "
            "(local text-to-speech, no server dependency) -- set a setup password "
            "on the VOIP tab first to get an exact, ready-to-paste command here.</p>");
    }
    return off;
}

static int append_dialcodes_tab(char *buf, size_t cap, int off)
{
    dialplan_clip_entry_t clips[DIALPLAN_MAX_CLIP_ENTRIES];
    size_t nclips = dialplan_list_clip_entries(clips, DIALPLAN_MAX_CLIP_ENTRIES);

    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<div class=\"card\"><h2>Dial codes (%u)</h2>"
        "<p class=\"hint\">Change which number plays which clip, or point a code at a "
        "different file, without a rebuild. \"Built-in\" entries can be recoded and given "
        "new audio but not removed. Upload new audio under Storage files below first if it "
        "isn't in the list yet.</p>",
        (unsigned)nclips);

    if (nclips == 0) {
        off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
            "<div class=\"empty\">none yet</div>");
    } else {
        off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
            "<table><tr><th>Code</th><th>Plays</th><th>Play</th><th>Description</th><th></th><th></th></tr>");
        for (size_t i = 0; i < nclips && (size_t)off < cap; i++) {
            char edesc[64 * 6];
            html_escape(clips[i].desc, edesc, sizeof(edesc));
            off += snprintf(buf + off, cap - off,
                "<tr><td>"
                "<form method=\"POST\" action=\"/dialcode_set_code\" style=\"display:inline\">"
                "<input type=\"hidden\" name=\"old_code\" value=\"%s\">"
                "<input name=\"new_code\" value=\"%s\" pattern=\"[0-9]{2,6}\" maxlength=\"6\" "
                "size=\"6\" required>"
                "<button type=\"submit\">save</button></form></td>"
                "<td><code>%s</code></td>"
                "<td><audio controls preload=\"none\" src=\"/media/%s?inline=1\"></audio></td>"
                "<td><form method=\"POST\" action=\"/dialcode_set_desc\" style=\"display:inline\">"
                "<input type=\"hidden\" name=\"code\" value=\"%s\">"
                "<input name=\"desc\" value=\"%s\" maxlength=\"63\" size=\"20\">"
                "<button type=\"submit\">save</button></form>%s</td>"
                "<td><form method=\"POST\" action=\"/dialcode_set_filename\" style=\"display:inline\">"
                "<input type=\"hidden\" name=\"code\" value=\"%s\">",
                clips[i].code, clips[i].code, clips[i].filename, clips[i].filename,
                clips[i].code, edesc,
                clips[i].builtin ? " <span class=\"pill off\">built-in</span>" : "",
                clips[i].code);
            off = append_file_select(buf, cap, off, "filename", clips[i].filename);
            off += snprintf(buf + off, cap - off,
                "<button type=\"submit\">use existing file</button></form> "
                "<form method=\"POST\" action=\"/dialcode_upload\" enctype=\"multipart/form-data\" "
                "style=\"display:inline\">"
                "<input type=\"hidden\" name=\"code\" value=\"%s\">"
                "<input type=\"file\" name=\"file\" accept=\"audio/wav\" required>"
                "<button type=\"submit\">upload new audio</button></form></td><td>",
                clips[i].code);
            if (!clips[i].builtin) {
                off += snprintf(buf + off, cap - off,
                    "<form method=\"POST\" action=\"/dialcode_delete/%s\" "
                    "onsubmit=\"return confirm('Remove dial code %s?')\">"
                    "<button type=\"submit\">remove</button></form>",
                    clips[i].code, clips[i].code);
            }
            off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0, "</td></tr>");
        }
        off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0, "</table>");
    }

    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<form method=\"POST\" action=\"/dialcode_add\" class=\"settings-form\">"
        "<h3>Add a code for an existing file</h3>"
        "<div class=\"grid2\">"
        "<label>New code (2-6 digits) <input name=\"code\" pattern=\"[0-9]{2,6}\" "
        "maxlength=\"6\" required></label>"
        "<label>File to play ");
    off = append_file_select(buf, cap, off, "filename", NULL);
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "</label>"
        "<label>Description (optional) <input name=\"desc\" maxlength=\"63\"></label></div>"
        "<div class=\"toolbar sub\"><button type=\"submit\" class=\"primary\">Add dial code</button></div>"
        "</form>"
        "<form method=\"POST\" action=\"/dialcode_upload\" enctype=\"multipart/form-data\" class=\"settings-form\">"
        "<h3>...or upload new audio for a new code</h3>"
        "<div class=\"grid2\">"
        "<label>New code (2-6 digits) <input name=\"new_code\" pattern=\"[0-9]{2,6}\" "
        "maxlength=\"6\" required></label>"
        "<label>Description (optional) <input name=\"desc\" maxlength=\"63\"></label></div>"
        "<label>Audio file (WAV) <input type=\"file\" name=\"file\" accept=\"audio/wav\" required></label>"
        "<div class=\"toolbar sub\"><button type=\"submit\" class=\"primary\">Upload + add dial code</button></div>"
        "</form></div>");
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
    off = append_prompt_field(buf, cap, off, "operator.wav",
        "You've reached the operator line. To leave a message, dial 2-1-1. "
        "For other features, please refer to your phone's guide.");
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<p class=\"hint\">Or upload audio directly below (converted automatically to 8kHz "
        "mono) -- either way replaces what plays.</p>"
        "<form method=\"POST\" action=\"/operator_upload\" enctype=\"multipart/form-data\" "
        "class=\"settings-form\">"
        "<label>Replacement audio (WAV) <input type=\"file\" name=\"file\" accept=\"audio/wav\" required></label>"
        "<div class=\"toolbar sub\"><button type=\"submit\" class=\"primary\">Upload</button></div>"
        "</form></div>");
    return off;
}

/* Shared VOIP setup-steps builder -- defined further down, near
 * voip_server_script_handler() (also used by GET /voip_instructions.txt). */
#define VOIP_INSTR_STEPS 4
#define VOIP_INSTR_STEP_LEN 200
static void build_voip_instructions(char steps[VOIP_INSTR_STEPS][VOIP_INSTR_STEP_LEN]);

static int append_voip_tab(char *buf, size_t cap, int off, const char *checked, const char *result,
                            const char *created, const char *createresult)
{
    bool r0 = voip_is_registered(0);
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<div class=\"card wide\"><h2>VOIP</h2>"
        "<div class=\"statusrow\"><span class=\"label\">SIP server</span><span>%s</span></div>"
        "<div class=\"statusrow\"><span class=\"label\">Line 1</span>"
        "<span class=\"pill %s\">%s</span></div>",
        voip_server()[0] ? voip_server() : "(not set)",
        r0 ? "on" : "off", r0 ? "registered" : "not registered");
    /* Shown for a genuine 2-line board, but also whenever a line-2 account
     * is still configured on a 1-line board -- e.g. a board reconfigured
     * down from 2 lines to 1, or a stale account from before -- so there's
     * still a status pill (and, below, form fields) to clear it by. */
    if (board_hw_config_get().num_lines > 1 || voip_user(1)[0]) {
        bool r1 = voip_is_registered(1);
        off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
            "<div class=\"statusrow\"><span class=\"label\">Line 2</span>"
            "<span class=\"pill %s\">%s</span></div>",
            r1 ? "on" : "off", r1 ? "registered" : "not registered");
    }
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0, "</div>");

    /* 1. Point this phone at your SIP server -- promoted to the top of the
     * tab (was step 4, behind an auto-provisioning flow that made server
     * calls on this page's behalf; that's gone, so this plain "server +
     * credentials" form is the actual first thing to do now). Numbered
     * setup steps + both downloads sit right here, prominent and always
     * visible (not a collapsed <details>), since this is now the main
     * content of the tab, not an afterthought under three other cards. */
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<div class=\"card wide\"><h2>1. Point this phone at your SIP server</h2>"
        "<form method=\"POST\" action=\"/voip_set\" class=\"settings-form\">"
        "<label>SIP server (host or IP) <input name=\"server\" value=\"%s\"></label>"
        "<label>Line 1 extension/user <input name=\"user0\" value=\"%s\"></label>"
        "<label>Line 1 password <input type=\"password\" name=\"pass0\" "
        "placeholder=\"leave blank to keep\"></label>",
        voip_server(), voip_user(0));
    /* Same "or a stale account exists" condition as the status pill above
     * -- otherwise a 1-line board with a leftover line-2 config (e.g.
     * reconfigured down from 2 lines) has no way to ever clear it: the
     * fields to submit blank would themselves be hidden. */
    if (board_hw_config_get().num_lines > 1 || voip_user(1)[0]) {
        off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
            "<label>Line 2 extension/user <input name=\"user1\" value=\"%s\"></label>"
            "<label>Line 2 password <input type=\"password\" name=\"pass1\" "
            "placeholder=\"leave blank to keep\"></label>",
            voip_user(1));
    }
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<div class=\"toolbar sub\">"
        "<button type=\"submit\" class=\"primary\">Save (takes effect on reboot)</button>"
        "<a class=\"button\" href=\"/voip_server_script\" download=\"rotary-voip-server-setup.sh\">"
        "Download setup script</a>"
        "<a class=\"button\" href=\"/voip_instructions.txt\" download=\"voip_setup_instructions.txt\">"
        "Download setup instructions</a>"
        "</div></form>");

    char steps[VOIP_INSTR_STEPS][VOIP_INSTR_STEP_LEN];
    build_voip_instructions(steps);
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<p class=\"hint\">Setup steps (same wording as the downloadable copy above):</p><ol>");
    for (int i = 0; i < VOIP_INSTR_STEPS && (size_t)off < cap; i++) {
        off += snprintf(buf + off, cap - off, "<li>%s</li>", steps[i]);
    }
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "</ol>"
        "<details><summary>Prefer to add the line by hand instead?</summary><ol>"
        "<li>Pick a free extension -- \"Check a number\" below, or see "
        "<code>archive/VOIP_SETUP.md</code> for this project's shared numbering registry.</li>"
        "<li>SSH into the Asterisk server.</li>"
        "<li>Add an AOR + auth + endpoint trio to <code>/etc/asterisk/pjsip.conf</code> for this "
        "extension number: <code>disallow=all</code>, <code>allow=ulaw,alaw,opus</code>, "
        "<code>identify_by=username</code>, and whatever dialplan <code>context=</code> your "
        "server uses -- or just download the setup script above and read it, it's a plain bash "
        "script that writes exactly this.</li>"
        "<li>Add a matching <code>exten =&gt; &lt;number&gt;,1,Dial(PJSIP/&lt;number&gt;,20)</code> "
        "/ <code>same =&gt; n,Hangup()</code> entry to that context in "
        "<code>extensions.conf</code>.</li>"
        "<li><code>asterisk -rx \"pjsip reload\"</code> and "
        "<code>asterisk -rx \"dialplan reload\"</code>.</li>"
        "<li>Back here: same server/extension/password into the Line fields above, Save, reboot "
        "the phone, confirm \"registered\" above.</li>"
        "<li>Running FreePBX instead of plain Asterisk? Add the extension through its GUI "
        "(Applications &rarr; Extensions &rarr; + Add Extension &rarr; PJSIP) and click "
        "<b>Apply Config</b> -- same idea, different admin tool.</li>"
        "</ol></details>"
        "<p class=\"hint\">Embedded SIP/RTP (G.711 &micro;-law), custom UA -- no TLS/SRTP "
        "(LAN only).</p></div>");

    /* 2. Check-before-you-commit: separate tiny form (its own number field,
     * not tied to the Save form above) so you can try a few candidates
     * without touching your actual saved config. Server-rendered redirect-
     * with-result, same no-JS pattern as the rest of this page. Fires a SIP
     * OPTIONS at the currently-saved SIP server (voip_check_handler) -- no
     * separate "server" field here: it used to duplicate the one in the
     * Save form above with different wording, which just read as two
     * different settings. */
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<div class=\"card wide\"><h2>2. Check a number</h2>"
        "<p class=\"hint\">See <code>archive/VOIP_SETUP.md</code> for this project's shared numbering "
        "registry, then confirm a candidate is actually free here before you commit to it.</p>"
        "<form method=\"POST\" action=\"/voip_check\" class=\"settings-form\">"
        "<label>Number to check <input name=\"number\" placeholder=\"e.g. 1101\" "
        "pattern=\"[0-9]{2,6}\" required></label>"
        "<div class=\"toolbar sub\"><button type=\"submit\">Check availability</button></div>"
        "</form>");
    if (checked && checked[0]) {
        const char *cls = strcmp(result, "free") == 0 ? "on" : "off";
        const char *label = strcmp(result, "free") == 0 ? "nothing answered -- looks free" :
                strcmp(result, "taken") == 0 ? "in use, or not confirmable" :
                "couldn't tell (server unreachable)";
        off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
            "<div class=\"statusrow\"><span class=\"label\">%s</span>"
            "<span class=\"pill %s\">%s</span></div>", checked, cls, label);
    }
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<p class=\"hint\">Sends a SIP OPTIONS to that number on %s, nothing more. Only a clean "
        "404 Not Found counts as \"free\"; that's the one answer this can trust. On stock "
        "Asterisk/PJSIP (<code>identify_by=username</code>), an unrecognized number usually gets "
        "challenged for auth (401) exactly like a real endpoint with the wrong password would -- "
        "so \"in use, or not confirmable\" often just means Asterisk couldn't be bothered to say "
        "no, not that the number is actually taken. Treat a non-free result as inconclusive, and "
        "confirm on the server itself: "
        "<code>asterisk -rx \"pjsip show endpoint &lt;number&gt;\"</code>.</p></div>",
        voip_server()[0] ? voip_server() : "(no SIP server saved yet)");

    /* Unrelated to placing calls -- this secret only gates host-side tooling
     * (gen_prompt.py --sync, GET /api/prompt_requests off-LAN; see
     * append_prompt_field()'s own comment). Kept on this tab since it's the
     * only place a "shared per-board secret" naturally lives, now that the
     * auto-provisioning flow that originally introduced it is gone. */
    bool setup_pw_set = voip_setup_password()[0] != '\0';
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<div class=\"card wide\"><h2>Setup password</h2>"
        "<p class=\"hint\">Not needed to make calls -- this is a shared secret for host-side "
        "tools that talk to this board directly, e.g. <code>gen_prompt.py --sync</code> "
        "(see the Dial Codes/Operator tabs' prompt-text fields).</p>"
        "<form method=\"POST\" action=\"/voip_setup_password\" class=\"settings-form\">"
        "<label>Setup password"
        "<input type=\"password\" name=\"password\" placeholder=\"%s\">"
        "<span class=\"fieldhint\">%s</span>"
        "</label>"
        "<div class=\"toolbar sub\"><button type=\"submit\">Save</button></div>"
        "</form></div>",
        setup_pw_set ? "\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2" : "not set",
        setup_pw_set ? "Already set on this board -- leave blank to keep it, or type a new one to replace it."
                     : "Not set yet.");

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
    uint32_t max_ms = board_record_max_ms_get(120000);
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<p class=\"hint\">Dial 211: if the prompt below is generated, it plays first, then a "
        "beep, then recording starts. Speak, then hang up (or pause ~8 s / hit the max length "
        "below, at which point recording just stops silently -- no warning beeps). Mention the "
        "time limit in the prompt text below if callers should know it up front. The message is "
        "saved as an 8 kHz mono WAV in <a href=\"/recordings\">Recordings</a> / Storage below.</p>"
        "<form method=\"POST\" action=\"/record_max_ms\" class=\"settings-form\">"
        "<label>Max recording length (seconds) "
        "<input type=\"number\" name=\"seconds\" value=\"%u\" min=\"%u\" max=\"%u\" required></label>"
        "<div class=\"toolbar sub\"><button type=\"submit\" class=\"primary\">Save</button></div>"
        "</form>",
        (unsigned)(max_ms / 1000),
        (unsigned)(BOARD_RECORD_MAX_MS_MIN / 1000), (unsigned)(BOARD_RECORD_MAX_MS_MAX / 1000));
    off = append_prompt_field(buf, cap, off, RECORD_PROMPT_NAME, NULL);
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0, "</div>");
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

    /* microSD row: only shown at all if this board is even configured for
     * it (board_hw_config_get().has_microsd -- a 2-line board, or a 1-line
     * board still on the safe unconfirmed default, has no SD tradeoff to
     * report on). Distinguishes "configured but no card detected" from
     * "present" explicitly, rather than just folding it into the Storage
     * row's existing "(SD)" suffix -- that suffix already means "SD is
     * primary right now," which says nothing when it isn't. */
    char sd_row[160] = "";
    if (board_hw_config_get().has_microsd) {
        if (storage_sd_present()) {
            char sdc[16];
            snprintf(sd_row, sizeof(sd_row),
                "<div class=\"statusrow\"><span class=\"label\">microSD</span>"
                "<span class=\"pill on\">present, %s</span></div>",
                fmt_bytes(storage_cap_bytes_sd(), sdc, sizeof(sdc)));
        } else {
            snprintf(sd_row, sizeof(sd_row),
                "<div class=\"statusrow\"><span class=\"label\">microSD</span>"
                "<span class=\"pill off\">not detected -- inserted &amp; FAT-formatted?</span></div>");
        }
    }

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
        "%s"
        "<div class=\"statusrow\"><span class=\"label\">Storage used / free</span>"
        "<span>%s / %s%s</span></div>"
        "<p class=\"hint\">Type is fixed at build time (<code>CONFIG_BOARD_TYPE_*</code>) since "
        "GPIO routing is per-PCB; the detected codec is probed live on the I2C bus and warns in "
        "the log if it disagrees. Serial is the factory MAC -- unique per chip, survives reflash. "
        "Flash is the whole chip; Storage is whichever backend is primary (the microSD card "
        "above, if present, otherwise internal flash) -- see Storage below for both listed "
        "separately. RAM is live internal heap. Uptime/WiFi are on the <a href=\"/\">Status</a> "
        "page.</p></div>",
        hw, board_codec_name(detected), serial, esp_get_idf_version(),
        fmt_bytes(flash_bytes, fl, sizeof(fl)),
        fmt_bytes(ram_used, ru, sizeof(ru)), fmt_bytes(ram_free, rf, sizeof(rf)), sd_row,
        fmt_bytes(st_used, su, sizeof(su)), fmt_bytes(st_free, sf, sizeof(sf)),
        storage_using_sd() ? " (SD)" : "");
}

/* SMD2-only: the "line 2 or microSD" GPIO18-21 config card. THT has no such
 * tradeoff (dedicated pins for both lines, no SD at all), so this whole card
 * is skipped there -- board_hw_config_get() already hardcodes (2, false) for
 * THT and there's nothing for a human to confirm. */
static int append_hw_config_tab(char *buf, size_t cap, int off)
{
    if (board_current_type() != BOARD_TYPE_SMD) return off;

    board_hw_config_t cfg = board_hw_config_get();
    bool confirmed = board_hw_config_is_set();

    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<div class=\"card\"><h2>Line 2 / microSD configuration</h2>"
        "<p class=\"hint\">This board's GPIO 18-21 are wired to <em>either</em> a second "
        "SLIC (line 2) <em>or</em> a microSD socket, never both -- whichever set of 0R "
        "jumpers got populated when the board was assembled (see "
        "<code>ROTARY_PHONE_SMD.md</code>, \"Build options\"). Firmware can't see the "
        "jumpers themselves, so it has to be told which one is actually stuffed. Getting "
        "this wrong doesn't just misbehave in software -- it drives real GPIO pins as the "
        "wrong peripheral. Until confirmed, this board defaults to the safe option: 1 line, "
        "no microSD.</p>"
        "<div class=\"statusrow\"><span class=\"label\">Current setting</span><span>%s%s</span></div>",
        cfg.num_lines > 1 ? "2-line board (0R jumpers on line 2 populated)"
                           : (cfg.has_microsd ? "1-line board, microSD populated"
                                               : "1-line board, no microSD"),
        confirmed ? "" : " -- <strong>unconfirmed default, please check below</strong>");

    if (!confirmed) {
        board_hw_evidence_t ev = board_hw_detect_evidence();
        if (ev.valid) {
            off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
                "<div class=\"statusrow\"><span class=\"label\">Advisory evidence</span>"
                "<span>GPIO18-21 raw levels: %d %d %d %d -- %s</span></div>"
                "<p class=\"hint\">Passive read of the shared pins before anything has "
                "claimed them, offered as a hint only -- not proof, and never applied "
                "automatically. Please confirm which board you actually have below.</p>",
                ev.raw_levels[0], ev.raw_levels[1], ev.raw_levels[2], ev.raw_levels[3],
                ev.looks_like_line2 ? "looks more like a populated line 2 (SHK divider idle-low)"
                                     : "looks more like an unpopulated/floating line 2 (no SD "
                                       "hardware would explain this either)");
        }
    }

    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<form method=\"POST\" action=\"/board_hw_set\" class=\"settings-form\">"
        "<label>Board variant"
        "<select name=\"variant\">"
        "<option value=\"2line\"%s>2-line board (0R jumpers present, line 2 populated)</option>"
        "<option value=\"1line_sd\"%s>1-line board (jumpers removed), microSD populated</option>"
        "<option value=\"1line_nosd\"%s>1-line board (jumpers removed), no microSD</option>"
        "</select></label>"
        "<button type=\"submit\">Save (takes effect on reboot)</button>"
        "</form></div>",
        cfg.num_lines > 1 ? " selected" : "",
        (cfg.num_lines == 1 && cfg.has_microsd) ? " selected" : "",
        (cfg.num_lines == 1 && !cfg.has_microsd) ? " selected" : "");

    return off;
}

/* Diagnostics tab: runtime log level (and, in test builds, test-mode
 * controls -- see Phase 2). */
static int append_diag_tab(char *buf, size_t cap, int off)
{
    static const char *LEVELS[] = { "none", "error", "warn", "info", "debug", "verbose" };
    esp_log_level_t cur = logctl_get_level();

    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<div class=\"card\"><h2>Logging</h2>"
        "<div class=\"statusrow\"><span class=\"label\">Current level</span>"
        "<span class=\"pill on\">%s</span></div>"
        "<form method=\"POST\" action=\"/log_level\" class=\"settings-form\">"
        "<label>Level <select name=\"level\">",
        logctl_level_name(cur));
    for (size_t i = 0; i < sizeof(LEVELS) / sizeof(LEVELS[0]); i++) {
        off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
            "<option value=\"%s\"%s>%s</option>",
            LEVELS[i], ((size_t)cur == i) ? " selected" : "", LEVELS[i]);
    }
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "</select></label>"
        "<div class=\"toolbar sub\"><button type=\"submit\" class=\"primary\">Set level</button></div>"
        "</form>"
        "<p class=\"hint\">Applies immediately and persists across reboots (NVS). "
        "<code>debug</code> logs rotary pulse timing, hook-switch make/break with durations, "
        "dialed-digit collection, and telephony state changes -- watch it live in the log "
        "below, or over the serial console (<code>log_level debug</code>). WiFi/PHY noise "
        "stays at warn so it doesn't bury the app's own debug output.</p></div>");

    /* Test mode + audio loopback + SD self-test (DVT harness controls; also
     * usable by hand here). */
    bool tmode = telephony_test_enabled();
    bool loop  = audio_test_loopback();
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<div class=\"card\"><h2>Test mode</h2>"
        "<div class=\"statusrow\"><span class=\"label\">Injection</span>"
        "<span class=\"pill %s\">%s</span></div>"
        "<div class=\"toolbar sub\">"
        "<form method=\"POST\" action=\"/test/mode\" style=\"display:inline\">"
        "<input type=\"hidden\" name=\"on\" value=\"1\"><button type=\"submit\">Enable</button></form>"
        "<form method=\"POST\" action=\"/test/mode\" style=\"display:inline\">"
        "<input type=\"hidden\" name=\"on\" value=\"0\"><button type=\"submit\">Disable</button></form>"
        "</div>"
        "<div class=\"statusrow\"><span class=\"label\">Audio TX&rarr;RX loopback</span>"
        "<span class=\"pill %s\">%s</span></div>"
        "<div class=\"toolbar sub\">"
        "<form method=\"POST\" action=\"/test/loopback\" style=\"display:inline\">"
        "<input type=\"hidden\" name=\"on\" value=\"1\"><button type=\"submit\">Enable</button></form>"
        "<form method=\"POST\" action=\"/test/loopback\" style=\"display:inline\">"
        "<input type=\"hidden\" name=\"on\" value=\"0\"><button type=\"submit\">Disable</button></form>"
        "</div>"
        "<div class=\"toolbar sub\">"
        "<form method=\"POST\" action=\"/test/sd\" style=\"display:inline\">"
        "<button type=\"submit\">Run microSD self-test</button></form>"
        "</div>"
        "<p class=\"hint\">Test mode lets the design-verification harness simulate a handset "
        "(off-hook, hang-up, dialed digits) without physical dialing. Loopback feeds "
        "synthesized TX audio back into the RX detector so a played tone/DTMF is confirmable "
        "without a handset. Leave both <b>off</b> for normal use. Full protocol + host runner: "
        "<code>firmware/test/</code>.</p></div>",
        tmode ? "on" : "off", tmode ? "ENABLED" : "off",
        loop  ? "on" : "off", loop  ? "ENABLED" : "off");

    int num_lines = board_hw_config_get().num_lines;

#if CONFIG_BOARD_TYPE_SMD2
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<div class=\"card\"><h2>Speaker (earpiece) volume</h2>");
    for (int l = 0; l < num_lines; l++) {
        uint8_t vol = board_speaker_volume_get(l, 0x1E);
        /* Displayed as a percentage (0-200%, 100% = the register's 0dB
         * point) rather than the raw 0-63/dB-step register value -- dB
         * isn't a meaningful unit for most people tuning this by ear. The
         * underlying value submitted to the backend is still the raw 0-63
         * register step; only the label is converted (see spkPct() below),
         * so es8388_set_dac_volume()'s convention doesn't have to change. */
        unsigned pct = (unsigned)((vol * 200u + 31) / 63u);
        off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
            "<form method=\"POST\" action=\"/speaker_volume\" class=\"settings-form\">"
            "<input type=\"hidden\" name=\"line\" value=\"%d\">"
            "<label>Line %d <input type=\"range\" name=\"vol\" min=\"0\" max=\"63\" "
            "value=\"%u\" oninput=\"spkLive(%d,this.value)\"> "
            "<span id=\"spkpct%d\">%u%%</span></label>"
            "<div class=\"toolbar sub\"><button type=\"submit\" class=\"primary\">Save line %d</button></div>"
            "</form>",
            l, l, (unsigned)vol, l, l, pct, l);
    }
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<p class=\"hint\">Drag while on a call to hear the change live (applied "
        "immediately, not saved yet) -- click Save once you land on a level you like. "
        "100%% is the codec's 0dB reference point, 200%% is max. Same register the "
        "bring-up <code>spkamp</code> console command tunes.</p></div>");
#endif

    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<div class=\"card\"><h2>Echo cancellation</h2>");
    for (int l = 0; l < num_lines; l++) {
        uint32_t ms = board_aec_delay_get(l, AEC_DEFAULT_DELAY_MS);
        bool aec_on = aec_is_enabled(l);
        off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
            "<div class=\"statusrow\"><span class=\"label\">Line %d</span>"
            "<span class=\"pill %s\">%s</span></div>"
            "<div class=\"toolbar sub\">"
            "<form method=\"POST\" action=\"/aec_enable\" style=\"display:inline\">"
            "<input type=\"hidden\" name=\"line\" value=\"%d\">"
            "<input type=\"hidden\" name=\"on\" value=\"1\">"
            "<button type=\"submit\">Enable</button></form> "
            "<form method=\"POST\" action=\"/aec_enable\" style=\"display:inline\">"
            "<input type=\"hidden\" name=\"line\" value=\"%d\">"
            "<input type=\"hidden\" name=\"on\" value=\"0\">"
            "<button type=\"submit\">Disable</button></form>"
            "</div>"
            "<div class=\"statusrow\"><span class=\"label\">Line %d delay</span>"
            "<span>%u ms</span></div>"
            "<div class=\"toolbar sub\">"
            "<button type=\"button\" id=\"aecbtn%d\" onclick=\"aecMeasure(%d)\">Measure line %d</button>"
            "</div><div id=\"aecresult%d\" class=\"hint\"></div>",
            l, aec_on ? "on" : "off", aec_on ? "ENABLED" : "off",
            l, l, l, (unsigned)ms, l, l, l, l);
    }
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<p class=\"hint\">Enable/Disable is for testing -- leave off for normal use "
        "unless a real echo has actually been measured and calibrated below (an "
        "uncalibrated or wrongly-delayed canceller can make echo worse, not better). "
        "To calibrate: pick up the handset (off-hook) and make sure it's NOT in a "
        "call, then click Measure -- plays a short burst out the earpiece and listens on "
        "the same line's own mic to find its local echo delay, the number the canceller "
        "needs to align against. Takes about 2 seconds (3 quick measurements averaged for "
        "reliability). Applying saves it and takes effect immediately, no reboot.</p></div>"
        "<script>"
        "function spkLive(line,vol){"
        "document.getElementById('spkpct'+line).textContent="
        "Math.round(vol*200/63)+'%%';"
        "fetch('/speaker_volume_live',{method:'POST',"
        "headers:{'Content-Type':'application/x-www-form-urlencoded'},"
        "body:'line='+line+'&vol='+vol});}"
        "function aecMeasure(line){"
        "var btn=document.getElementById('aecbtn'+line);"
        "var out=document.getElementById('aecresult'+line);"
        "btn.disabled=true;out.textContent='Measuring...';"
        "fetch('/aec_measure',{method:'POST',"
        "headers:{'Content-Type':'application/x-www-form-urlencoded'},"
        "body:'line='+line})"
        ".then(function(r){return r.json();})"
        ".then(function(d){btn.disabled=false;"
        "if(!d.ok){out.textContent='No echo detected in '+d.hits+'/'+d.attempts+"
        "' tries -- off-hook and not in a call?';return;}"
        "var ms=Math.round(d.delay_ms);"
        "out.innerHTML='Delay '+d.delay_ms.toFixed(1)+'ms, tail '+d.tail_ms.toFixed(1)+"
        "'ms, span ~'+d.span_ms.toFixed(0)+'ms ('+Math.round(d.taps)+' taps), '+"
        "d.hits+'/'+d.attempts+' consistent. '+"
        "'<form method=\"POST\" action=\"/aec_delay_apply\" style=\"display:inline\">'+"
        "'<input type=\"hidden\" name=\"line\" value=\"'+line+'\">'+"
        "'<input type=\"hidden\" name=\"ms\" value=\"'+ms+'\">'+"
        "'<button type=\"submit\">Apply '+ms+'ms as default</button></form>';})"
        ".catch(function(){btn.disabled=false;out.textContent='Measurement failed.';});"
        "}"
        "</script>");

    /* Full debug log -- moved here from the Status page (which now only
     * shows the much shorter activity panel, see append_activity_panel())
     * since Diagnostics already has the log-level controls right above and
     * is a much more natural home for the verbose raw log than the
     * at-a-glance Status page. append_log_panel()/append_log_line() and the
     * /api/log endpoint are unchanged, just called from here instead. */
    size_t log_cap = 8200;
    char *log_snapshot = malloc(log_cap);
    if (log_snapshot) {
        logbuf_snapshot(log_snapshot, log_cap);
        off = append_log_panel(buf, cap, off, log_snapshot);
        free(log_snapshot);
    }
    return off;
}

/* Renders just the <table> (or "none yet") for one backend's file list --
 * shared by the microSD and internal-flash cards below, since the row
 * markup (name/size/play/download/delete) doesn't depend on which backend
 * the entries came from. */
/* Reads just enough of a stored WAV's header to compute its duration --
 * short reads are fine, audio_wav_parse() is a tolerant chunk walker (see
 * its own doc comment). UPLOAD_HEADER_SCAN_BYTES (not a smaller guess): a
 * canonical WAV header is 44 bytes, but real-world files can carry several
 * KB of filler/metadata before `data` (macOS afconvert's FLLR chunk,
 * observed on this project's own clips pushing it out ~4KB) -- confirmed
 * live that 512 bytes silently produced a blank duration for such a file.
 * Heap-allocated per UPLOAD_HEADER_SCAN_BYTES's own doc comment (never an
 * 8KB stack array here -- this runs inside append_storage_table()'s
 * per-row loop, itself nested inside the settings page handler, both of
 * which already have their own locals on the same stack). Blank output
 * (not an error) for anything that doesn't parse as PCM WAV -- this is a
 * display nicety, not something worth failing the whole page render over. */
static void wav_duration_str(const char *name, char *out, size_t out_len)
{
    out[0] = '\0';
    uint8_t *header = malloc(UPLOAD_HEADER_SCAN_BYTES);
    if (!header) return;
    size_t got = 0;
    esp_err_t err = storage_read(name, header, UPLOAD_HEADER_SCAN_BYTES, &got);
    if (err != ESP_OK || got == 0) { free(header); return; }
    audio_wav_info_t wav;
    bool ok = audio_wav_parse(header, got, &wav) && wav.sample_rate != 0 &&
              wav.channels != 0 && wav.bits_per_sample != 0;
    free(header);
    if (!ok) return;
    double secs = (double)wav.data_bytes /
        ((double)wav.sample_rate * wav.channels * (wav.bits_per_sample / 8));
    if (secs >= 60.0) {
        snprintf(out, out_len, "%u:%02u", (unsigned)(secs / 60), (unsigned)secs % 60);
    } else {
        snprintf(out, out_len, "%.1fs", secs);
    }
}

/* Parses a recording filename in telephony.c's rec_name_timestamped() format
 * -- "rec_l<N>_YYYYMMDD_HHMMSSZ.wav" once the system clock has synced via
 * SNTP -- into a readable "YYYY-MM-DD HH:MM UTC" timestamp. Before sync,
 * that function instead names the file "rec_l<N>_boot<ms>.wav" (no real date
 * embedded, just a boot-relative tag to avoid collisions); for that shape
 * this reports "recorded before clock sync" rather than trying to parse a
 * date out of it. Blank output (not an error) for anything matching
 * neither shape -- a display nicety for the Recordings page's "Recorded"
 * column, not worth failing the row render over. */
static void format_recording_timestamp(const char *name, char *out, size_t len)
{
    if (len == 0) return;
    out[0] = '\0';

    const char *p = name;
    if (strncmp(p, "rec_l", 5) != 0) return;
    p += 5;
    while (*p >= '0' && *p <= '9') p++; /* line number */
    if (*p != '_') return;
    p++;

    if (strncmp(p, "boot", 4) == 0) {
        snprintf(out, len, "recorded before clock sync");
        return;
    }

    int y, mo, d, h, mi, s;
    if (sscanf(p, "%4d%2d%2d_%2d%2d%2dZ", &y, &mo, &d, &h, &mi, &s) == 6) {
        snprintf(out, len, "%04d-%02d-%02d %02d:%02d UTC", y, mo, d, h, mi);
    }
}

/* Shared table renderer for both the Settings -> Storage file manager
 * (unfiltered, no "Recorded" column -- it lists every kind of file, not just
 * timestamped recordings) and the Recordings page (filtered to `name_prefix`
 * -- "rec_", see is_user_recording() -- with `show_recorded` on). One code
 * path, not two near-duplicate table builders. `name_prefix` NULL/"" means
 * no filtering. `return_to` is embedded as each row's delete form's hidden
 * "return_to" field so media_delete_handler() sends the browser back to
 * whichever page it was deleted from (/recordings vs /settings?tab=storage)
 * instead of always assuming Settings -- see that handler's own comment. */
static int append_storage_table(char *buf, size_t cap, int off,
                                storage_entry_t *entries, size_t n,
                                const char *name_prefix, bool show_recorded,
                                const char *return_to)
{
    size_t plen = name_prefix ? strlen(name_prefix) : 0;
    size_t shown = 0;
    for (size_t i = 0; i < n; i++) {
        if (plen && strncmp(entries[i].name, name_prefix, plen) != 0) continue;
        shown++;
    }
    if (shown == 0) {
        off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
            "<div class=\"empty\">none yet</div>");
        return off;
    }
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<table><tr><th>Name</th>%s<th>Size</th><th>Length</th><th>Play</th><th></th><th></th></tr>",
        show_recorded ? "<th>Recorded</th>" : "");
    for (size_t i = 0; i < n && off > 0 && (size_t)off < cap; i++) {
        if (plen && strncmp(entries[i].name, name_prefix, plen) != 0) continue;
        char sz[16];
        char dur[16];
        char play[160];
        char recorded[32] = "";
        if (is_wav_name(entries[i].name) || is_mp3_name(entries[i].name)) {
            snprintf(play, sizeof(play),
                "<audio controls preload=\"none\" src=\"/media/%s?inline=1\"></audio>",
                entries[i].name);
            /* wav_duration_str() reads a RIFF/WAV header -- doesn't apply to
             * MP3's frame-based format, and the browser's own <audio>
             * element shows its duration once loaded anyway, so this just
             * stays blank rather than computing/guessing one. */
            if (is_wav_name(entries[i].name)) wav_duration_str(entries[i].name, dur, sizeof(dur));
            else dur[0] = '\0';
        } else {
            play[0] = '\0';
            dur[0] = '\0';
        }
        if (show_recorded) format_recording_timestamp(entries[i].name, recorded, sizeof(recorded));
        char recorded_td[48] = "";
        if (show_recorded) snprintf(recorded_td, sizeof(recorded_td), "<td>%s</td>", recorded);
        off += snprintf(buf + off, cap - off,
            "<tr><td><code>%s</code></td>%s<td>%s</td><td>%s</td><td>%s</td>"
            "<td><a href=\"/media/%s\">download</a></td>"
            "<td><form method=\"POST\" action=\"/media_delete/%s\" "
            "onsubmit=\"return confirm('Delete %s?')\" style=\"display:inline\">"
            "<input type=\"hidden\" name=\"return_to\" value=\"%s\">"
            "<button type=\"submit\">delete</button></form></td></tr>",
            entries[i].name, recorded_td, fmt_bytes(entries[i].size, sz, sizeof(sz)), dur, play,
            entries[i].name, entries[i].name, entries[i].name, return_to);
    }
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0, "</table>");
    return off;
}

/* Storage file manager -- lists every file in the storage layer(s):
 * recordings, uploaded prompt/clip WAVs, anything else that landed there.
 * No type distinction in storage.c itself -- it's all just files -- so this
 * stays outside the tabs (clip uploads, operator.wav, and recordings all
 * land here together; no single tab owns it).
 *
 * One card (internal flash) normally; a microSD card adds a second, shown
 * first since it's primary for new recordings/uploads once present (see
 * storage.h) -- but the internal-flash card stays visible too, so anything
 * saved before the card was inserted doesn't just vanish from view. Both
 * tables' delete/download/play links work regardless of which card they're
 * in: storage.c's storage_read_open()/storage_delete() search both backends
 * by name, not just whichever is currently primary. */
static int append_storage_section(char *buf, size_t cap, int off)
{
    char ip[16];
    wifi_get_ip_str(ip, sizeof(ip));
    bool sd = storage_sd_present();

    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<div id=\"storage\" class=\"card wide\" style=\"margin-top:16px\"><h2>Storage</h2>"
        "<p class=\"hint\">Recordings, uploaded prompt/clip WAVs, anything else. Upload with "
        "<code>curl -X POST --data-binary @file.wav http://%s/media/&lt;name&gt;</code> or "
        "<code>firmware/bringup/gen_prompt.py</code>. <code>operator.wav</code> and "
        "<code>clip_*.wav</code> are never auto-deleted to make room for recordings -- only "
        "explicit deletes (below) remove them. Prompt/clip uploads always go to internal "
        "flash (so pulling the card mid-use can't take dial tone or the operator message "
        "with it) -- only 211 recordings prefer the card.%s</p></div>",
        ip, sd ? " A microSD card is present and takes priority for new recordings "
                 "-- internal flash is never evicted to make room on the card, and stays "
                 "listed below in its own section so anything saved before the card was "
                 "inserted is still reachable." : "");

    if (sd) {
        storage_entry_t sd_entries[32];
        size_t sd_n = storage_list_sd(sd_entries, 32);
        char su[16], sc[16];
        off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
            "<div class=\"card wide\" style=\"margin-top:16px\"><h2>microSD (%u file%s)</h2>"
            "<p class=\"hint\">%s / %s used. New recordings and uploads go here.</p>",
            (unsigned)sd_n, sd_n == 1 ? "" : "s",
            fmt_bytes(storage_used_bytes_sd(), su, sizeof(su)),
            fmt_bytes(storage_cap_bytes_sd(), sc, sizeof(sc)));
        off = append_storage_table(buf, cap, off, sd_entries, sd_n, NULL, false, "/settings?tab=storage");
        off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0, "</div>");
    }

    storage_entry_t in_entries[32];
    size_t in_n = storage_list_internal(in_entries, 32);
    char isu[16], isc[16];
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<div class=\"card wide\" style=\"margin-top:16px\"><h2>%s (%u file%s)</h2>"
        "<p class=\"hint\">%s / %s used.%s</p>",
        sd ? "Internal flash" : "Storage files",
        (unsigned)in_n, in_n == 1 ? "" : "s",
        fmt_bytes(storage_used_bytes_internal(), isu, sizeof(isu)),
        fmt_bytes(storage_cap_bytes_internal(), isc, sizeof(isc)),
        sd ? "" : " Capped at this size -- oldest recording evicted first once it's hit "
                  "(uploaded clips/operator.wav are never auto-evicted).");
    off = append_storage_table(buf, cap, off, in_entries, in_n, NULL, false, "/settings?tab=storage");
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0, "</div>");

    return off;
}

static esp_err_t settings_page_handler(httpd_req_t *req)
{
    char tab[16] = "general";
    char checked[8] = "", result[8] = "";
    char created[8] = "", createresult[16] = "";
    char query[144];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "tab", tab, sizeof(tab));
        httpd_query_key_value(query, "checked", checked, sizeof(checked));
        httpd_query_key_value(query, "result", result, sizeof(result));
        httpd_query_key_value(query, "created", created, sizeof(created));
        httpd_query_key_value(query, "createresult", createresult, sizeof(createresult));
    }
    /* checked/result/created/createresult land directly in the VOIP tab's
     * HTML (no escaping) -- this GET's query string is otherwise
     * attacker-controlled input (unlike the values the POST handlers above
     * generate for their own redirects), so validate defensively here too
     * rather than trusting the redirect that normally produces them. */
    {
        size_t clen = strlen(checked);
        bool cok = clen >= 2 && clen <= 6;
        for (size_t i = 0; cok && i < clen; i++) cok = checked[i] >= '0' && checked[i] <= '9';
        if (!cok) checked[0] = '\0';
        if (strcmp(result, "free") != 0 && strcmp(result, "taken") != 0 && strcmp(result, "unknown") != 0) {
            result[0] = '\0';
        }
        if (!checked[0] || !result[0]) { checked[0] = '\0'; result[0] = '\0'; }

        size_t dlen = strlen(created);
        bool dok = dlen >= 2 && dlen <= 6;
        for (size_t i = 0; dok && i < dlen; i++) dok = created[i] >= '0' && created[i] <= '9';
        if (!dok) created[0] = '\0';
        if (strcmp(createresult, "ok") != 0 && strcmp(createresult, "exists") != 0 &&
            strcmp(createresult, "needsreload") != 0 && strcmp(createresult, "failed") != 0) {
            createresult[0] = '\0';
        }
        if (!created[0] || !createresult[0]) { created[0] = '\0'; createresult[0] = '\0'; }
    }
    /* Old bookmarks/links to tabs folded into a bigger one during the
     * Settings cleanup -- send them to the tab that now has that content,
     * not back to General (the unknown-tab fallback below). */
    if (strcmp(tab, "operator") == 0 || strcmp(tab, "record") == 0) {
        strncpy(tab, "dialcodes", sizeof(tab) - 1);
    } else if (strcmp(tab, "storage") == 0) {
        strncpy(tab, "hardware", sizeof(tab) - 1);
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
     * handler's stack arrays is what overflowed the httpd task's stack.
     * Diagnostics also needs room for the full log panel (moved here from
     * the Status page, see append_diag_tab()) -- each raw log line can
     * roughly triple in size once wrapped in .logline/.t/.k/.m markup. */
    size_t log_room = (strcmp(tab, "diag") == 0) ? 8200 * 3 : 0;
    size_t cap = 6144 + n * 220 + 32 * 340 + log_room; /* storage rows now also carry an <audio> element */
    char *buf = malloc(cap);
    if (!buf) { httpd_resp_send_500(req); return ESP_FAIL; }

    int off = append_page_head(buf, cap, 0, "Settings", "/settings", false);
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "</header><main class=\"settings-main\">");
    off = append_settings_tabs(buf, cap, off, tab);
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0, "<div class=\"cards\">");

    if (strcmp(tab, "general") == 0) off = append_general_tab(buf, cap, off);
    /* Dial Codes & Prompts: the editable clip table, plus Operator help and
     * Record-message -- each just one card (prompt text + audio upload),
     * structurally the same thing as a Dial Codes row -- stacked below it.
     * Used to be three separate tabs. */
    else if (strcmp(tab, "dialcodes") == 0) {
        off = append_dialcodes_tab(buf, cap, off);
        off = append_operator_tab(buf, cap, off, entries, n);
        off = append_record_tab(buf, cap, off, entries, n);
    }
    else if (strcmp(tab, "voip") == 0) off = append_voip_tab(buf, cap, off, checked, result, created, createresult);
    else if (strcmp(tab, "diag") == 0) off = append_diag_tab(buf, cap, off);
    /* Hardware: stats + the SMD line2/microSD config card + the full
     * SD+internal file manager -- all "this board's physical resources,"
     * previously split across a Hardware tab and a separate Storage tab. */
    else {
        off = append_hw_config_tab(buf, cap, off);
        off = append_hardware_tab(buf, cap, off);
        off = append_storage_section(buf, cap, off);
    }

    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0, "</div>" /* .cards */);

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

/* POST /offhook_alert -- form body [enabled=1&]seconds=N. An unchecked
 * checkbox submits nothing at all, so a missing `enabled` field means "off"
 * -- it can't be treated as "leave unchanged" the way blank password fields
 * elsewhere in this file are. */
static esp_err_t offhook_alert_handler(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len >= WIFI_FORM_MAX_BODY) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad form body");
        return ESP_OK;
    }
    char body[WIFI_FORM_MAX_BODY];
    int r = httpd_req_recv(req, body, req->content_len);
    if (r <= 0) { httpd_resp_send_500(req); return ESP_FAIL; }
    body[r] = '\0';

    char enabled[8] = {0}, seconds[12] = {0};
    form_field(body, "enabled", enabled, sizeof(enabled));
    form_field(body, "seconds", seconds, sizeof(seconds));

    board_offhook_alert_t cfg = {
        .enabled = (enabled[0] == '1'),
        .seconds = seconds[0] ? (uint32_t)strtoul(seconds, NULL, 10) : 0,
    };
    if (board_offhook_alert_set(cfg) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "seconds out of range");
        return ESP_OK;
    }

    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/settings?tab=general");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* POST /idle_power -- form body enabled=1 (checkbox present) + minutes=N. */
static esp_err_t idle_power_settings_handler(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len >= WIFI_FORM_MAX_BODY) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad form body");
        return ESP_OK;
    }
    char body[WIFI_FORM_MAX_BODY];
    int r = httpd_req_recv(req, body, req->content_len);
    if (r <= 0) { httpd_resp_send_500(req); return ESP_FAIL; }
    body[r] = '\0';

    char enabled[8] = {0}, minutes[12] = {0};
    form_field(body, "enabled", enabled, sizeof(enabled));
    form_field(body, "minutes", minutes, sizeof(minutes));

    idle_power_cfg_t cfg = {
        .enabled = (enabled[0] == '1'),
        .minutes = minutes[0] ? (uint16_t)strtoul(minutes, NULL, 10) : 0,
    };
    if (idle_power_set(cfg) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "minutes out of range");
        return ESP_OK;
    }

    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/settings?tab=general");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* POST /record_max_ms -- form body seconds=N (whole seconds; board.h stores
 * ms). Mirrors offhook_alert_handler's shape. */
static esp_err_t record_max_ms_handler(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len >= WIFI_FORM_MAX_BODY) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad form body");
        return ESP_OK;
    }
    char body[WIFI_FORM_MAX_BODY];
    int r = httpd_req_recv(req, body, req->content_len);
    if (r <= 0) { httpd_resp_send_500(req); return ESP_FAIL; }
    body[r] = '\0';

    char seconds[12] = {0};
    form_field(body, "seconds", seconds, sizeof(seconds));
    uint32_t ms = seconds[0] ? (uint32_t)strtoul(seconds, NULL, 10) * 1000 : 0;
    if (board_record_max_ms_set(ms) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "seconds out of range");
        return ESP_OK;
    }

    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/settings?tab=dialcodes");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* POST /board_hw_set -- form body variant=2line|1line_sd|1line_nosd. The
 * human's explicit confirmation of which GPIO18-21 peripheral is actually
 * populated on this board (board.h's board_hw_config_set()) -- see that
 * file's doc comment for why this can't be guessed automatically. Takes
 * effect on reboot, same as every other hardware config in this project. */
static esp_err_t board_hw_set_handler(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len >= WIFI_FORM_MAX_BODY) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad form body");
        return ESP_OK;
    }
    char body[WIFI_FORM_MAX_BODY];
    int r = httpd_req_recv(req, body, req->content_len);
    if (r <= 0) { httpd_resp_send_500(req); return ESP_FAIL; }
    body[r] = '\0';

    char variant[16] = {0};
    form_field(body, "variant", variant, sizeof(variant));

    board_hw_config_t cfg;
    if (strcmp(variant, "2line") == 0) {
        cfg = (board_hw_config_t){ .num_lines = 2, .has_microsd = false };
    } else if (strcmp(variant, "1line_sd") == 0) {
        cfg = (board_hw_config_t){ .num_lines = 1, .has_microsd = true };
    } else if (strcmp(variant, "1line_nosd") == 0) {
        cfg = (board_hw_config_t){ .num_lines = 1, .has_microsd = false };
    } else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unknown variant");
        return ESP_OK;
    }
    board_hw_config_set(cfg);

    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/settings?tab=hardware");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* POST /voip_set -- form body server=..&user0=..&pass0=..[&user1=..&pass1=..].
 * Blank password fields mean "leave unchanged" (voip_set_config() already
 * treats a NULL field that way; empty-string form fields are passed as
 * NULL here for the same reason the WiFi form does). user1 is the one
 * exception: unlike a password, there's no need to hide it, so a blank
 * user1 field is passed through as "" (not collapsed to NULL) -- this is
 * how you actually clear a stale/unused line 2, e.g. a 1-line board that
 * still has a leftover line-2 account from before it was reconfigured, and
 * otherwise retries registering it forever. Clearing user1 also clears
 * pass1 (an orphaned password serves no purpose once its extension is
 * gone). Takes effect on reboot, same as the console's sip_set command. */
#define VOIP_FORM_MAX_BODY 512

static esp_err_t voip_set_handler(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len >= VOIP_FORM_MAX_BODY) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad form body");
        return ESP_OK;
    }
    char body[VOIP_FORM_MAX_BODY];
    int r = httpd_req_recv(req, body, req->content_len);
    if (r <= 0) { httpd_resp_send_500(req); return ESP_FAIL; }
    body[r] = '\0';

    char server[64] = {0}, user0[32] = {0}, pass0[48] = {0}, user1[32] = {0}, pass1[48] = {0};
    form_field(body, "server", server, sizeof(server));
    form_field(body, "user0", user0, sizeof(user0));
    form_field(body, "pass0", pass0, sizeof(pass0));
    form_field(body, "user1", user1, sizeof(user1));
    form_field(body, "pass1", pass1, sizeof(pass1));

    voip_set_config(server[0] ? server : NULL, user0[0] ? user0 : NULL, pass0[0] ? pass0 : NULL,
                     user1, user1[0] ? (pass1[0] ? pass1 : NULL) : "");

    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/settings?tab=voip");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* POST /voip_check -- form body number=.. . Always checks against the
 * currently-*saved* SIP server (voip_server()) -- there used to be a
 * second "server" field here, independent of the one in the Save form
 * below with different wording ("to check against" vs "(host or IP)"),
 * which just read as two unrelated server settings instead of the same
 * value entered twice. Uses voip_probe_number()'s best-effort SIP OPTIONS
 * probe (blocks up to ~2s), which can only ever confirm "free", never
 * "taken" for sure -- see that function's own doc comment. Redirects back
 * to the VOIP tab with the result in the query string, same
 * request/redirect/re-render pattern as every other form on this page -- no
 * separate JSON/AJAX endpoint needed. */
static esp_err_t voip_check_handler(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len >= VOIP_FORM_MAX_BODY) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad form body");
        return ESP_OK;
    }
    char body[VOIP_FORM_MAX_BODY];
    int r = httpd_req_recv(req, body, req->content_len);
    if (r <= 0) { httpd_resp_send_500(req); return ESP_FAIL; }
    body[r] = '\0';

    char number[8] = {0};
    form_field(body, "number", number, sizeof(number));
    const char *server = voip_server();

    /* Digits only, 2-6 chars (matches the HTML pattern= and dialplan's own
     * valid_code() range) -- `number` lands straight in a redirect Location
     * header below, so this is a correctness AND a header-injection guard,
     * not just cosmetic. */
    size_t nlen = strlen(number);
    bool ok = nlen >= 2 && nlen <= 6;
    for (size_t i = 0; ok && i < nlen; i++) ok = number[i] >= '0' && number[i] <= '9';

    char loc[80];
    if (ok && server[0]) {
        voip_probe_result_t pr = VOIP_PROBE_UNKNOWN;
        voip_probe_number(server, number, &pr);
        const char *result = pr == VOIP_PROBE_FREE ? "free" : pr == VOIP_PROBE_TAKEN ? "taken" : "unknown";
        snprintf(loc, sizeof(loc), "/settings?tab=voip&checked=%s&result=%s", number, result);
    } else {
        snprintf(loc, sizeof(loc), "/settings?tab=voip");
    }
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", loc);
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* POST /voip_setup_password -- form body password=.. . Sets the shared
 * "setup" secret (voip_set_setup_password()) used by gen_prompt.py --sync
 * and GET /api/prompt_requests -- see voip_setup_password()'s doc comment
 * in voip.h. Takes effect immediately, no reboot. Blank means "leave
 * unchanged", same convention as the main VOIP form. */
static esp_err_t voip_setup_password_handler(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len >= VOIP_FORM_MAX_BODY) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad form body");
        return ESP_OK;
    }
    char body[VOIP_FORM_MAX_BODY];
    int r = httpd_req_recv(req, body, req->content_len);
    if (r <= 0) { httpd_resp_send_500(req); return ESP_FAIL; }
    body[r] = '\0';

    char password[80] = {0};
    form_field(body, "password", password, sizeof(password));
    if (password[0]) voip_set_setup_password(password);

    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/settings?tab=voip");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* POST /log_level -- form body level=<name>. Persists + applies (logctl). */
static esp_err_t log_level_handler(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len >= WIFI_FORM_MAX_BODY) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad form body");
        return ESP_OK;
    }
    char body[WIFI_FORM_MAX_BODY];
    int r = httpd_req_recv(req, body, req->content_len);
    if (r <= 0) { httpd_resp_send_500(req); return ESP_FAIL; }
    body[r] = '\0';

    char level[16] = {0};
    form_field(body, "level", level, sizeof(level));
    esp_log_level_t lvl = logctl_level_from_name(level);
    if (lvl > ESP_LOG_VERBOSE) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad level");
        return ESP_OK;
    }
    logctl_set_level(lvl);

    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/settings?tab=diag");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* ---- Test / DVT-harness endpoints ----
 * All plain-text responses ("ok\n" / an error string) so a host script can
 * parse them trivially; state is read back from /api/status. These are LAN
 * bring-up/test hooks with no auth, same posture as the rest of this UI. */

/* Reads a single urlencoded form field from the request body into `out`.
 * Returns false on a body that's missing/oversized/unreadable. */
static bool read_form_body(httpd_req_t *req, char *body, size_t cap)
{
    if (req->content_len == 0 || req->content_len >= cap) return false;
    int r = httpd_req_recv(req, body, req->content_len);
    if (r <= 0) return false;
    body[r] = '\0';
    return true;
}

/* POST /test/mode  body on=0|1 -- enable/disable telephony injection test mode. */
static esp_err_t test_mode_handler(httpd_req_t *req)
{
    char body[64], on[8] = {0};
    if (!read_form_body(req, body, sizeof(body))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body"); return ESP_OK;
    }
    form_field(body, "on", on, sizeof(on));
    telephony_test_set_enabled(on[0] == '1');
    /* Redirect so a browser form lands back on the tab; a host harness
     * ignores the redirect and confirms via GET /api/status (test_mode). */
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/settings?tab=diag");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* POST /test/loopback  body on=0|1 -- audio TX->RX digital loopback. */
static esp_err_t test_loopback_handler(httpd_req_t *req)
{
    char body[64], on[8] = {0};
    if (!read_form_body(req, body, sizeof(body))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body"); return ESP_OK;
    }
    form_field(body, "on", on, sizeof(on));
    audio_set_test_loopback(on[0] == '1');
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/settings?tab=diag");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* POST /test/inject  body type=offhook|onhook|digit & line=N [& digit=D]
 * -- simulate a handset event (requires test mode on). */
static esp_err_t test_inject_handler(httpd_req_t *req)
{
    char body[96], type[12] = {0}, line_s[8] = {0}, digit_s[8] = {0};
    if (!read_form_body(req, body, sizeof(body))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body"); return ESP_OK;
    }
    form_field(body, "type", type, sizeof(type));
    form_field(body, "line", line_s, sizeof(line_s));
    form_field(body, "digit", digit_s, sizeof(digit_s));
    int line = line_s[0] ? atoi(line_s) : 0;

    esp_err_t err;
    if (strcmp(type, "offhook") == 0)      err = telephony_test_hook(line, true);
    else if (strcmp(type, "onhook") == 0)  err = telephony_test_hook(line, false);
    else if (strcmp(type, "digit") == 0)   err = telephony_test_digit(line, digit_s[0]);
    else { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "type must be offhook|onhook|digit"); return ESP_OK; }

    if (err == ESP_ERR_INVALID_STATE) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "test mode is off (POST /test/mode on=1 first)");
    } else if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad line/digit");
    } else {
        httpd_resp_sendstr(req, "ok\n");
    }
    return ESP_OK;
}

/* POST /test/sd -- run the microSD write/read/verify self-test, mounting
 * first if needed. Plain-text result the harness can grep. */
static esp_err_t test_sd_handler(httpd_req_t *req)
{
    if (!sdcard_mounted()) sdcard_init();
    if (!sdcard_mounted()) { httpd_resp_sendstr(req, "sd: not mounted\n"); return ESP_OK; }
    esp_err_t err = sdcard_selftest();
    httpd_resp_sendstr(req, err == ESP_OK ? "sd: ok\n" : "sd: FAIL\n");
    return ESP_OK;
}

/* ---- Speaker (earpiece) volume: live tuning + persistence ----
 * Webui counterpart to the bring-up `spkamp` console command -- same
 * ES8388 LOUT1VOL/ROUT1VOL convention (0=mute, 33=0dB, 1dB/step), same
 * per-line NVS persistence (board_speaker_volume_get/set). ES8388/SMD2
 * only: LOUT1VOL/ROUT1VOL doesn't exist on the THT board's WM8731. */
#if CONFIG_BOARD_TYPE_SMD2
/* Sets one line's channel while preserving the other's current value --
 * es8388_set_dac_volume() always writes both at once, and this endpoint is
 * only ever asked to change one. */
static esp_err_t speaker_volume_apply(int line, uint8_t vol)
{
    uint8_t l = (line == 0) ? vol : es8388_shadow(ES8388_REG_LOUT1VOL);
    uint8_t r = (line == 1) ? vol : es8388_shadow(ES8388_REG_ROUT1VOL);
    return es8388_set_dac_volume(l, r);
}

/* POST /speaker_volume_live -- body line=N&vol=N. Applies immediately,
 * does NOT persist -- the slider calls this on every drag tick (see the
 * settings page's inline script) so listening while adjusting doesn't spam
 * NVS writes. Plain-text response (not a redirect): called via fetch(),
 * never a real form submit. */
static esp_err_t speaker_volume_live_handler(httpd_req_t *req)
{
    char body[64], line_s[8] = {0}, vol_s[8] = {0};
    if (!read_form_body(req, body, sizeof(body))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body"); return ESP_OK;
    }
    form_field(body, "line", line_s, sizeof(line_s));
    form_field(body, "vol", vol_s, sizeof(vol_s));
    int line = atoi(line_s);
    int vol = atoi(vol_s);
    if (line < 0 || line > 1 || vol < 0 || vol > 63) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "line 0-1, vol 0-63"); return ESP_OK;
    }
    speaker_volume_apply(line, (uint8_t)vol);
    httpd_resp_sendstr(req, "ok\n");
    return ESP_OK;
}

/* POST /speaker_volume -- body line=N&vol=N. Applies AND persists -- the
 * "Save" button next to the slider, a real form submit (unlike the live
 * endpoint above), so it redirects back to the settings page like every
 * other settings form in this file. */
static esp_err_t speaker_volume_handler(httpd_req_t *req)
{
    char body[64], line_s[8] = {0}, vol_s[8] = {0};
    if (!read_form_body(req, body, sizeof(body))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body"); return ESP_OK;
    }
    form_field(body, "line", line_s, sizeof(line_s));
    form_field(body, "vol", vol_s, sizeof(vol_s));
    int line = atoi(line_s);
    int vol = atoi(vol_s);
    if (line < 0 || line > 1 || vol < 0 || vol > 63) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "line 0-1, vol 0-63"); return ESP_OK;
    }
    speaker_volume_apply(line, (uint8_t)vol);
    board_speaker_volume_set(line, (uint8_t)vol);

    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/settings?tab=diag");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}
#endif /* CONFIG_BOARD_TYPE_SMD2 */

/* ---- AEC calibration wizard ----
 * Webui counterpart to the bring-up `echoprobe` console command, for a
 * public build where users won't have serial access. Not board-type gated
 * -- audio_echo_probe() and the AEC delay it feeds are generic across
 * codecs (see docs/signal-levels.md Sec.1C for why this, not a live
 * call, is what actually sizes a canceller, and why the per-board delay
 * can't be a shared firmware constant). */

#define AEC_CAL_REPEATS      3   /* same reasoning as echoprobe's repeat mode:
                                   * a single run is vulnerable to ambient
                                   * noise inflating the noise floor and
                                   * hiding a real echo */
#define AEC_CAL_SETTLE_MS  300

/* POST /aec_measure -- body line=N. Runs a few probes back to back (see
 * AEC_CAL_REPEATS) and returns JSON with the averaged result. Read-only --
 * doesn't touch the persisted or live delay, so it's safe to call via
 * fetch() from the settings page without a page reload, unlike the actual
 * apply step below. */
static esp_err_t aec_measure_handler(httpd_req_t *req)
{
    char body[64], line_s[8] = {0};
    if (!read_form_body(req, body, sizeof(body))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body"); return ESP_OK;
    }
    form_field(body, "line", line_s, sizeof(line_s));
    int line = atoi(line_s);
    if (line < 0 || line > 1) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "line 0-1"); return ESP_OK;
    }

    int hits = 0;
    double delay_sum = 0, tail_sum = 0;
    for (int i = 0; i < AEC_CAL_REPEATS; i++) {
        audio_echo_probe_t res;
        if (audio_echo_probe(line, 1000, 10, 400, NULL, 0, &res) && res.leak_detected) {
            hits++;
            delay_sum += res.delay_ms;
            tail_sum += res.tail_ms;
        }
        if (i + 1 < AEC_CAL_REPEATS) vTaskDelay(pdMS_TO_TICKS(AEC_CAL_SETTLE_MS));
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "attempts", AEC_CAL_REPEATS);
    cJSON_AddNumberToObject(root, "hits", hits);
    if (hits > 0) {
        double delay_ms = delay_sum / hits, tail_ms = tail_sum / hits;
        double span_ms = delay_ms + tail_ms;
        cJSON_AddBoolToObject(root, "ok", true);
        cJSON_AddNumberToObject(root, "delay_ms", delay_ms);
        cJSON_AddNumberToObject(root, "tail_ms", tail_ms);
        cJSON_AddNumberToObject(root, "span_ms", span_ms);
        cJSON_AddNumberToObject(root, "taps", span_ms * 8.0);
    } else {
        cJSON_AddBoolToObject(root, "ok", false);
    }
    char *text = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, text ? text : "{}");
    cJSON_Delete(root);
    free(text);
    return ESP_OK;
}

/* POST /aec_delay_apply -- body line=N&ms=N. The wizard's "Apply this
 * value" step: persists (survives reboot) and applies live (aec_set_delay_ms
 * resets the adaptive filter, same as the console `aec delay` command) so
 * it takes effect on the next call without one. A real form submit, so it
 * redirects like every other settings form here. */
static esp_err_t aec_delay_apply_handler(httpd_req_t *req)
{
    char body[64], line_s[8] = {0}, ms_s[8] = {0};
    if (!read_form_body(req, body, sizeof(body))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body"); return ESP_OK;
    }
    form_field(body, "line", line_s, sizeof(line_s));
    form_field(body, "ms", ms_s, sizeof(ms_s));
    int line = atoi(line_s);
    long ms = strtol(ms_s, NULL, 10);
    if (line < 0 || line > 1 || ms < 0 || ms > AEC_MAX_DELAY_MS) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad line/ms"); return ESP_OK;
    }
    if (aec_set_delay_ms(line, (uint32_t)ms) == ESP_OK) {
        board_aec_delay_set(line, (uint32_t)ms);
    }

    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/settings?tab=diag");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* POST /aec_enable -- body line=N&on=0|1. Testing toggle -- see the
 * calibration card's hint text for why this isn't meant to be left on
 * without also calibrating the delay below it. */
static esp_err_t aec_enable_handler(httpd_req_t *req)
{
    char body[64], line_s[8] = {0}, on_s[8] = {0};
    if (!read_form_body(req, body, sizeof(body))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body"); return ESP_OK;
    }
    form_field(body, "line", line_s, sizeof(line_s));
    form_field(body, "on", on_s, sizeof(on_s));
    int line = atoi(line_s);
    if (line < 0 || line > 1) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad line"); return ESP_OK;
    }
    aec_set_enabled(line, on_s[0] == '1');

    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/settings?tab=diag");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* GET /voip_server_script -- downloadable bash script that provisions this
 * phone's line(s) as SIP peers on an Asterisk/PBX box, generated from the
 * currently-saved config (Settings -> VOIP). Marker-tagged blocks (named
 * after the board) so re-running it (config changed, or alongside other
 * phones' scripts on the same server) replaces just this phone's own
 * blocks -- sed-deletes the old marked block before appending the new one,
 * rather than overwriting the whole file the way a from-scratch installer
 * would. */
static esp_err_t voip_server_script_handler(httpd_req_t *req)
{
    char name[32];
    board_name_get(name, sizeof(name));

    char *buf = malloc(4096);
    if (!buf) { httpd_resp_send_500(req); return ESP_FAIL; }
    int off = 0;
    off += snprintf(buf + off, 4096 - off,
        "#!/bin/bash\n"
        "# Asterisk setup for rotary phone gateway '%s' -- generated from its\n"
        "# Settings -> VOIP page. Run on the Asterisk/PBX box (sudo).\n"
        "set -e\n"
        "which asterisk >/dev/null || { echo \"installing asterisk...\"; apt-get update && apt-get install -y asterisk; }\n"
        "\n"
        "PJSIP=/etc/asterisk/pjsip.conf\n"
        "EXT=/etc/asterisk/extensions.conf\n"
        "touch \"$PJSIP\" \"$EXT\"\n"
        "\n"
        "grep -q '^\\[transport-udp\\]' \"$PJSIP\" || cat >> \"$PJSIP\" <<'TRANSPORT'\n"
        "[transport-udp]\n"
        "type=transport\n"
        "protocol=udp\n"
        "bind=0.0.0.0:5060\n"
        "TRANSPORT\n"
        "\n"
        "grep -q '^\\[rotary\\]' \"$EXT\" || cat >> \"$EXT\" <<'CONTEXT'\n"
        "[rotary]\n"
        "CONTEXT\n",
        name);

    int num_lines = board_hw_config_get().num_lines;
    for (int line = 0; line < num_lines; line++) {
        const char *user = voip_user(line);
        const char *pass = voip_pass(line);
        if (!user[0]) continue; /* line not configured yet -- nothing to provision */
        off += snprintf(buf + off, (off < 4096) ? (size_t)(4096 - off) : 0,
            "\n"
            "# ---- %s line %d (%s) ----\n"
            "sed -i \"/^; BEGIN %s-line%d/,/^; END %s-line%d/d\" \"$PJSIP\"\n"
            "cat >> \"$PJSIP\" <<PEER\n"
            "; BEGIN %s-line%d\n"
            "[%s]\n"
            "type=aor\n"
            "max_contacts=1\n"
            "remove_existing=yes\n"
            "\n"
            "[%s]\n"
            "type=auth\n"
            "auth_type=userpass\n"
            "username=%s\n"
            "password=%s\n"
            "\n"
            "[%s]\n"
            "type=endpoint\n"
            "context=rotary\n"
            "disallow=all\n"
            "allow=ulaw,alaw,opus\n"
            "auth=%s\n"
            "aors=%s\n"
            "transport=transport-udp\n"
            "identify_by=username\n"
            "; END %s-line%d\n"
            "PEER\n"
            /* Same marker-delete-then-reinsert approach as the pjsip.conf
             * block above, not a bare "grep -v the exten line" -- this
             * dialplan entry is 2 lines (exten => + a continuation same =>),
             * and grep -v only ever dropped the first, leaving an orphaned
             * " same => n,Hangup()" behind on every re-run that silently
             * glued itself onto whatever extension happened to follow it in
             * the file. Wrapping the whole block in its own BEGIN/END
             * markers makes the delete exact regardless of line count. */
            "sed -i \"/^; BEGIN %s-line%d-ext/,/^; END %s-line%d-ext/d\" \"$EXT\"\n"
            "awk -v u=\"%s\" '{print} /^\\[rotary\\]$/{print \"; BEGIN %s-line%d-ext\"; "
            "print \"exten => \" u \",1,Dial(PJSIP/\" u \",30)\"; print \" same => n,Hangup()\"; "
            "print \"; END %s-line%d-ext\"}' \"$EXT\" > \"$EXT.tmp\" && mv \"$EXT.tmp\" \"$EXT\"\n",
            name, line, user,           /* comment */
            name, line, name, line,     /* sed delete marker range */
            name, line,                 /* BEGIN marker */
            user,                       /* [user] aor */
            user,                       /* [user] auth */
            user, pass,                 /* username=/password= */
            user,                       /* [user] endpoint */
            user, user,                 /* auth=/aors= */
            name, line,                 /* END marker */
            name, line, name, line,     /* extensions.conf sed delete marker range */
            user,                       /* awk -v u= */
            name, line,                 /* extensions.conf BEGIN marker */
            name, line);                /* extensions.conf END marker */
    }

    off += snprintf(buf + off, (off < 4096) ? 4096 - off : 0,
        "\n"
        "asterisk -rx \"pjsip reload\" 2>/dev/null || true\n"
        "asterisk -rx \"dialplan reload\" 2>/dev/null || true\n"
        "echo \"Done -- check with: asterisk -rx 'pjsip show contacts'\"\n");

    httpd_resp_set_hdr(req, "Content-Disposition",
        "attachment; filename=\"rotary-voip-server-setup.sh\"");
    httpd_resp_set_type(req, "text/x-shellscript");
    httpd_resp_send(req, buf, off);
    free(buf);
    return ESP_OK;
}

/* Builds the numbered VOIP setup steps, filled in with THIS board's current
 * config (its own address, and the currently-saved SIP server) rather than
 * generic boilerplate -- one shared builder used both by the on-page steps
 * in append_voip_tab() and the downloadable GET /voip_instructions.txt, so
 * the two can never drift out of sync with each other. */
static void build_voip_instructions(char steps[VOIP_INSTR_STEPS][VOIP_INSTR_STEP_LEN])
{
    char ip[16];
    wifi_get_ip_str(ip, sizeof(ip));
    const char *addr = wifi_is_connected() ? ip : "<this phone's IP>";
    const char *server = voip_server()[0] ? voip_server() : "your-asterisk-server";

    snprintf(steps[0], VOIP_INSTR_STEP_LEN,
        "Download the setup script from http://%s/voip_server_script", addr);
    snprintf(steps[1], VOIP_INSTR_STEP_LEN,
        "Copy it to your Asterisk server: scp voip_setup.sh user@%s:", server);
    snprintf(steps[2], VOIP_INSTR_STEP_LEN,
        "Run it there: ssh user@%s bash voip_setup.sh", server);
    if (voip_user(0)[0]) {
        snprintf(steps[3], VOIP_INSTR_STEP_LEN,
            "Come back to this page and check that line 1 (extension %s) shows "
            "\"registered\".", voip_user(0));
    } else {
        snprintf(steps[3], VOIP_INSTR_STEP_LEN,
            "Come back to this page and check the registration status below.");
    }
}

/* GET /voip_instructions.txt -- plain-text numbered setup steps, for anyone
 * who'd rather read/save/print them than follow the on-page copy in
 * append_voip_tab() -- same wording either way, see build_voip_instructions(). */
static esp_err_t voip_instructions_txt_handler(httpd_req_t *req)
{
    char steps[VOIP_INSTR_STEPS][VOIP_INSTR_STEP_LEN];
    build_voip_instructions(steps);

    char body[VOIP_INSTR_STEPS * VOIP_INSTR_STEP_LEN + 64];
    int off = 0;
    for (int i = 0; i < VOIP_INSTR_STEPS; i++) {
        off += snprintf(body + off, (off < (int)sizeof(body)) ? sizeof(body) - off : 0,
                         "%d. %s\n", i + 1, steps[i]);
    }
    httpd_resp_set_hdr(req, "Content-Disposition",
        "attachment; filename=\"voip_setup_instructions.txt\"");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, body, off);
    return ESP_OK;
}

/* POST /dialcode_add -- form body code=..&filename=..&desc=.. (see
 * dialplan.h's dialplan_clip_add() for the validation/conflict rules). */
static esp_err_t dialcode_add_handler(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len >= WIFI_FORM_MAX_BODY) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad form body");
        return ESP_OK;
    }
    char body[WIFI_FORM_MAX_BODY];
    int r = httpd_req_recv(req, body, req->content_len);
    if (r <= 0) { httpd_resp_send_500(req); return ESP_FAIL; }
    body[r] = '\0';

    char code[8] = {0}, filename[32] = {0}, desc[64] = {0};
    form_field(body, "code", code, sizeof(code));
    form_field(body, "filename", filename, sizeof(filename));
    form_field(body, "desc", desc, sizeof(desc));

    esp_err_t err = dialplan_clip_add(code, filename, desc[0] ? desc : filename);
    if (err != ESP_OK) {
        const char *why =
            (err == ESP_ERR_INVALID_ARG)   ? "code must be 2-6 digits and a file must be picked" :
            (err == ESP_ERR_INVALID_STATE) ? "that code conflicts with an existing one" :
            (err == ESP_ERR_NO_MEM)        ? "dial code limit reached" : esp_err_to_name(err);
        ESP_LOGW(TAG, "dialcode_add '%s' -> '%s' failed: %s", code, filename, why);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, why);
        return ESP_OK;
    }

    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/settings?tab=dialcodes");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* POST /dialcode_set_code -- form body old_code=..&new_code=.. */
static esp_err_t dialcode_set_code_handler(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len >= WIFI_FORM_MAX_BODY) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad form body");
        return ESP_OK;
    }
    char body[WIFI_FORM_MAX_BODY];
    int r = httpd_req_recv(req, body, req->content_len);
    if (r <= 0) { httpd_resp_send_500(req); return ESP_FAIL; }
    body[r] = '\0';

    char old_code[8] = {0}, new_code[8] = {0};
    form_field(body, "old_code", old_code, sizeof(old_code));
    form_field(body, "new_code", new_code, sizeof(new_code));

    esp_err_t err = dialplan_clip_set_code(old_code, new_code);
    if (err != ESP_OK) {
        const char *why =
            (err == ESP_ERR_INVALID_ARG)   ? "code must be 2-6 digits" :
            (err == ESP_ERR_INVALID_STATE) ? "that code conflicts with an existing one" :
            (err == ESP_ERR_NOT_FOUND)     ? "code not found" : esp_err_to_name(err);
        ESP_LOGW(TAG, "dialcode_set_code '%s' -> '%s' failed: %s", old_code, new_code, why);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, why);
        return ESP_OK;
    }

    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/settings?tab=dialcodes");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* POST /dialcode_set_filename -- form body code=..&filename=.. (points an
 * existing code at a different already-stored file). */
static esp_err_t dialcode_set_filename_handler(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len >= WIFI_FORM_MAX_BODY) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad form body");
        return ESP_OK;
    }
    char body[WIFI_FORM_MAX_BODY];
    int r = httpd_req_recv(req, body, req->content_len);
    if (r <= 0) { httpd_resp_send_500(req); return ESP_FAIL; }
    body[r] = '\0';

    char code[8] = {0}, filename[32] = {0};
    form_field(body, "code", code, sizeof(code));
    form_field(body, "filename", filename, sizeof(filename));

    esp_err_t err = dialplan_clip_set_filename(code, filename);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "dialcode_set_filename '%s' -> '%s' failed: %s",
                 code, filename, esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, esp_err_to_name(err));
        return ESP_OK;
    }

    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/settings?tab=dialcodes");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* POST /dialcode_set_desc -- form body code=..&desc=.. (relabels an
 * existing code, builtin or not -- see dialplan_clip_set_desc()). */
static esp_err_t dialcode_set_desc_handler(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len >= WIFI_FORM_MAX_BODY) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad form body");
        return ESP_OK;
    }
    char body[WIFI_FORM_MAX_BODY];
    int r = httpd_req_recv(req, body, req->content_len);
    if (r <= 0) { httpd_resp_send_500(req); return ESP_FAIL; }
    body[r] = '\0';

    char code[8] = {0}, desc[64] = {0};
    form_field(body, "code", code, sizeof(code));
    form_field(body, "desc", desc, sizeof(desc));

    esp_err_t err = dialplan_clip_set_desc(code, desc);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "dialcode_set_desc '%s' -> '%s' failed: %s", code, desc, esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, esp_err_to_name(err));
        return ESP_OK;
    }

    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/settings?tab=dialcodes");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* POST /dialcode_delete/<code> */
static esp_err_t dialcode_delete_handler(httpd_req_t *req)
{
    const char *prefix = "/dialcode_delete/";
    if (strncmp(req->uri, prefix, strlen(prefix)) != 0) {
        httpd_resp_send_404(req);
        return ESP_OK;
    }
    const char *code = req->uri + strlen(prefix);
    esp_err_t err = dialplan_clip_remove(code);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "dialcode_delete '%s' failed: %s", code, esp_err_to_name(err));
    }

    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/settings?tab=dialcodes");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* POST /media_delete/<name> -- plain HTML-form-friendly delete (browsers
 * can't send DELETE from a <form>), redirects back to whichever page the
 * delete was clicked from so it just refreshes with the file gone -- this
 * form is now shared by both the Recordings page and Settings -> Storage
 * (see append_storage_table()), so it can no longer just always assume
 * Settings the way it did when Storage was the form's only caller. */
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

    /* return_to is attacker-influenceable POST body content (anyone on the
     * LAN could POST here directly with any value), so it's allowlisted
     * against known exact values rather than dropped straight into the
     * Location header -- an unvalidated redirect target would be an open
     * redirect. Falls back to Settings -> Storage if missing/unrecognized
     * (e.g. a request with no body at all). */
    char body[64] = {0};
    if (req->content_len > 0 && req->content_len < sizeof(body)) {
        int r = httpd_req_recv(req, body, req->content_len);
        if (r > 0) body[r] = '\0';
    }
    char return_to[32] = "";
    form_field(body, "return_to", return_to, sizeof(return_to));

    const char *loc = "/settings?tab=storage";
    if (strcmp(return_to, "/recordings") == 0) loc = "/recordings";

    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", loc);
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

/* Card-rendering for the dial-code guide -- formerly its own /guide page
 * (guide_handler), now rendered inline on the Status page (index_handler)
 * below the status/line cards and the activity panel; /guide itself just
 * redirects to / for old bookmarks (see redirect_to_index_handler()).
 * Caller opens the "<div class=\"cards\">" wrapper (shared with the
 * status/line cards above it on that page) and appends the closing
 * "</main></body></html>" after this returns -- this only fills in the
 * cards themselves plus the closing hint paragraph, same content
 * guide_handler used to generate from this same live table
 * (dialplan_list()/dialplan_list_clip_entries()) -- that invariant (the
 * guide can't drift out of sync with what dialplan_feed() actually
 * matches) is unchanged, only WHERE it's rendered moved. */
static int append_dialguide_cards(char *buf, size_t cap, int off,
                                   dialplan_entry_info_t *entries, size_t n)
{
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

    /* Clip codes (Settings -> Dial Codes) -- builtin "silly audio" clips
     * plus anything a user's added, same live-table guarantee as everything
     * else here. No fixed prefix card for these any more since codes are
     * freely reassignable (see dialplan_clip_entry_t). */
    dialplan_clip_entry_t clips[DIALPLAN_MAX_CLIP_ENTRIES];
    size_t nclips = dialplan_list_clip_entries(clips, DIALPLAN_MAX_CLIP_ENTRIES);
    if (nclips > 0 && off > 0 && (size_t)off < cap) {
        off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
            "<div class=\"card\"><h2>Clips</h2>");
        for (size_t i = 0; i < nclips && (size_t)off < cap; i++) {
            off = append_guide_row(buf, cap, off, clips[i].code, clips[i].desc, true);
        }
        off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0, "</div>");
    }

    if (off > 0 && (size_t)off < cap) {
        /* Synthetic row: 555xxxx is matched directly in dialplan_feed(),
         * not a TABLE entry, so it isn't in dialplan_list()'s output. */
        off += snprintf(buf + off, cap - off,
            "<div class=\"card\"><h2>VoIP</h2>"
            "<div class=\"code-row\"><span class=\"code\">5-5-5-x-x-x-x</span>"
            "<span class=\"desc\">7-digit extension</span>"
            "<span class=\"pill on\">working</span></div></div>"
            "</div>" /* .cards, opened by the caller (index_handler) */
            "<p class=\"hint\">Source of truth: <code>components/dialplan/dialplan.c</code> -- "
            "this list is generated from the same table the phone actually matches against, "
            "so it can't drift out of sync with real behavior.</p>");
    }
    return off;
}

/* /guide and the old /dialplan route name both redirect here now that the
 * dial-code guide is merged into the Status page (append_dialguide_cards())
 * instead of living on its own page -- keeps old bookmarks/links working. */
static esp_err_t redirect_to_index_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
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
    /* req->uri includes any query string (e.g. "foo.wav?inline=1") -- strip
     * it before using this as a filename, both for lookups below and so the
     * query-string handling further down doesn't see it munged in. */
    char name[STORAGE_MAX_NAME];
    strncpy(name, req->uri + strlen(prefix), sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';
    char *qmark = strchr(name, '?');
    if (qmark) *qmark = '\0';

    storage_entry_t entries[32];
    size_t n_entries = storage_list(entries, 32);
    bool found = false;
    size_t total = 0;
    for (size_t i = 0; i < n_entries; i++) {
        if (strcmp(entries[i].name, name) == 0) { found = true; total = entries[i].size; break; }
    }
    if (!found) {
        /* Not an upload/recording -- check the bundled (build-time) assets
         * too, same fallback order as storage_read_open()/player_play(). */
        storage_entry_t bundled[16];
        size_t nb = storage_list_bundled(bundled, 16);
        for (size_t i = 0; i < nb; i++) {
            if (strcmp(bundled[i].name, name) == 0) { found = true; total = bundled[i].size; break; }
        }
    }
    if (!found) {
        httpd_resp_send_404(req);
        return ESP_OK;
    }

    /* Stream in small chunks rather than malloc(size) + one-shot send --
     * clips can be a few hundred KB and the C6's heap doesn't reliably have
     * that much contiguous free RAM alongside WiFi/audio buffers (a single
     * malloc(338748) failed outright in testing). */
    storage_reader_t r;
    if (storage_read_open(name, &r) != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    /* HTTP Range support: browsers' <audio> elements routinely probe with
     * "Range: bytes=0-" (or similar) before they'll even attempt playback --
     * without a proper 206 response, several browsers (Safari especially)
     * refuse to play at all and surface a bare "error", even though a plain
     * GET of the same URL returns a perfectly valid file (confirmed via
     * curl). start/end are an inclusive byte range, end defaulting to EOF
     * when the client leaves it open ("bytes=1234-"). */
    size_t start = 0, end = total ? total - 1 : 0;
    bool has_range = false;
    char rng[64];
    if (total > 0 && httpd_req_get_hdr_value_str(req, "Range", rng, sizeof(rng)) == ESP_OK) {
        size_t rs, re;
        if (sscanf(rng, "bytes=%zu-%zu", &rs, &re) == 2) { start = rs; end = re; has_range = true; }
        else if (sscanf(rng, "bytes=%zu-", &rs) == 1)    { start = rs; has_range = true; }
        if (has_range && (start >= total || start > end)) {
            char cr[48];
            snprintf(cr, sizeof(cr), "bytes */%zu", total);
            httpd_resp_set_hdr(req, "Content-Range", cr);
            httpd_resp_set_status(req, "416 Range Not Satisfiable");
            httpd_resp_send(req, NULL, 0);
            storage_read_close(&r);
            return ESP_OK;
        }
        if (end > total - 1) end = total - 1;
    }

    /* Same URL serves both the "download" links and the <audio> players --
     * an inline request (from the player, e.g. /media/foo.wav?inline=1)
     * skips Content-Disposition: attachment so browsers play it in place
     * instead of offering a save dialog; a bare /media/foo.wav still forces
     * a download, matching every existing link to this route. */
    char query[32];
    bool inline_req = httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
                       strstr(query, "inline=1") != NULL;
    if (!inline_req) {
        char disp[64];
        snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", name);
        httpd_resp_set_hdr(req, "Content-Disposition", disp);
    }
    httpd_resp_set_type(req, is_wav_name(name) ? "audio/wav" :
                              is_mp3_name(name) ? "audio/mpeg" : "application/octet-stream");
    httpd_resp_set_hdr(req, "Accept-Ranges", "bytes");
    if (has_range) {
        char cr[64];
        snprintf(cr, sizeof(cr), "bytes %zu-%zu/%zu", start, end, total);
        httpd_resp_set_hdr(req, "Content-Range", cr);
        httpd_resp_set_status(req, "206 Partial Content");
        storage_read_seek(&r, start);
    }

    char chunk[2048];
    size_t remaining = has_range ? (end - start + 1) : SIZE_MAX;
    size_t got;
    esp_err_t send_err = ESP_OK;
    while (remaining > 0 &&
           (got = storage_read_chunk(&r, chunk, (remaining < sizeof(chunk)) ? remaining : sizeof(chunk))) > 0) {
        send_err = httpd_resp_send_chunk(req, chunk, got);
        if (send_err != ESP_OK) break;
        remaining -= got;
    }
    storage_read_close(&r);
    if (send_err != ESP_OK) return ESP_FAIL;
    httpd_resp_send_chunk(req, NULL, 0); /* terminate the chunked response */
    return ESP_OK;
}

/* Feeds one input blob through the conversion pipeline in small steps --
 * caps how much a single audio_convert_push() call could ever need to write
 * (see the 4000 Hz floor enforced by the caller: worst case is a 2x
 * upsample, so 128 input bytes at 1 byte/frame (8-bit mono, the smallest
 * possible frame) -> at most 256 output samples, matching pcm_out's size
 * exactly). Small and fixed regardless of caller context so nothing here
 * ever needs a stack buffer sized off the upload itself. */
static size_t convert_and_write(audio_convert_ctx_t *cvt, storage_writer_t *w,
                                  const uint8_t *data, size_t len)
{
    int16_t pcm_out[256];
    const size_t IN_STEP = 128;
    size_t off = 0, data_bytes = 0;
    while (off < len) {
        size_t take = (len - off < IN_STEP) ? len - off : IN_STEP;
        size_t n = audio_convert_push(cvt, data + off, take, pcm_out, 256);
        if (n > 0) {
            storage_write_chunk(w, pcm_out, n * sizeof(int16_t));
            data_bytes += n * sizeof(int16_t);
        }
        off += take;
    }
    return data_bytes;
}

static void wr_u16le(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr_u32le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* Mono 16-bit PCM WAV header, matching recorder.c's build_wav_header (same
 * format, independent copy -- small enough that sharing it isn't worth a
 * cross-component dependency). */
static void build_wav_header(uint8_t *h, uint32_t rate, uint32_t data_bytes)
{
    memcpy(h, "RIFF", 4);      wr_u32le(h + 4, 36 + data_bytes); memcpy(h + 8, "WAVE", 4);
    memcpy(h + 12, "fmt ", 4); wr_u32le(h + 16, 16);
    wr_u16le(h + 20, 1);         /* PCM */
    wr_u16le(h + 22, 1);         /* mono */
    wr_u32le(h + 24, rate);
    wr_u32le(h + 28, rate * 2);  /* byte rate = rate * channels * bytes/sample */
    wr_u16le(h + 32, 2);         /* block align */
    wr_u16le(h + 34, 16);        /* bits/sample */
    memcpy(h + 36, "data", 4); wr_u32le(h + 40, data_bytes);
}

/* Reads the CURRENT part's whole body as text into `out` (NUL-terminated,
 * truncated to fit `cap`) -- for small non-file form fields (code, desc,
 * ...) in a multipart form. Drains the full part even if it's longer than
 * `cap`, so the reader stays correctly positioned for the next part. */
static void read_field_body(multipart_reader_t *mp, char *out, size_t cap)
{
    size_t got = 0;
    uint8_t buf[64];
    size_t n;
    while ((n = multipart_read_body(mp, buf, sizeof(buf))) > 0) {
        size_t space = (cap > got + 1) ? cap - 1 - got : 0;
        size_t take = (n < space) ? n : space;
        if (take > 0) { memcpy(out + got, buf, take); got += take; }
    }
    out[got] = '\0';
}

/* Keeps only [A-Za-z0-9_.-] from `in` (browsers may send a full path in
 * `filename`, and arbitrary punctuation isn't a safe bare storage name),
 * forcing a .wav extension if the result doesn't already have one. Falls
 * back to `fallback` if nothing usable is left. */
static void sanitize_upload_name(const char *in, const char *fallback, char *out, size_t cap)
{
    const char *base = strrchr(in, '/');
    base = base ? base + 1 : in;
    const char *base2 = strrchr(base, '\\');
    if (base2) base = base2 + 1;

    size_t o = 0;
    for (; *base && o + 1 < cap; base++) {
        char c = *base;
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
        out[o++] = ok ? c : '_';
    }
    out[o] = '\0';
    if (!out[0]) { strncpy(out, fallback, cap - 1); out[cap - 1] = '\0'; return; }
    if (!is_wav_name(out)) {
        size_t len = strlen(out);
        if (len + 4 >= cap) len = (cap > 4) ? cap - 5 : 0;
        memcpy(out + len, ".wav", 5);
    }
}

/* Converts the CURRENT part's body (a WAV file, common sample rates,
 * 8/16/24/32-bit, mono or multi-channel) to 8kHz mono 16-bit PCM via
 * `audio_convert` and streams the result straight into storage under
 * `dest_name` -- never buffers the whole file (this hardware has hard-
 * failed a single ~330KB malloc before). Shared by /operator_upload and
 * /dialcode_upload. Returns NULL on success, else a static error string. */
#ifdef CONFIG_AUDIO_CONVERT_MP3
/* MP3 trial (see audio_convert's Kconfig) -- decodes frame-by-frame from a
 * rolling compressed-bytes buffer (minimp3 needs a few KB of contiguous
 * input per frame, not the whole file) into the same normalize/downmix/
 * LPF/resample pipeline WAV uploads use. `prefetched` is the header-sniff
 * buffer convert_upload_part() already read (up to UPLOAD_HEADER_SCAN_BYTES,
 * <= this function's rolling buffer, so nothing from it is ever dropped). */
#define MP3_ROLLING_BUF_SIZE UPLOAD_HEADER_SCAN_BYTES

static const char *convert_mp3_upload(multipart_reader_t *mp, const char *dest_name,
                                        const uint8_t *prefetched, size_t prefetched_len)
{
    audio_mp3_decoder_t *dec = audio_mp3_decoder_create();
    if (!dec) return "out of memory";

    uint8_t *rbuf = malloc(MP3_ROLLING_BUF_SIZE);
    int16_t *pcm = malloc(AUDIO_CONVERT_MP3_MAX_SAMPLES * sizeof(int16_t));
    if (!rbuf || !pcm) {
        free(rbuf); free(pcm); audio_mp3_decoder_destroy(dec);
        return "out of memory";
    }
    size_t rlen = prefetched_len; /* <= UPLOAD_HEADER_SCAN_BYTES == MP3_ROLLING_BUF_SIZE */
    memcpy(rbuf, prefetched, rlen);
    size_t rpos = 0;
    bool more_input = true;

    bool have_fmt = false;
    audio_convert_ctx_t cvt;
    storage_writer_t w;
    size_t data_bytes = 0;
    const char *err = NULL;

    for (;;) {
        if (rlen - rpos < MP3_ROLLING_BUF_SIZE / 2 && more_input) {
            if (rpos > 0) { memmove(rbuf, rbuf + rpos, rlen - rpos); rlen -= rpos; rpos = 0; }
            size_t n = multipart_read_body(mp, rbuf + rlen, MP3_ROLLING_BUF_SIZE - rlen);
            if (n == 0) more_input = false;
            else rlen += n;
        }
        if (rlen - rpos == 0) break;

        int channels = 0;
        uint32_t rate = 0;
        size_t consumed = 0;
        int samples = audio_mp3_decode_frame(dec, rbuf + rpos, rlen - rpos, pcm,
                                               &channels, &rate, &consumed);
        if (consumed == 0) {
            if (!more_input) break; /* trailing bytes too short for a frame -- done */
            continue; /* top up the buffer next iteration and retry the same bytes */
        }
        rpos += consumed;
        if (samples <= 0) continue; /* skipped an ID3/garbage region */

        if (!have_fmt) {
            audio_convert_fmt_t fmt = {
                .in_rate = rate, .in_channels = (uint16_t)channels, .in_bits = 16, .in_float = false,
            };
            audio_convert_init(&cvt, &fmt);
            if (storage_write_begin_local(dest_name, 0, &w) != ESP_OK) {
                err = "couldn't open storage for writing";
                break;
            }
            uint8_t wav_hdr[44];
            build_wav_header(wav_hdr, AUDIO_CONVERT_OUT_RATE, 0);
            storage_write_chunk(&w, wav_hdr, sizeof(wav_hdr));
            have_fmt = true;
        }
        data_bytes += convert_and_write(&cvt, &w, (const uint8_t *)pcm,
                                          (size_t)samples * channels * sizeof(int16_t));
    }

    free(rbuf);
    free(pcm);
    audio_mp3_decoder_destroy(dec);
    if (err) return err;
    if (!have_fmt) return "not a valid MP3 (no frames decoded)";

    storage_write_end(&w);
    uint8_t sz[4];
    wr_u32le(sz, 36 + (uint32_t)data_bytes); storage_patch(dest_name, 4, sz, 4);
    wr_u32le(sz, (uint32_t)data_bytes);      storage_patch(dest_name, 40, sz, 4);
    return NULL;
}
#endif /* CONFIG_AUDIO_CONVERT_MP3 */

static const char *convert_upload_part(multipart_reader_t *mp, const char *dest_name)
{
    uint8_t *header = malloc(UPLOAD_HEADER_SCAN_BYTES);
    if (!header) return "out of memory";
    size_t got = 0;
    while (got < UPLOAD_HEADER_SCAN_BYTES) {
        size_t n = multipart_read_body(mp, header + got, UPLOAD_HEADER_SCAN_BYTES - got);
        if (n == 0) break;
        got += n;
    }

#ifdef CONFIG_AUDIO_CONVERT_MP3
    if (audio_convert_mp3_magic(header, got)) {
        const char *err = convert_mp3_upload(mp, dest_name, header, got);
        free(header);
        return err;
    }
#endif

    audio_wav_info_t wav;
    if (!audio_wav_parse(header, got, &wav) ||
        (wav.audio_format != 1 && wav.audio_format != 3) ||
        wav.channels == 0 || wav.channels > 8 ||
        (wav.bits_per_sample != 8 && wav.bits_per_sample != 16 &&
         wav.bits_per_sample != 24 && wav.bits_per_sample != 32) ||
        wav.sample_rate < 4000 || wav.sample_rate > 192000 ||
        wav.data_offset >= got) {
        free(header);
        return "not a supported WAV (PCM/float, 8/16/24/32-bit, 4kHz-192kHz)";
    }

    /* Estimate the converted (8kHz mono 16-bit) output size so the eviction
     * cap has something meaningful to work with -- passing 0 here would
     * skip proactively freeing space for a long upload. */
    size_t in_bytes_per_frame = (size_t)wav.channels * (wav.bits_per_sample / 8);
    uint64_t est_out_bytes = in_bytes_per_frame
        ? (uint64_t)wav.data_bytes * AUDIO_CONVERT_OUT_RATE * 2
              / ((uint64_t)in_bytes_per_frame * wav.sample_rate)
        : 0;

    storage_writer_t w;
    if (storage_write_begin_local(dest_name, 44 + (size_t)est_out_bytes, &w) != ESP_OK) {
        free(header);
        return "couldn't open storage for writing";
    }

    /* Placeholder header -- sizes get patched in below once the real
     * converted length is known (same pattern as recorder.c). */
    uint8_t wav_hdr[44];
    build_wav_header(wav_hdr, AUDIO_CONVERT_OUT_RATE, 0);
    storage_write_chunk(&w, wav_hdr, sizeof(wav_hdr));

    audio_convert_ctx_t cvt;
    audio_convert_fmt_t fmt = {
        .in_rate = wav.sample_rate,
        .in_channels = wav.channels,
        .in_bits = wav.bits_per_sample,
        .in_float = (wav.audio_format == 3),
    };
    audio_convert_init(&cvt, &fmt);

    /* Bytes after the WAV header in what we already scanned are the start
     * of the actual audio data -- convert those before pulling any more
     * from the multipart stream. */
    size_t data_bytes = convert_and_write(&cvt, &w, header + wav.data_offset, got - wav.data_offset);

    uint8_t chunk[2048];
    size_t r;
    while ((r = multipart_read_body(mp, chunk, sizeof(chunk))) > 0) {
        data_bytes += convert_and_write(&cvt, &w, chunk, r);
    }
    storage_write_end(&w);

    uint8_t sz[4];
    wr_u32le(sz, 36 + (uint32_t)data_bytes); storage_patch(dest_name, 4, sz, 4);
    wr_u32le(sz, (uint32_t)data_bytes);      storage_patch(dest_name, 40, sz, 4);
    free(header);
    return NULL;
}

/* POST /operator_upload -- multipart, field "file". Fixed target filename,
 * no code/conflict logic at all -- the simplest possible use of
 * convert_upload_part(). */
static esp_err_t operator_upload_handler(httpd_req_t *req)
{
    multipart_reader_t mp;
    if (multipart_reader_init(&mp, req) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "expected multipart/form-data");
        return ESP_FAIL;
    }

    char part_name[32], filename[64];
    bool wrote = false;
    const char *err_msg = "no file uploaded";

    while (multipart_next_part(&mp, part_name, sizeof(part_name), filename, sizeof(filename)) == ESP_OK) {
        if (strcmp(part_name, "file") == 0 && filename[0]) {
            const char *err = convert_upload_part(&mp, "operator.wav");
            if (err) { err_msg = err; break; }
            wrote = true;
        } else {
            uint8_t scratch[128];
            while (multipart_read_body(&mp, scratch, sizeof(scratch)) > 0) { } /* discard */
        }
    }
    multipart_reader_close(&mp);

    if (!wrote) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, err_msg);
        return ESP_FAIL;
    }
    prompts_mark_generated("operator.wav");
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/settings?tab=dialcodes");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* POST /dialcode_upload -- multipart, fields (in this order in the HTML
 * forms, so they're parsed before the "file" part streams): "code" (an
 * EXISTING code -- replaces its audio in place, same filename) OR
 * "new_code" + optional "desc" (adds a brand new code, filename derived
 * from the uploaded file's own name). Exactly one of code/new_code should
 * be present -- the Dial Codes tab's two forms each send only one. */
static esp_err_t dialcode_upload_handler(httpd_req_t *req)
{
    multipart_reader_t mp;
    if (multipart_reader_init(&mp, req) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "expected multipart/form-data");
        return ESP_FAIL;
    }

    char code[8] = {0}, new_code[8] = {0}, desc[64] = {0};
    char part_name[32], filename[64];
    char dest[STORAGE_MAX_NAME] = {0};
    bool wrote = false;
    const char *err_msg = "no file uploaded";

    while (multipart_next_part(&mp, part_name, sizeof(part_name), filename, sizeof(filename)) == ESP_OK) {
        if (strcmp(part_name, "code") == 0) {
            read_field_body(&mp, code, sizeof(code));
        } else if (strcmp(part_name, "new_code") == 0) {
            read_field_body(&mp, new_code, sizeof(new_code));
        } else if (strcmp(part_name, "desc") == 0) {
            read_field_body(&mp, desc, sizeof(desc));
        } else if (strcmp(part_name, "file") == 0 && filename[0]) {
            if (new_code[0]) {
                char fallback[24];
                snprintf(fallback, sizeof(fallback), "clip_%s.wav", new_code);
                sanitize_upload_name(filename, fallback, dest, sizeof(dest));
            } else if (code[0]) {
                if (dialplan_clip_get_filename(code, dest, sizeof(dest)) != ESP_OK) {
                    err_msg = "code not found";
                    break;
                }
            } else {
                err_msg = "missing code";
                break;
            }

            const char *err = convert_upload_part(&mp, dest);
            if (err) { err_msg = err; break; }

            if (new_code[0]) {
                esp_err_t aerr = dialplan_clip_add(new_code, dest, desc[0] ? desc : dest);
                if (aerr != ESP_OK) {
                    /* Audio uploaded fine but the code itself couldn't be
                     * added (bad code / conflict / table full) -- still
                     * leave the file in storage; report the failure rather
                     * than silently dropping it. */
                    err_msg = (aerr == ESP_ERR_INVALID_ARG) ? "code must be 2-6 digits" :
                              (aerr == ESP_ERR_INVALID_STATE) ? "that code conflicts with an existing one" :
                              "dial code limit reached";
                    break;
                }
            }
            wrote = true;
        } else {
            uint8_t scratch[128];
            while (multipart_read_body(&mp, scratch, sizeof(scratch)) > 0) { } /* discard */
        }
    }
    multipart_reader_close(&mp);

    if (!wrote) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, err_msg);
        return ESP_FAIL;
    }
    if (dest[0]) prompts_mark_generated(dest);
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/settings?tab=dialcodes");
    httpd_resp_send(req, NULL, 0);
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
    esp_err_t err = storage_write_begin_local(name, total, &w);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "upload '%s': storage_write_begin_local failed: %s", name, esp_err_to_name(err));
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

    prompts_mark_generated(name);
    ESP_LOGI(TAG, "uploaded '%s' (%u bytes)", name, (unsigned)received);
    httpd_resp_sendstr(req, "ok\n");
    return ESP_OK;
}

/* POST /prompt_text_set -- form body name=..&text=.. (see append_prompt_field()).
 * Sets/updates the desired spoken text for a clip; doesn't touch the audio
 * itself -- that only changes once gen_prompt.py --sync uploads a WAV under
 * the same name (media_upload_handler above, which calls
 * prompts_mark_generated()). Blank text removes the tracked entry. */
#define PROMPT_FORM_MAX_BODY 1024
static esp_err_t prompt_text_set_handler(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len >= PROMPT_FORM_MAX_BODY) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad form body");
        return ESP_OK;
    }
    char body[PROMPT_FORM_MAX_BODY];
    int r = httpd_req_recv(req, body, req->content_len);
    if (r <= 0) { httpd_resp_send_500(req); return ESP_FAIL; }
    body[r] = '\0';

    char name[PROMPT_NAME_MAX] = {0}, text[PROMPT_TEXT_MAX] = {0};
    form_field(body, "name", name, sizeof(name));
    form_field(body, "text", text, sizeof(text));

    esp_err_t err = prompts_set_text(name, text);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "prompt_text_set '%s' failed: %s", name, esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, esp_err_to_name(err));
        return ESP_OK;
    }

    /* Bounce back to whichever page sent the form -- Referer is the only
     * place that's recorded, since this one endpoint serves three different
     * prompt fields (Operator, Record message, and each Dial Codes row),
     * all on the Dial Codes & Prompts settings tab. Falls back to the
     * general tab, which is harmless (not "settings.php-style redirect"
     * territory -- nothing else reads this header). */
    char referer[128];
    if (httpd_req_get_hdr_value_str(req, "Referer", referer, sizeof(referer)) == ESP_OK) {
        httpd_resp_set_status(req, "303 See Other");
        httpd_resp_set_hdr(req, "Location", referer);
    } else {
        httpd_resp_set_status(req, "303 See Other");
        httpd_resp_set_hdr(req, "Location", "/settings");
    }
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* True if `req`'s peer address is in the same IPv4 subnet as this board's
 * own WiFi-STA address (peer & netmask == our_ip & netmask). Lets
 * gen_prompt.py --sync skip the setup-password check entirely when it's
 * unmistakably a local, on-LAN caller -- the same trust boundary as every
 * other endpoint on this web UI (nothing else here has auth either). The
 * secret stays meaningful for the case this is really for: a board set up
 * over VPN/port-forward/etc. from off the local network, where "same
 * subnet" naturally fails closed and the password is still required.
 * Best-effort and fails closed (false, i.e. "still require the secret") on
 * anything that doesn't cleanly resolve to an IPv4 peer against our own
 * IPv4 STA netif -- a real IPv6 peer, no STA netif, a lookup error, etc. --
 * so an unexpected setup never silently grants LAN trust it can't verify. */
static bool peer_on_lan(httpd_req_t *req)
{
    int sockfd = httpd_req_to_sockfd(req);
    if (sockfd < 0) return false;

    struct sockaddr_in6 peer = {0};
    socklen_t peer_len = sizeof(peer);
    if (getpeername(sockfd, (struct sockaddr *)&peer, &peer_len) != 0) return false;

    /* esp_http_server listens on a single AF_INET6 socket when LWIP_IPV6 is
     * on (it is here) -- real IPv4 peers show up v4-mapped (::ffff:a.b.c.d),
     * not as a plain AF_INET sockaddr. Pull the last 4 bytes out of that
     * form; anything else (a genuine IPv6 peer) has no IPv4 subnet to
     * compare against. */
    static const uint8_t v4_mapped_prefix[12] = {0,0,0,0,0,0,0,0,0,0,0xff,0xff};
    if (peer.sin6_family != AF_INET6 ||
        memcmp(peer.sin6_addr.s6_addr, v4_mapped_prefix, sizeof(v4_mapped_prefix)) != 0) {
        return false;
    }
    uint32_t peer_ip;
    memcpy(&peer_ip, &peer.sin6_addr.s6_addr[12], sizeof(peer_ip));

    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!sta) return false;
    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(sta, &ip_info) != ESP_OK) return false;

    return (peer_ip & ip_info.netmask.addr) == (ip_info.ip.addr & ip_info.netmask.addr);
}

/* GET /api/prompt_requests?secret=... -- lets a host-side script (gen_prompt.py
 * --sync) fetch the list of tracked prompts without needing any other
 * credential on the board. Uses the shared setup password
 * (voip_setup_password(), Settings -> VOIP) -- same "per board, shared with
 * tools you trust" security model, at the user's request. Skipped entirely
 * for same-subnet callers (peer_on_lan()) -- everything else on this UI
 * already trusts the LAN unconditionally, so gating just this one endpoint
 * bought nothing once the settings pages themselves show this same prompt
 * text with no auth at all. Off-LAN callers (VPN/port-forward/etc.) still
 * need it, and get refused outright if it was never set, rather than
 * falling back to "no auth". */
static esp_err_t prompt_requests_json_handler(httpd_req_t *req)
{
    if (!peer_on_lan(req)) {
        const char *want = voip_setup_password();
        if (!want[0]) {
            httpd_resp_send_err(req, HTTPD_403_FORBIDDEN,
                                 "set a setup password first (Settings -> VOIP) "
                                 "-- only needed for off-LAN access, e.g. over a VPN");
            return ESP_OK;
        }

        char query[160], secret[64] = {0};
        if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
            httpd_query_key_value(query, "secret", secret, sizeof(secret));
        }
        if (strcmp(secret, want) != 0) {
            httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "bad secret");
            return ESP_OK;
        }
    }

    /* Heap, not stack -- PROMPT_MAX_ENTRIES * sizeof(prompt_entry_t) is
     * ~6.9KB (each entry carries two 200-byte text fields), which overflowed
     * the httpd worker task's 8KB stack when it was a local array here
     * (silent corruption/hang, not a clean panic -- see media_upload_handler
     * above for the same fix on the same task). */
    prompt_entry_t *list = malloc(PROMPT_MAX_ENTRIES * sizeof(prompt_entry_t));
    if (!list) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    size_t n = prompts_list(list, PROMPT_MAX_ENTRIES);
    cJSON *arr = cJSON_CreateArray();
    for (size_t i = 0; i < n; i++) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "name", list[i].name);
        cJSON_AddStringToObject(e, "text", list[i].text);
        cJSON_AddBoolToObject(e, "pending", strcmp(list[i].text, list[i].generated_text) != 0);
        cJSON_AddItemToArray(arr, e);
    }
    free(list);
    char *out = cJSON_PrintUnformatted(arr);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, out ? out : "[]");
    cJSON_Delete(arr);
    free(out);
    return ESP_OK;
}

esp_err_t webui_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    /* Default is 8 -- we register 42 routes below (two share the media
     * path, one GET, one POST, counted separately) and it keeps growing.
     * Past the cap, httpd_register_uri_handler() just fails per-route, which
     * used to go unchecked below, silently dropping the last couple of
     * routes -- the loop below now at least logs it, but leave headroom. */
    config.max_uri_handlers = 48;
    /* Default httpd task stack is 4 KB. Our page handlers stack a few
     * storage_entry_t[32]/dialplan_entry_info_t[40] arrays and then descend
     * into SPIFFS readdir -> esp_flash_read, which is itself stack-hungry;
     * that combination overflowed the 4 KB guard on the Settings page. 8 KB
     * covers that. Only one httpd worker task exists (no concurrent-
     * connection stack multiplication), so this is a one-time cost, not
     * per-request. */
#ifdef CONFIG_AUDIO_CONVERT_MP3
    /* minimp3's own mp3dec_decode_frame() declares a ~16KB
     * `mp3dec_scratch_t` local (grbuf/syn/maindata buffers, see
     * minimp3/minimp3.h) -- confirmed via a live panic + addr2line against
     * the crash PC, not a guess. 16 KB total wasn't enough either; this
     * needs real headroom on top of that ~16KB single local. This is the
     * REAL cost of MP3 support on this hardware (a permanent +24KB on the
     * one always-running httpd task), not the ~23KB flash delta -- see
     * audio_convert's Kconfig help text. */
    config.stack_size = 32768;
#else
    /* 8 KB was sized for SPIFFS-only traversal (see the comment above this
     * block). Confirmed live: the Dial Codes tab -- append_dialcodes_tab()
     * calls append_file_select() once per row plus once more for the "add"
     * form, and each call puts storage_entry_t files[32]+bundled[16]
     * (~2KB) on the stack on top of settings_page_handler()'s and
     * append_dialcodes_tab()'s own locals -- reliably crashed/rebooted a
     * board with a microSD card mounted (storage_list() there hits
     * FATFS/SDMMC, not SPIFFS) while the identical request against a board
     * with no card worked fine. FATFS/SDMMC is a known heavier stack
     * consumer than SPIFFS in ESP-IDF; 8 KB not accounting for that is the
     * likely culprit, though this wasn't confirmed via addr2line the way
     * the MP3 case above was (no crash log was available at the time).
     * 16 KB is a generous, cheap (one-time, single always-running task)
     * bump rather than a tightly-measured minimum. (append_file_select()
     * itself no longer touches the SD card at all -- it now always lists
     * storage_list_internal(), since a dial-code clip only ever lives on
     * internal flash -- but the Settings -> Storage tab's own SD listing
     * still walks FATFS/SDMMC on the same task, so this headroom stays.) */
    config.stack_size = 16384;
#endif

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }

    static const httpd_uri_t routes[] = {
        { .uri = "/",                .method = HTTP_GET, .handler = index_handler },
        { .uri = "/recordings",      .method = HTTP_GET, .handler = recordings_page_handler },
        { .uri = "/guide",           .method = HTTP_GET, .handler = redirect_to_index_handler },
        { .uri = "/settings",        .method = HTTP_GET, .handler = settings_page_handler },
        { .uri = "/dialplan",        .method = HTTP_GET, .handler = redirect_to_index_handler },
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
        { .uri = "/board_hw_set",    .method = HTTP_POST, .handler = board_hw_set_handler },
        { .uri = "/offhook_alert",   .method = HTTP_POST, .handler = offhook_alert_handler },
        { .uri = "/idle_power",      .method = HTTP_POST, .handler = idle_power_settings_handler },
        { .uri = "/record_max_ms",   .method = HTTP_POST, .handler = record_max_ms_handler },
        { .uri = "/dialcode_add",           .method = HTTP_POST, .handler = dialcode_add_handler },
        { .uri = "/dialcode_set_code",      .method = HTTP_POST, .handler = dialcode_set_code_handler },
        { .uri = "/dialcode_set_filename",  .method = HTTP_POST, .handler = dialcode_set_filename_handler },
        { .uri = "/dialcode_set_desc",      .method = HTTP_POST, .handler = dialcode_set_desc_handler },
        { .uri = "/dialcode_delete/*",      .method = HTTP_POST, .handler = dialcode_delete_handler },
        { .uri = "/dialcode_upload",        .method = HTTP_POST, .handler = dialcode_upload_handler },
        { .uri = "/operator_upload",        .method = HTTP_POST, .handler = operator_upload_handler },
        { .uri = "/prompt_text_set",        .method = HTTP_POST, .handler = prompt_text_set_handler },
        { .uri = "/api/prompt_requests",    .method = HTTP_GET,  .handler = prompt_requests_json_handler },
        { .uri = "/voip_set",               .method = HTTP_POST, .handler = voip_set_handler },
        { .uri = "/voip_check",             .method = HTTP_POST, .handler = voip_check_handler },
        { .uri = "/voip_setup_password",    .method = HTTP_POST, .handler = voip_setup_password_handler },
        { .uri = "/log_level",              .method = HTTP_POST, .handler = log_level_handler },
        { .uri = "/test/mode",              .method = HTTP_POST, .handler = test_mode_handler },
        { .uri = "/test/loopback",          .method = HTTP_POST, .handler = test_loopback_handler },
        { .uri = "/test/inject",            .method = HTTP_POST, .handler = test_inject_handler },
        { .uri = "/test/sd",                .method = HTTP_POST, .handler = test_sd_handler },
#if CONFIG_BOARD_TYPE_SMD2
        { .uri = "/speaker_volume_live",    .method = HTTP_POST, .handler = speaker_volume_live_handler },
        { .uri = "/speaker_volume",         .method = HTTP_POST, .handler = speaker_volume_handler },
#endif
        { .uri = "/aec_measure",            .method = HTTP_POST, .handler = aec_measure_handler },
        { .uri = "/aec_delay_apply",        .method = HTTP_POST, .handler = aec_delay_apply_handler },
        { .uri = "/aec_enable",             .method = HTTP_POST, .handler = aec_enable_handler },
        { .uri = "/voip_server_script",     .method = HTTP_GET,  .handler = voip_server_script_handler },
        { .uri = "/voip_instructions.txt",  .method = HTTP_GET,  .handler = voip_instructions_txt_handler },
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

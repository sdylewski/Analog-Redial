#include "webui.h"
#include "sdkconfig.h"
#include "esp_http_server.h"
#include "cJSON.h"
#include "i18n.h"
#include "wifi.h"
#include "board.h"
#include "board_config.h"
#include "esp_idf_version.h"
#include "storage.h"
#include "logbuf.h"
#include "activity.h"
#include "telephony.h"
#include "recorder.h"
#include "prompts.h"
#include "dialplan.h"
#include "voip.h"
#include "bt_companion.h"
#include "selftest.h"
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
#include "esp_partition.h"
#include "esp_ota_ops.h"
#include "esp_image_format.h"
#include "esp_app_desc.h"
#include "nvs.h"
#include "esp_netif.h"
#include "nvs_flash.h"
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

/* Small first-try read size for wav_duration_str()'s duration scan
 * specifically (a different, much smaller job than the conversion-time
 * scan above) -- most files in this project's own storage have a plain
 * ~44-byte canonical WAV header, so this is normally enough on the first
 * try. Falls back to UPLOAD_HEADER_SCAN_BYTES's full read when it isn't
 * (see wav_duration_str()'s doc comment for why that fallback has to
 * exist, not just might). */
#define WAV_DURATION_SCAN_BYTES 512

/* Per-backend cap on how many files storage_list_sd()/storage_list_internal()
 * return to any one listing (Storage tab, Recordings page, /api/recordings,
 * media_handler's existence check). Past this many files on one backend,
 * the extra ones don't paginate -- there's no "next page" UI -- they just
 * silently don't appear (and, for media_handler specifically, silently
 * 404 if you already have a direct link to one). 100 is comfortable
 * headroom over any realistic personal-use file count; the arrays sized to
 * it are heap-allocated everywhere (see each call site), never stack --
 * this project has hit a real httpd-task stack overflow from smaller
 * stack-allocated storage_entry_t arrays once already (see
 * UPLOAD_HEADER_SCAN_BYTES's doc comment above and this file's own
 * "append_file_select()" removal note). */
#define STORAGE_LIST_MAX 100
#define MEMORY_MAP_MAX_PARTS 16

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
static void js_escape(const char *in, char *out, size_t outsz);

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

    bt_companion_status_t bt_st = bt_companion_get_status();
    cJSON_AddBoolToObject(root, "bt_enabled", bt_st.enabled);
    cJSON_AddBoolToObject(root, "bt_link_up", bt_st.link_up);
    cJSON_AddBoolToObject(root, "bt_connected", bt_st.bt_connected);
    cJSON_AddStringToObject(root, "bt_paired_name", bt_st.paired_name);
    cJSON_AddStringToObject(root, "bt_addr", bt_st.bd_addr);

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

extern const uint8_t favicon_svg_start[] asm("_binary_favicon_svg_start");
extern const uint8_t favicon_svg_end[]   asm("_binary_favicon_svg_end");
extern const uint8_t favicon_ico_start[] asm("_binary_favicon_ico_start");
extern const uint8_t favicon_ico_end[]   asm("_binary_favicon_ico_end");

/* GET /static/favicon.svg -- what append_page_head()'s <link rel="icon"
 * type="image/svg+xml"> points at. Scalable, and what Chrome/Firefox
 * actually use. */
static esp_err_t favicon_svg_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "image/svg+xml");
    httpd_resp_send(req, (const char *)favicon_svg_start,
                     favicon_svg_end - favicon_svg_start);
    return ESP_OK;
}

/* GET /favicon.ico -- a real PNG-in-ICO raster fallback (see
 * firmware/bringup/gen_favicon.py), not the SVG re-served under an .ico
 * name. An earlier version did serve the SVG here on the theory that
 * browsers key off Content-Type, not the URL extension -- true for
 * Chrome/Firefox, but Safari's own-initiative /favicon.ico probe (which
 * runs regardless of the page's <link rel="icon"> tags) has never reliably
 * rendered SVG there, which is why the tab icon wasn't showing up in
 * Safari. This path and the <link rel="icon" href="/favicon.ico"> tag in
 * append_page_head() now both point at real raster data instead. */
static esp_err_t favicon_ico_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "image/x-icon");
    httpd_resp_send(req, (const char *)favicon_ico_start,
                     favicon_ico_end - favicon_ico_start);
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

/* Escapes for a single-quoted JS string literal embedded directly in an
 * inline <script> block (see append_bluetooth_tab()'s live-status JS) --
 * i18n_str() strings are translator-supplied at runtime, not just the
 * hardcoded English defaults, so a translated string containing an
 * apostrophe or backslash has to be escaped here rather than assumed safe. */
static void js_escape(const char *in, char *out, size_t outsz)
{
    size_t o = 0;
    if (outsz == 0) return;
    for (size_t i = 0; in[i] && o < outsz - 1; i++) {
        if ((in[i] == '\'' || in[i] == '\\') && o + 2 < outsz) {
            out[o++] = '\\';
            out[o++] = in[i];
        } else if (in[i] == '\n' || in[i] == '\r') {
            continue; /* strip -- these are short single-line UI strings */
        } else {
            out[o++] = in[i];
        }
    }
    out[o] = '\0';
}

/* Every page handler here builds its response into one malloc(cap) buffer
 * via repeated `off += snprintf(buf + off, ((size_t)off < cap) ? cap - off
 * : 0, ...)` calls, then calls httpd_resp_send(req, buf, <length>) once at
 * the end. snprintf's return value is the length that WOULD have been
 * written, which keeps growing past `cap` once truncation starts (each
 * further guarded call passes size 0 and writes nothing, but still returns
 * its own would-be length) -- so a too-small `cap` estimate means `off`
 * ends up bigger than the buffer actually is. Passing that raw `off`
 * straight to httpd_resp_send() tells it to read past the end of a
 * malloc'd buffer -- adjacent heap memory ends up in the HTTP response.
 * Route every such call through this instead of a raw `off`. */
static size_t clamp_send_len(int off, size_t cap)
{
    if (off <= 0) return 0;
    size_t len = (size_t)off;
    return (len < cap) ? len : (cap ? cap - 1 : 0);
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
typedef struct { const char *href; const char *label; const char *i18n_key; } nav_tab_t;
static const nav_tab_t NAV_TABS[] = {
    { "/",           "Status",     "nav.status" },
    { "/recordings", "Recordings", "nav.recordings" },
    { "/settings",   "Settings",   "nav.settings" },
};
#define NAV_TABS_N (sizeof(NAV_TABS) / sizeof(NAV_TABS[0]))

/* Languages with a real regional tone treatment today (tones/tone_regions.h
 * -- kept as a small, separate list here rather than exposing that
 * component's private table, since this is purely a display concern: name +
 * flag emoji for the header picker below). Deliberately short -- see
 * i18n_str()'s doc comment (i18n.h) for why this project isn't bundling a
 * long language list into firmware. Adding a region to tone_regions.h and
 * forgetting to add it here just means that language doesn't show in the
 * picker yet (safe, obvious gap) -- board_language_set() itself still
 * accepts any code via the console/API either way. */
typedef struct { const char *code; const char *flag; const char *name; } ui_lang_t;
static const ui_lang_t UI_LANGUAGES[] = {
    { "en", "\xf0\x9f\x87\xba\xf0\x9f\x87\xb8", "English" },
    { "it", "\xf0\x9f\x87\xae\xf0\x9f\x87\xb9", "Italiano" },
    { "de", "\xf0\x9f\x87\xa9\xf0\x9f\x87\xaa", "Deutsch" },
    { "fr", "\xf0\x9f\x87\xab\xf0\x9f\x87\xb7", "Fran\xc3\xa7""ais" },
    { "es", "\xf0\x9f\x87\xaa\xf0\x9f\x87\xb8", "Espa\xc3\xb1ol" },
};
#define UI_LANGUAGES_N (sizeof(UI_LANGUAGES) / sizeof(UI_LANGUAGES[0]))

/* Curated POSIX TZ strings for the Quiet Hours card's timezone dropdown --
 * same "short, deliberately incomplete" philosophy as UI_LANGUAGES above:
 * board_timezone_set() (and the still-live /timezone_set endpoint) accept
 * any POSIX TZ string via the console/API either way, so a zone missing
 * here is a UI gap, not a functional one. "" means UTC (board_timezone_get()'s
 * own convention -- see its callers). Look a zone's string up at
 * github.com/nayarsystems/posix_tz_db if adding one. */
typedef struct { const char *tz; const char *label; } ui_tz_t;
static const ui_tz_t UI_TIMEZONES[] = {
    { "", "UTC" },
    { "HST10", "Hawaii (no DST)" },
    { "AKST9AKDT,M3.2.0,M11.1.0", "US Alaska" },
    { "PST8PDT,M3.2.0,M11.1.0", "US Pacific" },
    { "MST7", "US Arizona (no DST)" },
    { "MST7MDT,M3.2.0,M11.1.0", "US Mountain" },
    { "CST6CDT,M3.2.0,M11.1.0", "US Central" },
    { "EST5EDT,M3.2.0,M11.1.0", "US Eastern" },
    { "GMT0BST,M3.5.0/1,M10.5.0", "UK / Ireland" },
    { "CET-1CEST,M3.5.0,M10.5.0/3", "Central Europe" },
    { "EET-2EEST,M3.5.0/3,M10.5.0/4", "Eastern Europe" },
    { "IST-5:30", "India (no DST)" },
    { "CST-8", "China (no DST)" },
    { "JST-9", "Japan (no DST)" },
    { "AEST-10AEDT,M10.1.0,M4.1.0/3", "Australia Eastern" },
};
#define UI_TIMEZONES_N (sizeof(UI_TIMEZONES) / sizeof(UI_TIMEZONES[0]))

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
        "<link rel=\"icon\" href=\"/favicon.ico\" sizes=\"any\">"
        "<link rel=\"icon\" type=\"image/svg+xml\" href=\"/static/favicon.svg\">"
        "<link rel=\"stylesheet\" href=\"/static/style.css\"></head><body>"
        "<header><h1>%s <span class=\"dot\">%s</span></h1><nav>",
        name, page_title, auto_refresh ? "<meta http-equiv=\"refresh\" content=\"5\">" : "",
        name, i18n_str("header.subtitle", "Analog Redial"));
    for (size_t i = 0; i < NAV_TABS_N; i++) {
        bool active = strcmp(NAV_TABS[i].href, active_href) == 0;
        off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
            "<a%s href=\"%s\">%s</a>", active ? " class=\"active\"" : "",
            NAV_TABS[i].href, i18n_str(NAV_TABS[i].i18n_key, NAV_TABS[i].label));
    }
    /* Language picker: lives inside <nav> (not as a 3rd <header> child --
     * see append_page_head()'s git history for why that broke the
     * space-between flex layout once already) so it just flows after the
     * last tab. Auto-submits on change; language_set_handler() bounces back
     * to whichever page this was, via Referer. Only a UI-language/prompt-
     * accent selector today -- see i18n.h for what "changes the UI
     * language" actually means right now (nothing yet but the tone accent;
     * this is the visible seam for when that's built out). */
    char cur_lang[8];
    board_language_get(cur_lang, sizeof(cur_lang));
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<form method=\"POST\" action=\"/language_set\" class=\"langpick\">"
        "<select name=\"lang\" onchange=\"this.form.submit()\" title=\"Language\">");
    for (size_t i = 0; i < UI_LANGUAGES_N; i++) {
        off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
            "<option value=\"%s\"%s>%s %s</option>",
            UI_LANGUAGES[i].code, strcmp(UI_LANGUAGES[i].code, cur_lang) == 0 ? " selected" : "",
            UI_LANGUAGES[i].flag, UI_LANGUAGES[i].name);
    }
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0, "</select></form></nav>");

    /* Shown on every page (this function is every page's shared head) --
     * see board_sd_line1_conflict()'s own doc comment (board.h) for why
     * this specific combination gets a loud banner instead of staying
     * silent, and why line 1 has already been powered down by the time
     * anyone sees this (app_main.c's periodic check, not this render). */
    if (board_sd_line1_conflict()) {
        off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
            "<div class=\"hazard-banner\">%s</div>",
            i18n_str("hazard.sd_line1_conflict",
                     "&#9888; microSD card detected, but this board is configured for 2 lines "
                     "-- microSD and line 1 share the same wiring and can't both be active. "
                     "Line 1 has been disabled until this is fixed: remove the card, or "
                     "reconfigure this board in <a href=\"/settings?tab=hardware\">Settings "
                     "&rarr; Hardware</a> if it's actually meant to be single-line + microSD."));
    }
    if (board_type_mismatch()) {
        char recorded[16];
        board_type_recorded(recorded, sizeof(recorded));
        char mismatch_hint[420];
        snprintf(mismatch_hint, sizeof(mismatch_hint),
            i18n_str("hazard.board_type_mismatch",
                     "&#9888; This firmware is built for <b>%s</b>, but this board last "
                     "confirmed booting as <b>%s</b> -- GPIO pin routing differs between board "
                     "types, so things like microSD or line 1 may silently misbehave. Reflash "
                     "the correct firmware, or if this board genuinely changed, confirm it in "
                     "<a href=\"/settings?tab=hardware\">Settings &rarr; Hardware</a>."),
            board_pcb_variant_name(), recorded[0] ? recorded : "(unknown)");
        off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
            "<div class=\"hazard-banner\">%s</div>", mismatch_hint);
    }
    return off;
}

/* ---- WiFi provisioning (setup SoftAP) UI ---- */

static void reboot_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1500)); /* let the HTTP response flush first */
    esp_restart();
}

/* Shared by both SSID pickers (the captive setup page and Settings ->
 * General): fetches /api/wifi_scan into a <select id="ssid_select">, with an
 * "Other... (enter manually)" option that swaps in a plain text
 * #ssid_manual input -- only one of the two ever carries name="ssid" at a
 * time, so the form always POSTs a single unambiguous value. If the select
 * has a data-current attribute (the currently-configured SSID, set by
 * Settings' form -- empty/absent on the setup page, which has none yet) and
 * a scan doesn't find that network, falls back to manual mode pre-filled
 * with it rather than silently losing it from view. */
static const char *WIFI_SCAN_JS =
    "function wifiRenderOptions(sel,list,err){"
    "sel.innerHTML='';"
    "if(list.length){"
    "var ph=document.createElement('option');ph.value='';ph.textContent='Select a network\\u2026';"
    "ph.disabled=true;ph.selected=true;sel.appendChild(ph);"
    "list.forEach(function(n){var o=document.createElement('option');o.value=n.ssid;"
    "o.textContent=n.ssid+' ('+n.rssi+' dBm)'+(n.secured?' \\uD83D\\uDD12':'');sel.appendChild(o);});"
    "}else{"
    "var o2=document.createElement('option');o2.value='';"
    "o2.textContent=err?'Scan failed \\u2013 enter manually below':'No networks found \\u2013 enter manually below';"
    "sel.appendChild(o2);"
    "}"
    "var other=document.createElement('option');other.value='__manual__';"
    "other.textContent='Other\\u2026 (enter manually)';sel.appendChild(other);"
    "var cur=sel.dataset.current;"
    "if(cur){"
    "var match=false;"
    "for(var i=0;i<sel.options.length;i++){if(sel.options[i].value===cur){sel.options[i].selected=true;match=true;break;}}"
    "if(!match){sel.value='__manual__';wifiSelectChange();document.getElementById('ssid_manual').value=cur;}"
    "}"
    "}"
    "function wifiRescan(){"
    "var sel=document.getElementById('ssid_select');"
    "sel.innerHTML='<option value=\"\">Scanning\\u2026</option>';"
    "fetch('/api/wifi_scan').then(function(r){return r.json();})"
    ".then(function(j){wifiRenderOptions(sel,j.networks||[],j.error);})"
    ".catch(function(){wifiRenderOptions(sel,[],true);});"
    "}"
    "function wifiSelectChange(){"
    "var sel=document.getElementById('ssid_select');"
    "var row=document.getElementById('ssid_manual_row');"
    "var man=document.getElementById('ssid_manual');"
    "if(sel.value==='__manual__'){"
    "row.style.display='';sel.removeAttribute('name');man.name='ssid';man.required=true;man.focus();"
    "}else{"
    "row.style.display='none';sel.name='ssid';man.removeAttribute('name');man.required=false;"
    "}"
    "}"
    "wifiRescan();";

/* Recording a message or greeting only ever finishes at hang-up, well after
 * the page that started it was rendered -- there's no request/response to
 * hook a refresh onto. Poll instead: watch /api/status for any line
 * dropping OUT of RECORDING (a state telephony.c already reports, see
 * telephony_state_name()) and reload once that happens. Shared by the
 * Recordings page (211 messages) and the Hardware tab's Answering Machine
 * card (212 greetings) -- same signal either way, just different rows
 * waiting to update once it fires. */
static const char *RECORDING_RELOAD_JS =
    "(function(){"
    "var wasRecording=false;"
    "setInterval(function(){"
    "fetch('/api/status').then(function(r){return r.json();}).then(function(d){"
    "var recording=(d.lines||[]).some(function(l){return l.state==='RECORDING';});"
    "if(wasRecording&&!recording){location.reload();}"
    "wasRecording=recording;"
    "}).catch(function(){});"
    "},2000);"
    "})();";

/* Shared by every "enable this feature" master checkbox (VOIP, Answering
 * Machine, Bluetooth, ...): shows/hides the rest of that feature's fields
 * based on the checkbox's live state, so unchecking it doesn't require a
 * round trip to the server just to see the rest of the section collapse.
 * Purely cosmetic -- the hidden fields are still part of the DOM (just
 * [hidden]), so whatever's in them still submits normally with their own
 * form's Save button. Each call site's initial [hidden] state is rendered
 * server-side from the actual persisted value, so this JS only needs to
 * handle live toggling after that, not an on-load sync. */
static const char *FEATURE_TOGGLE_JS =
    "function toggleSection(cb,id){var e=document.getElementById(id);if(e)e.hidden=!cb.checked;}";

/* Setup page served on the SoftAP: an SSID picker (scanned live) + password
 * form. Kept minimal and self-contained (its own <form>, the shared
 * stylesheet) so it works before any network config exists. */
static esp_err_t wifi_setup_page(httpd_req_t *req)
{
    static const char *tmpl =
        "<!doctype html><html><head><meta charset=\"utf-8\">"
        "<title>Analog Redial \xc2\xb7 %s</title>"
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
        "<link rel=\"icon\" href=\"/favicon.ico\" sizes=\"any\">"
        "<link rel=\"icon\" type=\"image/svg+xml\" href=\"/static/favicon.svg\">"
        "<link rel=\"stylesheet\" href=\"/static/style.css\"></head><body>"
        "<header><h1>Analog Redial <span class=\"dot\">%s</span></h1></header>"
        "<main class=\"auth-page\"><div class=\"auth-card\">"
        "<h1>%s</h1>"
        "<p class=\"hint\">%s</p>"
        "<form method=\"POST\" action=\"/wifi_set\" class=\"settings-form\">"
        "<label>%s"
        "<select id=\"ssid_select\" name=\"ssid\" required autofocus onchange=\"wifiSelectChange()\">"
        "<option value=\"\">%s\xe2\x80\xa6</option>"
        "</select></label>"
        "<div id=\"ssid_manual_row\" style=\"display:none\"><label>%s"
        "<input id=\"ssid_manual\" placeholder=\"%s\" autocomplete=\"off\"></label></div>"
        "<p class=\"hint\"><a href=\"#\" onclick=\"wifiRescan();return false;\">%s</a></p>"
        "<noscript><label>%s <input name=\"ssid\" required></label></noscript>"
        "<label>%s <input type=\"password\" name=\"password\" "
        "placeholder=\"%s\"></label>"
        "<div class=\"toolbar sub\"><button type=\"submit\" class=\"primary\">"
        "%s</button></div>"
        "</form></div></main>"
        "<script>%s</script>"
        "</body></html>";

    const char *setup_title = i18n_str("wifisetup.title", "WiFi setup");
    const char *connect_h1 = i18n_str("wifisetup.h1", "Connect to your WiFi");
    const char *intro = i18n_str("wifisetup.intro",
        "Enter your home network so the phone can join it. It'll reboot and connect; this "
        "setup network then disappears.");
    const char *ssid_label = i18n_str("general.wifi.ssid_label", "SSID");
    const char *network_name_label = i18n_str("wifisetup.network_name", "Network name (SSID)");
    const char *scanning = i18n_str("general.wifi.scanning", "Scanning for nearby networks");
    const char *enter_network = i18n_str("general.wifi.enter_network", "Enter network name");
    const char *rescan = i18n_str("general.wifi.rescan", "Rescan");
    const char *password_label = i18n_str("general.wifi.password_label", "Password");
    const char *password_placeholder = i18n_str("general.wifi.password_placeholder",
        "leave blank for an open network");
    const char *save_connect = i18n_str("wifisetup.save_connect", "Save & connect");

    size_t cap = strlen(tmpl) + strlen(WIFI_SCAN_JS)
               + strlen(setup_title) * 2 + strlen(connect_h1) + strlen(intro)
               + strlen(network_name_label) * 2 + strlen(scanning) + strlen(enter_network)
               + strlen(rescan) + strlen(ssid_label) + strlen(password_label) * 2
               + strlen(password_placeholder) + strlen(save_connect) + 32;
    char *buf = malloc(cap);
    if (!buf) { httpd_resp_send_500(req); return ESP_FAIL; }
    snprintf(buf, cap, tmpl,
        setup_title, setup_title, connect_h1, intro,
        network_name_label, scanning, network_name_label, enter_network, rescan,
        ssid_label, password_label, password_placeholder, save_connect,
        WIFI_SCAN_JS);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, buf);
    free(buf);
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

/* Shared with index_handler(), which needs to know this BEFORE calling
 * append_page_head() -- see that call site's comment on why the Status
 * page's usual 5s auto-refresh has to be off while this banner has live
 * form fields on it. */
static bool first_boot_banner_needed(void)
{
    bool name_done = board_name_is_set();
    bool hw_applicable = (board_current_type() == BOARD_TYPE_SMD);
    bool hw_done = board_hw_config_is_set();
    return !(name_done && (!hw_applicable || hw_done));
}

/* First-boot setup banner, shown on the Status page only while there's
 * still something a human hasn't reviewed: the board name (board_name_get()
 * returns the generic CONFIG_MDNS_HOSTNAME default until board_name_set()
 * is called at least once) and/or, on SMD boards, the line2/microSD
 * hardware config (board_hw_config_is_set() -- see board.h). Hardware
 * already WORKS out of the box via board_hw_config_autodetect()'s guess
 * (board.c, run once at boot before ag1170_init()); this is just asking for
 * a one-time human look, not gating function on it. Disappears for good
 * once both are confirmed -- no dismiss button, nothing to re-show. */
static int append_first_boot_banner(char *buf, size_t cap, int off)
{
    if (!first_boot_banner_needed()) return off;
    bool name_done = board_name_is_set();
    bool hw_applicable = (board_current_type() == BOARD_TYPE_SMD);
    bool hw_done = board_hw_config_is_set();

    char name[32];
    board_name_get(name, sizeof(name));

    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<div class=\"card wide firstboot\"><h2>%s</h2>"
        "<p class=\"hint\">%s</p>",
        i18n_str("firstboot.title", "First-time setup"),
        i18n_str("firstboot.intro", "Quick one-time check -- confirm these and this won't show again."));

    if (!name_done) {
        off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
            "<form method=\"POST\" action=\"/board_name\" class=\"settings-form\">"
            "<label>%s "
            "<input name=\"name\" value=\"%s\" maxlength=\"31\" required></label>"
            "<div class=\"toolbar sub\"><button type=\"submit\" class=\"primary\">"
            "%s</button></div>"
            "</form>",
            i18n_str("firstboot.name.label",
                     "Board name (shows up in the UI/logs, and as the <name>.local address)"),
            name, i18n_str("firstboot.name.button", "Confirm name"));
    }

    if (hw_applicable && !hw_done) {
        board_hw_config_t cfg = board_hw_config_get();
        off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
            "<form method=\"POST\" action=\"/board_hw_set\" class=\"settings-form\">"
            "<label>%s"
            "<select name=\"variant\">"
            "<option value=\"2line\"%s>2-line board (second AG1171 SLIC populated)</option>"
            "<option value=\"1line_sd\"%s>1-line board, microSD populated</option>"
            "<option value=\"1line_nosd\"%s>1-line board, no microSD</option>"
            "</select></label>"
            "<div class=\"toolbar sub\"><button type=\"submit\" class=\"primary\">"
            "%s</button></div>"
            "</form>"
            "<p class=\"hint\">%s</p>",
            i18n_str("firstboot.hw.label", "Hardware type -- this board carries either a second "
                     "phone line or a microSD card, never both"),
            cfg.num_lines > 1 ? " selected" : "",
            (cfg.num_lines == 1 && cfg.has_microsd) ? " selected" : "",
            (cfg.num_lines == 1 && !cfg.has_microsd) ? " selected" : "",
            i18n_str("firstboot.hw.button", "Confirm hardware type"),
            i18n_str("firstboot.hw.hint", "Pre-selected from onboard signals so it should already "
                     "be right and working -- just double-check it matches what you actually "
                     "built before confirming."));
    }

    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0, "</div>");
    return off;
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
    size_t cap = 8192 + n * 220 + 1800 /* append_first_boot_banner(), while unconfirmed */
        + (board_type_mismatch() ? 700 : 0); /* append_page_head()'s board-type hazard banner */
    char *buf = malloc(cap);
    if (!buf) { httpd_resp_send_500(req); return ESP_FAIL; }

    /* The usual 5s auto-refresh (append_page_head()'s auto_refresh arg,
     * <meta http-equiv="refresh">) is a full page reload -- fine for a
     * read-only status display, but it wipes whatever's half-typed into the
     * first-boot banner's form fields below every few seconds. Off while
     * that banner has something to ask for; back on once both are
     * confirmed and the banner stops rendering for good. */
    int off = append_page_head(buf, cap, 0, i18n_str("nav.status", "Status"), "/", !first_boot_banner_needed());
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "</header>"
        "<main>"
        "<p class=\"tagline\">%s</p>"
        "<div class=\"cards\">",
        i18n_str("status.tagline", "Plain old telephone service exchange"));
    if (off > 0 && (size_t)off < cap) {
        off = append_first_boot_banner(buf, cap, off);
    }
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<div class=\"card\"><h2>%s</h2>"
        /* Connection status: used to live as a 3rd <header> child (a
         * "<span class=dot>" sibling of <h1>/<nav>), which broke the
         * header's flex space-between layout (nav needs to be the LAST
         * child to sit flush right) -- moved here since it's the same
         * information the System card already shows underneath. */
        "<div class=\"statusrow\"><span class=\"label\">%s</span><span>%s%s%s</span></div>"
        "<div class=\"statusrow\"><span class=\"label\">%s</span><span>%s, %d dBm</span></div>"
        "<div class=\"statusrow\"><span class=\"label\">%s</span><span>%s</span></div>"
        "<div class=\"statusrow\"><span class=\"label\">%s</span><span>%lld s</span></div>"
        "<div class=\"statusrow\"><span class=\"label\">%s</span><span>%s</span></div>"
        "<div class=\"statusrow\"><span class=\"label\">%s</span><span>%s / %s%s</span></div>"
        "</div>",
        i18n_str("status.system.title", "System"),
        i18n_str("status.system.connection", "Connection"),
        wifi_is_connected() ? i18n_str("status.system.connected", "connected")
                             : i18n_str("status.system.disconnected", "disconnected"),
        wifi_is_connected() ? " \xc2\xb7 " : "", wifi_is_connected() ? ip : "",
        i18n_str("status.system.wifi", "WiFi"),
        wifi_get_ssid()[0] ? wifi_get_ssid() : i18n_str("status.system.not_configured", "(not configured)"),
        wifi_rssi(),
        i18n_str("status.system.ip", "IP"), ip,
        i18n_str("status.system.uptime", "Uptime"), (long long)uptime_s,
        i18n_str("status.system.free_heap", "Free heap"),
        fmt_bytes(esp_get_free_heap_size(), hb, sizeof(hb)),
        i18n_str("status.system.storage_used", "Storage used"),
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

        char line_title[32];
        snprintf(line_title, sizeof(line_title), i18n_str("status.line.title", "Line %d"), l);
        off += snprintf(buf + off, cap - off,
            "<div class=\"card\"><h2>%s</h2>"
            "<div class=\"statusrow\"><span class=\"label\">%s</span>"
            "<span class=\"pill %s\">%s</span></div>"
            "<div class=\"statusrow\"><span class=\"label\">%s</span><span>%s</span></div>"
            "<div class=\"statusrow\"><span class=\"label\">%s</span><span>%s</span></div>"
            "</div>",
            line_title, i18n_str("status.line.state", "State"), pill_class_for(st), telephony_state_name(st),
            i18n_str("status.line.last_number", "Last number"), telephony_last_number(l),
            st == TEL_RINGING ? i18n_str("status.line.incoming_call", "Incoming call")
                              : i18n_str("status.line.last_caller", "Last caller"),
            caller);
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
    httpd_resp_send(req, buf, clamp_send_len(off, cap));
    free(buf);
    return ESP_OK;
}

/* A "recording" is something the user recorded from a handset (211 "leave a
 * message" archives, named rec_*.wav). Everything else in the storage
 * partition -- operator.wav, clip_*.wav fun clips -- is a built-in/uploaded
 * asset, managed under Settings, not shown here. */
static bool is_user_recording(const char *name)
{
    return strncmp(name, "rec_", 4) == 0 || strncmp(name, "voicemail_", 10) == 0;
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
    storage_entry_t *entries = malloc(2 * STORAGE_LIST_MAX * sizeof(storage_entry_t));
    if (!entries) { httpd_resp_send_500(req); return ESP_FAIL; }
    size_t n = storage_sd_present() ? storage_list_sd(entries, STORAGE_LIST_MAX) : 0;
    n += storage_list_internal(entries + n, 2 * STORAGE_LIST_MAX - n);
    size_t shown = 0;
    for (size_t i = 0; i < n; i++) if (is_user_recording(entries[i].name)) shown++;

    /* 2048 base + 1024 for append_storage_table()'s one-time bulk-delete
     * <form> wrapper (measured ~746 bytes: outer form + hidden return_to +
     * header select-all checkbox + "Delete selected" row/JS -- 1024 leaves
     * real margin, not just enough). 600/row: measured a real row (32-char
     * STORAGE_MAX_NAME filename, worst case) at ~525 bytes -- the name is
     * repeated up to 6x (checkbox value, <code>, <audio> src, download
     * href, delete formaction, delete confirm text), which a flat
     * per-field guess undercounts fast. Confirmed truncating real device
     * output twice already (7247 and then 8147 of a page that needed
     * ~9600) before landing on these numbers from actual measurement
     * instead of another guess -- if this ever needs to change again,
     * measure a rendered row rather than estimating. */
    size_t cap = 2048 + 1024 + n * 600
        + (board_type_mismatch() ? 700 : 0); /* append_page_head()'s board-type hazard banner */
    char *buf = malloc(cap);
    if (!buf) { free(entries); httpd_resp_send_500(req); return ESP_FAIL; }

    int off = append_page_head(buf, cap, 0, i18n_str("nav.recordings", "Recordings"), "/recordings", false);
    char recordings_h2[48];
    snprintf(recordings_h2, sizeof(recordings_h2), i18n_str("recordings.title", "Recordings (%u)"),
             (unsigned)shown);
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "</header><main>"
        "<div class=\"card\"><h2>%s</h2>"
        "<p class=\"hint\">%s</p>",
        recordings_h2,
        i18n_str("recordings.hint",
                 "Messages left with dial code 211. Built-in audio (operator, clips) is under "
                 "<a href=\"/settings\">Settings</a>."));

    if (shown == 0) {
        off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
            "<div class=\"empty\">%s</div>",
            i18n_str("recordings.none_yet", "none yet \xe2\x80\x94 dial 211, leave a message, hang up"));
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
            "<p class=\"hint\">%s: %s / %s%s. %s "
            "<a href=\"/api/recordings\">%s</a>.</p>"
            "<script>%s</script>"
            "</main></body></html>",
            i18n_str("recordings.storage_label", "Storage"),
            fmt_bytes(sd ? storage_used_bytes_sd() : storage_used_bytes_internal(), su, sizeof(su)),
            fmt_bytes(sd ? storage_cap_bytes_sd() : storage_cap_bytes_internal(), sc, sizeof(sc)),
            sd ? " (SD)" : " (internal flash)",
            i18n_str("recordings.raw_list", "Raw list:"),
            i18n_str("recordings.json_link", "recordings JSON"),
            RECORDING_RELOAD_JS);
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, buf, clamp_send_len(off, cap));
    free(buf);
    free(entries);
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
/* Six tabs, not the nine this once grew to: Operator and Record message
 * were each exactly one card (prompt text + audio upload) and Dial Codes is
 * "what plays for a code" already, so all three now render together under
 * "dialcodes" -- see the dispatch in settings_page_handler(). Inter-phone
 * was dropped outright: it only ever showed the 3xx dial codes read-only
 * with a "not editable here" hint -- identical data the merged Status
 * page's Dial Guide already shows, so the tab added a click with no unique
 * content.
 *
 * Storage was folded into Hardware for one round, then split back out: the
 * per-file WAV-duration parsing in append_storage_table() (open+read+parse
 * every recording/clip) is the single slowest thing in Settings, and the
 * one httpd worker task blocks on it for the whole render -- bundling it
 * with Hardware's cheap stat lookups (and, since then, Answering
 * Machine/Speaker/Echo/idle-power cards piling on top) made an already
 * heavy page render for multiple seconds, which is exactly the kind of
 * window that lets keep-alive connections pile up and exhaust the httpd
 * socket table (see webui_start()'s lru_purge_enable comment). Keeping it
 * a separate tab means visiting Hardware doesn't pay Storage's I/O cost,
 * and vice versa. */
typedef struct { const char *id; const char *label; const char *i18n_key; } settings_tab_t;
static const settings_tab_t SETTINGS_TABS[] = {
    { "general",    "General",              "settings.tab.general" },
    { "phone",      "Phone Settings",       "settings.tab.phone" },
    { "dialcodes",  "Dial Codes & Prompts", "settings.tab.dialcodes" },
    { "voip",       "VOIP",                 "settings.tab.voip" },
    { "bluetooth",  "Bluetooth",            "settings.tab.bluetooth" },
    { "hardware",   "Hardware",             "settings.tab.hardware" },
    { "storage",    "Storage",              "settings.tab.storage" },
    { "diag",       "Diagnostics",          "settings.tab.diag" },
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
            is_active ? " active" : "", SETTINGS_TABS[i].id,
            i18n_str(SETTINGS_TABS[i].i18n_key, SETTINGS_TABS[i].label));
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
        "<div class=\"statusrow\"><span class=\"label\">%s</span>"
        "<span class=\"pill %s\">%s</span></div>"
        "<div class=\"toolbar sub\">"
        "<form method=\"POST\" action=\"%s\" style=\"display:inline\">"
        "<button type=\"submit\">%s</button></form>"
        "<form method=\"POST\" action=\"%s\" style=\"display:inline\">"
        "<button type=\"submit\">%s</button></form>"
        "</div>"
        "<p class=\"hint\">%s</p>"
        "</div>",
        title, i18n_str("general.current", "Current"), pill_class, state_label,
        url_a, opt_a_label, url_b, opt_b_label,
        i18n_str("general.persists_hint", "Persists to flash (NVS) and survives a reboot."));
}

static int append_general_tab(char *buf, size_t cap, int off)
{
    char name[32], host[64];
    board_name_get(name, sizeof(name));
    board_hostname(host, sizeof(host));
    /* This hint's translated text still carries a %s (for the hostname) --
     * render it into its own buffer first, since it can't be both "the
     * format string" and "one argument of the outer format string" in the
     * same snprintf call. Same two-step pattern used for every other hint
     * below that has a placeholder of its own. */
    char boardname_hint[400];
    snprintf(boardname_hint, sizeof(boardname_hint),
        i18n_str("general.boardname.hint",
                 "Labels this board in the UI and logs when you run several on one network. "
                 "Also sets the mDNS address: reachable as <code>%s.local</code>. Hardware type "
                 "and serial are on the <a href=\"/settings?tab=hardware\">Hardware</a> tab."),
        host);
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<div class=\"card\"><h2>%s</h2>"
        "<form method=\"POST\" action=\"/board_name\" class=\"settings-form\">"
        "<label>%s <input name=\"name\" value=\"%s\" maxlength=\"31\" required></label>"
        "<div class=\"toolbar sub\"><button type=\"submit\" class=\"primary\">%s</button></div>"
        "</form>"
        "<p class=\"hint\">%s</p>"
        "</div>",
        i18n_str("general.boardname.title", "Board name"),
        i18n_str("general.boardname.label", "Name"), name,
        i18n_str("general.boardname.button", "Save name"),
        boardname_hint);

    char ssid[33];
    strncpy(ssid, wifi_get_ssid(), sizeof(ssid) - 1);
    ssid[sizeof(ssid) - 1] = '\0';
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<div class=\"card\"><h2>%s</h2>"
        "<div class=\"statusrow\"><span class=\"label\">%s</span><span>%s</span></div>"
        "<form method=\"POST\" action=\"/wifi_set\" class=\"settings-form\">"
        "<label>%s"
        "<select id=\"ssid_select\" name=\"ssid\" data-current=\"%s\" required "
        "onchange=\"wifiSelectChange()\">"
        "<option value=\"\">%s\xe2\x80\xa6</option>"
        "</select></label>"
        "<div id=\"ssid_manual_row\" style=\"display:none\"><label>%s "
        "<input id=\"ssid_manual\" placeholder=\"%s\" autocomplete=\"off\"></label></div>"
        "<p class=\"hint\"><a href=\"#\" onclick=\"wifiRescan();return false;\">%s</a></p>"
        "<noscript><label>%s <input name=\"ssid\" value=\"%s\" required></label></noscript>"
        "<label>%s <input type=\"password\" name=\"password\" "
        "placeholder=\"%s\"></label>"
        "<div class=\"toolbar sub\"><button type=\"submit\" class=\"primary\">%s</button></div>"
        "</form>"
        "<p class=\"hint\">%s</p>"
        "<script>%s</script>"
        "</div>",
        i18n_str("general.wifi.title", "WiFi"),
        i18n_str("general.wifi.current", "Current"),
        ssid[0] ? ssid : i18n_str("status.system.not_configured", "(not configured)"),
        i18n_str("general.wifi.ssid_label", "SSID"), ssid,
        i18n_str("general.wifi.scanning", "Scanning for nearby networks"),
        i18n_str("general.wifi.ssid_label", "SSID"),
        i18n_str("general.wifi.enter_network", "Enter network name"),
        i18n_str("general.wifi.rescan", "Rescan"),
        i18n_str("general.wifi.ssid_label", "SSID"), ssid,
        i18n_str("general.wifi.password_label", "Password"),
        i18n_str("general.wifi.password_placeholder", "leave blank for an open network"),
        i18n_str("general.wifi.button", "Connect"),
        i18n_str("general.wifi.hint",
                 "Saved to flash (NVS); the device reconnects immediately, no reboot needed."),
        WIFI_SCAN_JS);

    /* Timezone folded into this card as a curated dropdown (UI_TIMEZONES)
     * instead of its own separate card with a freeform POSIX TZ string --
     * it exists only to feed this window's start/end interpretation, so
     * it doesn't need to live apart from it. A zone not in the list is
     * still reachable via POST /timezone_set (tz=<any POSIX TZ string>,
     * still a live endpoint) -- same "curated UI, freeform API" split as
     * the language picker. */
    char tz[BOARD_TZ_MAX];
    board_timezone_get(tz, sizeof(tz));
    board_quiet_hours_t qh = board_quiet_hours_get();
    char qh_status[48];
    if (qh.enabled) {
        snprintf(qh_status, sizeof(qh_status), i18n_str("general.quiethours.status_on", "on, %02u:%02u\xe2\x80\x93%02u:%02u"),
                 qh.start_hour, qh.start_min, qh.end_hour, qh.end_min);
    } else {
        snprintf(qh_status, sizeof(qh_status), "%s", i18n_str("general.quiethours.status_off", "off"));
    }
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<div class=\"card\"><h2>%s</h2>"
        "<div class=\"statusrow\"><span class=\"label\">%s</span><span>%s</span></div>"
        "<form method=\"POST\" action=\"/quiet_hours\" class=\"settings-form\">"
        "<label><input type=\"checkbox\" name=\"enabled\" value=\"1\"%s> %s</label>"
        "<label>%s<select name=\"tz\">",
        i18n_str("general.quiethours.title", "Quiet hours"),
        i18n_str("general.current", "Current"), qh_status,
        qh.enabled ? " checked" : "", i18n_str("general.quiethours.enabled", "Enabled"),
        i18n_str("general.quiethours.tz_label", "Timezone"));
    for (size_t i = 0; i < UI_TIMEZONES_N; i++) {
        off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
            "<option value=\"%s\"%s>%s</option>",
            UI_TIMEZONES[i].tz, strcmp(UI_TIMEZONES[i].tz, tz) == 0 ? " selected" : "",
            UI_TIMEZONES[i].label);
    }
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "</select></label>"
        "<label>%s <input type=\"time\" name=\"start\" value=\"%02u:%02u\" required></label>"
        "<label>%s <input type=\"time\" name=\"end\" value=\"%02u:%02u\" required></label>"
        "<div class=\"toolbar sub\"><button type=\"submit\" class=\"primary\">%s</button></div>"
        "</form>"
        "<p class=\"hint\">%s</p></div>",
        i18n_str("general.quiethours.start_label", "Start"), qh.start_hour, qh.start_min,
        i18n_str("general.quiethours.end_label", "End"), qh.end_hour, qh.end_min,
        i18n_str("general.button.save", "Save"),
        i18n_str("general.quiethours.hint",
                 "While enabled, incoming calls on either line during this window go straight "
                 "to the answering machine instead of ringing -- old mechanical ringers are "
                 "loud, and not everyone wants one going off at 2 a.m. Applies even if a line's "
                 "own Answering Machine (Phone Settings tab) is off, since there's nowhere else "
                 "to send the call. Timezone only affects this window -- recordings and "
                 "everything else stay in UTC regardless. Start later than end (e.g. "
                 "21:00-07:00) means the window wraps past midnight. Takes effect on the next "
                 "incoming call -- no reboot."));
    return off;
}

/* Everything about how a call sounds/behaves on the phone itself, one tab:
 * answering machine, off-hook alert, echo cancellation, digit readback
 * (the last one moved from General), speaker volume -- in that order, the
 * three not from General (answering machine, echo cancellation, speaker
 * volume) moved from Hardware, which now covers only physical board
 * identity/stats and firmware/reset -- not line behavior. */
static int append_phone_settings_tab(char *buf, size_t cap, int off)
{
    int num_lines = board_hw_config_get().num_lines;

    /* An answering machine only ever has anything to answer if a call can
     * actually come IN: either a second line to call this one from (no
     * VOIP needed), or VOIP registered so a SIP call can ring it -- a
     * single-line, VOIP-off board has no path for an incoming call at all,
     * so these settings would be saved but never do anything. Not worth
     * hiding the card over (the config is harmless to leave in place for
     * whenever VOIP/a second line gets added), just flagged. */
    bool am_active = voip_is_enabled() || num_lines > 1;
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<div class=\"card\"><h2>%s</h2>", i18n_str("am.title", "Answering machine"));
    if (!am_active) {
        off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
            "<p class=\"hint warn-text\">%s</p>",
            i18n_str("am.inactive_hint",
                     "Nothing can ring in to trigger this yet: VOIP is off (Settings -> VOIP) "
                     "and this board only has one line, so there's no way for a call to reach "
                     "it. Settings below are saved either way -- they'll take effect as soon as "
                     "VOIP is enabled or a second line is configured."));
    }
    for (int l = 0; l < num_lines; l++) {
        board_am_cfg_t am = board_am_get(l);
        char greeting[STORAGE_MAX_NAME];
        telephony_am_greeting_name(greeting, sizeof(greeting), l);
        uint8_t probe;
        size_t got;
        bool have_greeting = (storage_read(greeting, &probe, sizeof(probe), &got) == ESP_OK);

        char line_h3[32], save_line[48];
        snprintf(line_h3, sizeof(line_h3), i18n_str("status.line.title", "Line %d"), l);
        snprintf(save_line, sizeof(save_line), i18n_str("speaker.save_line", "Save line %d"), l);
        off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
            "<h3>%s</h3>"
            "<form method=\"POST\" action=\"/answering_machine\" class=\"settings-form\">"
            "<input type=\"hidden\" name=\"line\" value=\"%d\">"
            "<label><input type=\"checkbox\" name=\"enabled\" value=\"1\"%s> %s</label>"
            "<label>%s "
            "<input type=\"number\" name=\"rings\" value=\"%u\" min=\"%d\" max=\"%d\" required></label>"
            "<label>%s "
            "<input type=\"number\" name=\"max_secs\" value=\"%u\" min=\"%u\" max=\"%u\" required></label>",
            line_h3, l,
            am.enabled ? " checked" : "", i18n_str("general.offhook.enabled", "Enabled"),
            i18n_str("am.rings_label", "Rings before answering"),
            (unsigned)am.rings, BOARD_AM_RINGS_MIN, BOARD_AM_RINGS_MAX,
            i18n_str("am.max_secs_label", "Max message length (seconds)"),
            (unsigned)(am.max_ms / 1000), (unsigned)(BOARD_AM_MAX_MS_MIN / 1000),
            (unsigned)(BOARD_AM_MAX_MS_MAX / 1000));
        if (num_lines > 1) {
            off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
                "<label><input type=\"checkbox\" name=\"other_line\" value=\"1\"%s> "
                "%s</label>",
                am.other_line ? " checked" : "",
                i18n_str("am.other_line_label", "Also answer unanswered calls from other line"));
        }
        off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
            "<div class=\"toolbar sub\"><button type=\"submit\" class=\"primary\">%s</button></div>"
            "</form>",
            save_line);
        if (have_greeting) {
            off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
                "<div class=\"statusrow\"><span class=\"label\">%s</span>"
                "<span><audio controls preload=\"none\" src=\"/media/%s?inline=1\"></audio></span></div>",
                i18n_str("am.greeting", "Greeting"), greeting);
        } else {
            off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
                "<div class=\"statusrow\"><span class=\"label\">%s</span><span>%s</span></div>",
                i18n_str("am.greeting", "Greeting"), i18n_str("am.not_recorded", "not recorded yet"));
        }
    }
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<p class=\"hint\">%s</p></div>"
        "<script>%s</script>",
        i18n_str("am.hint",
                 "Record each line's greeting from that line's own phone: pick up, dial 9-9-9, "
                 "speak after the beep, hang up (or pause ~8s) to save -- overwrites the previous "
                 "greeting directly. Play it back above to check it; re-record any time the same "
                 "way. With no greeting recorded, callers just hear the beep. After the "
                 "configured rings, the line auto-answers, plays the greeting then a beep, then "
                 "records the caller -- capped at the length above or ~8s of silence, whichever "
                 "comes first. Picking up the handset at any point takes the call live instead."),
        RECORDING_RELOAD_JS);

    bool tone_follow_lang = board_tone_follow_language_get();
    char cur_lang[8];
    board_language_get(cur_lang, sizeof(cur_lang));
    char tone_region_hint[420];
    snprintf(tone_region_hint, sizeof(tone_region_hint),
        i18n_str("tones.region.hint",
                 "Checked: dial/ring/busy tones match the language picked in the header up top "
                 "(e.g. selecting Italian also switches to its distinctive dial cadence) -- "
                 "current language is \"%s\". Unchecked: always plays standard US tones (350+450 "
                 "Hz dial, 440+480 Hz ringback, 480+620 Hz busy) no matter what language is "
                 "selected. Takes effect on the next dial tone -- no reboot."),
        cur_lang);
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<div class=\"card\"><h2>%s</h2>"
        "<form method=\"POST\" action=\"/tone_lang_set\" class=\"settings-form\">"
        "<label><input type=\"checkbox\" name=\"enabled\" value=\"1\"%s> %s</label>"
        "<div class=\"toolbar sub\"><button type=\"submit\" class=\"primary\">%s</button></div>"
        "</form>"
        "<p class=\"hint\">%s</p></div>",
        i18n_str("tones.region.title", "Call-progress tones"),
        tone_follow_lang ? " checked" : "",
        i18n_str("tones.region.label", "Use phone tones matching language setting"),
        i18n_str("general.button.save", "Save"),
        tone_region_hint);

    board_offhook_alert_t roh = board_offhook_alert_get();
    char roh_status[48];
    if (roh.enabled) {
        snprintf(roh_status, sizeof(roh_status),
                 i18n_str("general.offhook.status_on", "on, after %u s"), (unsigned)roh.seconds);
    } else {
        snprintf(roh_status, sizeof(roh_status), "%s", i18n_str("general.offhook.status_off", "off"));
    }
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<div class=\"card\"><h2>%s</h2>"
        "<div class=\"statusrow\"><span class=\"label\">%s</span><span>%s</span></div>"
        "<form method=\"POST\" action=\"/offhook_alert\" class=\"settings-form\">"
        "<label><input type=\"checkbox\" name=\"enabled\" value=\"1\"%s> %s</label>"
        "<label>%s "
        "<input type=\"number\" name=\"seconds\" value=\"%u\" min=\"%d\" max=\"%d\" required></label>"
        "<div class=\"toolbar sub\"><button type=\"submit\" class=\"primary\">%s</button></div>"
        "</form>"
        "<p class=\"hint\">%s</p></div>",
        i18n_str("general.offhook.title", "Off-hook alert"),
        i18n_str("general.current", "Current"), roh_status,
        roh.enabled ? " checked" : "", i18n_str("general.offhook.enabled", "Enabled"),
        i18n_str("general.offhook.seconds_label", "Seconds on dial tone before the alert"),
        (unsigned)roh.seconds, BOARD_OFFHOOK_ALERT_MIN_S, BOARD_OFFHOOK_ALERT_MAX_S,
        i18n_str("general.button.save", "Save"),
        i18n_str("general.offhook.hint",
                 "A handset left off the hook sits on dial tone forever, and the line is "
                 "unreachable the whole time (callers get busy) with nothing to draw attention "
                 "to it. This plays the loud, warbling \"receiver off-hook\" alert real phone "
                 "switches use (1400+2060+2450+2600&nbsp;Hz, 100&nbsp;ms on/off, per Telcordia "
                 "GR-506-CORE) once dial tone has gone untouched this long. Hanging up stops "
                 "it. The clock restarts on every return to dial tone, so only a genuinely "
                 "forgotten handset ever reaches it. Takes effect on the next dial tone -- no "
                 "reboot."));

    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<div class=\"card\"><h2>%s</h2>", i18n_str("aec.title", "Echo cancellation"));
    for (int l = 0; l < num_lines; l++) {
        uint32_t ms = board_aec_delay_get(l, AEC_DEFAULT_DELAY_MS);
        bool aec_on = aec_is_enabled(l);
        char line_label[32], delay_label[32];
        snprintf(line_label, sizeof(line_label), i18n_str("status.line.title", "Line %d"), l);
        snprintf(delay_label, sizeof(delay_label), i18n_str("aec.line_delay", "Line %d delay"), l);
        char measure_line[48];
        snprintf(measure_line, sizeof(measure_line), i18n_str("aec.measure_line", "Measure line %d"), l);
        off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
            "<div class=\"statusrow\"><span class=\"label\">%s</span>"
            "<span class=\"pill %s\">%s</span></div>"
            "<div class=\"toolbar sub\">"
            "<form method=\"POST\" action=\"/aec_enable\" style=\"display:inline\">"
            "<input type=\"hidden\" name=\"line\" value=\"%d\">"
            "<input type=\"hidden\" name=\"on\" value=\"1\">"
            "<button type=\"submit\">%s</button></form> "
            "<form method=\"POST\" action=\"/aec_enable\" style=\"display:inline\">"
            "<input type=\"hidden\" name=\"line\" value=\"%d\">"
            "<input type=\"hidden\" name=\"on\" value=\"0\">"
            "<button type=\"submit\">%s</button></form>"
            "</div>"
            "<div class=\"statusrow\"><span class=\"label\">%s</span>"
            "<span>%u ms</span></div>"
            "<div class=\"toolbar sub\">"
            "<button type=\"button\" id=\"aecbtn%d\" onclick=\"aecMeasure(%d)\">%s</button>"
            "</div><div id=\"aecresult%d\" class=\"hint\"></div>",
            line_label, aec_on ? "on" : "off", aec_on ? "ENABLED" : "off",
            l, i18n_str("aec.enable", "Enable"),
            l, i18n_str("aec.disable", "Disable"),
            delay_label, (unsigned)ms, l, l, measure_line, l);
    }
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<p class=\"hint\">%s</p></div>"
        "<script>"
        "var AEC_I18N={measuring:'%s',noEcho:'%s',tries:'%s',delayFmt:'%s',consistent:'%s',"
        "apply:'%s',failed:'%s'};"
        "function spkLive(line,vol){"
        "document.getElementById('spkpct'+line).textContent="
        "Math.round(vol*200/63)+'%%';"
        "fetch('/speaker_volume_live',{method:'POST',"
        "headers:{'Content-Type':'application/x-www-form-urlencoded'},"
        "body:'line='+line+'&vol='+vol});}"
        "function aecMeasure(line){"
        "var btn=document.getElementById('aecbtn'+line);"
        "var out=document.getElementById('aecresult'+line);"
        "btn.disabled=true;out.textContent=AEC_I18N.measuring;"
        "fetch('/aec_measure',{method:'POST',"
        "headers:{'Content-Type':'application/x-www-form-urlencoded'},"
        "body:'line='+line})"
        ".then(function(r){return r.json();})"
        ".then(function(d){btn.disabled=false;"
        "if(!d.ok){out.textContent=AEC_I18N.noEcho+' '+d.hits+'/'+d.attempts+"
        "' '+AEC_I18N.tries;return;}"
        "var ms=Math.round(d.delay_ms);"
        "out.innerHTML=AEC_I18N.delayFmt.replace('%%1',d.delay_ms.toFixed(1))"
        ".replace('%%2',d.tail_ms.toFixed(1)).replace('%%3',d.span_ms.toFixed(0))"
        ".replace('%%4',Math.round(d.taps))+' '+d.hits+'/'+d.attempts+' '+AEC_I18N.consistent+' '+"
        "'<form method=\"POST\" action=\"/aec_delay_apply\" style=\"display:inline\">'+"
        "'<input type=\"hidden\" name=\"line\" value=\"'+line+'\">'+"
        "'<input type=\"hidden\" name=\"ms\" value=\"'+ms+'\">'+"
        "'<button type=\"submit\">'+AEC_I18N.apply.replace('%%1',ms)+'</button></form>';})"
        ".catch(function(){btn.disabled=false;out.textContent=AEC_I18N.failed;});"
        "}"
        "</script>",
        i18n_str("aec.hint",
                 "Enable/Disable is for testing -- leave off for normal use unless a real echo "
                 "has actually been measured and calibrated below (an uncalibrated or "
                 "wrongly-delayed canceller can make echo worse, not better). To calibrate: pick "
                 "up the handset (off-hook) and make sure it's NOT in a call, then click Measure "
                 "-- plays a short burst out the earpiece and listens on the same line's own mic "
                 "to find its local echo delay, the number the canceller needs to align against. "
                 "Takes about 2 seconds (3 quick measurements averaged for reliability). "
                 "Applying saves it and takes effect immediately, no reboot."),
        i18n_str("aec.js.measuring", "Measuring..."),
        i18n_str("aec.js.no_echo", "No echo detected in"),
        i18n_str("aec.js.tries", "tries -- off-hook and not in a call?"),
        i18n_str("aec.js.delay_fmt", "Delay %1ms, tail %2ms, span ~%3ms (%4 taps),"),
        i18n_str("aec.js.consistent", "consistent."),
        i18n_str("aec.js.apply", "Apply %1ms as default"),
        i18n_str("aec.js.failed", "Measurement failed."));

    dialplan_modes_t *m = dialplan_modes();
    off = append_mode_card(buf, cap, off, i18n_str("general.readback.title", "Digit readback"),
        m->readback_digits ? "on" : "off",
        m->readback_digits ? i18n_str("general.readback.on", "On") : i18n_str("general.readback.off", "Off"),
        i18n_str("general.button.turn_off", "Turn off"), "921",
        i18n_str("general.button.turn_on", "Turn on"), "922", -1);

#if CONFIG_BOARD_TYPE_SMD2 || CONFIG_BOARD_TYPE_SMD4 || CONFIG_BOARD_TYPE_SMD5 || CONFIG_BOARD_TYPE_SMD5_CHEAP
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<div class=\"card\"><h2>%s</h2>", i18n_str("speaker.title", "Speaker (earpiece) volume"));
    for (int l = 0; l < num_lines; l++) {
        uint8_t vol = board_speaker_volume_get(l, 0x1E);
        /* Displayed as a percentage (0-200%, 100% = the register's 0dB
         * point) rather than the raw 0-63/dB-step register value -- dB
         * isn't a meaningful unit for most people tuning this by ear. The
         * underlying value submitted to the backend is still the raw 0-63
         * register step; only the label is converted (see spkPct() below),
         * so es8388_set_dac_volume()'s convention doesn't have to change. */
        unsigned pct = (unsigned)((vol * 200u + 31) / 63u);
        char line_label[32];
        snprintf(line_label, sizeof(line_label), i18n_str("status.line.title", "Line %d"), l);
        char save_line[48];
        snprintf(save_line, sizeof(save_line), i18n_str("speaker.save_line", "Save line %d"), l);
        off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
            "<form method=\"POST\" action=\"/speaker_volume\" class=\"settings-form\">"
            "<input type=\"hidden\" name=\"line\" value=\"%d\">"
            "<label>%s <input type=\"range\" name=\"vol\" min=\"0\" max=\"63\" "
            "value=\"%u\" oninput=\"spkLive(%d,this.value)\"> "
            "<span id=\"spkpct%d\">%u%%</span></label>"
            "<div class=\"toolbar sub\"><button type=\"submit\" class=\"primary\">%s</button></div>"
            "</form>",
            l, line_label, (unsigned)vol, l, l, pct, save_line);
    }
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<p class=\"hint\">%s</p></div>",
        i18n_str("speaker.hint",
                 "Drag while on a call to hear the change live (applied immediately, not saved "
                 "yet) -- click Save once you land on a level you like. 100% is the codec's 0dB "
                 "reference point, 200% is max. Same register the bring-up <code>spkamp</code> "
                 "console command tunes."));
#endif
    return off;
}

/* append_file_select() (a <select> of every already-uploaded playable file,
 * for "point this code at an existing file") was removed along with the two
 * forms that used it -- per-row "use existing file" and "add a code for an
 * existing file", both on the Dial Codes tab. Reasoning: the only realistic
 * use for reassigning a code's audio is replacing what it plays, and
 * delete-then-add-with-new-upload already covers that in one obvious step,
 * without a second, rarely-used path to keep in sync (and without the
 * per-row storage_list_internal()+storage_list_bundled() rescan each
 * dropdown used to cost -- see SETTINGS_TABS's comment on why page weight
 * matters here). Recoding an existing entry's number (dialcode_set_code_handler,
 * still below) is unaffected -- that was never file-picker-based. */

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
        "<textarea name=\"text\" maxlength=\"%d\" rows=\"8\" placeholder="
        "\"Leave blank for no spoken prompt\">%s</textarea>"
        "<span class=\"fieldhint\">%s</span>"
        "</label>"
        "<div class=\"toolbar sub\"><button type=\"submit\">Save text</button></div>"
        "</form>",
        ename, PROMPT_TEXT_MAX - 1, etext, status);

    /* A ready-to-paste --sync command for just this one clip (--only), with
     * the real secret filled in -- at the user's request, plaintext rather
     * than the usual masked-bullets treatment (compare the Setup password
     * card's own password field above). That's a deliberate exception, not
     * an oversight: this secret already has no confidentiality left to
     * protect once you can load this settings page at all, since that same
     * card lets that same visitor read/replace it (masked only against
     * shoulder-surfing the input box, not against page access) and reuse it
     * to hit /api/prompt_requests directly. Spelling it out here just saves
     * a trip up the page and a manual copy. */
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
            "above first to get an exact, ready-to-paste command here.</p>");
    }
    return off;
}

/* Moved here (from the VOIP tab) since this secret has nothing to do with
 * placing calls -- it only gates the prompt-generation tooling every card on
 * this tab uses (append_prompt_field()'s ready-to-paste gen_prompt.py
 * command, and GET /api/prompt_requests for off-LAN callers). Living next to
 * the fields it actually affects beats living next to SIP settings it has
 * nothing to do with. */
static int append_setup_password_card(char *buf, size_t cap, int off)
{
    bool setup_pw_set = voip_setup_password()[0] != '\0';
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<div class=\"card wide\"><h2>Setup password</h2>"
        "<p class=\"hint\">Not needed to make or receive calls. This is a shared secret that "
        "lets a host-side script fetch this board's tracked prompt text and push back the "
        "generated audio -- set it once here, then run "
        "<code>firmware/bringup/gen_prompt.py --sync HOST --secret ...</code> from the repo "
        "root (each prompt-text field below shows a ready-to-paste copy of that command once "
        "this is set). It also doubles as the credential for "
        "<code>GET /api/prompt_requests?secret=...</code>, the endpoint that script calls -- "
        "only enforced for callers off this board's own LAN (e.g. over a VPN); same-subnet "
        "callers aren't gated, since anyone on the LAN can already read this page.</p>"
        "<form method=\"POST\" action=\"/voip_setup_password\" class=\"settings-form\">"
        "<label>Setup password"
        "<div class=\"pw-field\">"
        "<input type=\"password\" id=\"setup_pw_input\" name=\"password\" placeholder=\"%s\" "
        "oninput=\"this.nextElementSibling.disabled=!this.value\">"
        "<button type=\"button\" class=\"pw-toggle\" disabled aria-label=\"Show password\" "
        "onclick=\"var i=document.getElementById('setup_pw_input');var show=i.type==='password';"
        "i.type=show?'text':'password';this.classList.toggle('is-visible',show);"
        "this.setAttribute('aria-label',show?'Hide password':'Show password');\">"
        "<svg class=\"icon-eye\" viewBox=\"0 0 24 24\" width=\"18\" height=\"18\" fill=\"none\" "
        "stroke=\"currentColor\" stroke-width=\"2\" stroke-linecap=\"round\" stroke-linejoin=\"round\">"
        "<path d=\"M1 12s4-7 11-7 11 7 11 7-4 7-11 7-11-7-11-7z\"/><circle cx=\"12\" cy=\"12\" r=\"3\"/></svg>"
        "<svg class=\"icon-eye-off\" viewBox=\"0 0 24 24\" width=\"18\" height=\"18\" fill=\"none\" "
        "stroke=\"currentColor\" stroke-width=\"2\" stroke-linecap=\"round\" stroke-linejoin=\"round\">"
        "<path d=\"M17.94 17.94A10.94 10.94 0 0 1 12 19c-7 0-11-7-11-7a18.5 18.5 0 0 1 5.06-5.94M9.9 4.24A9.12 "
        "9.12 0 0 1 12 4c7 0 11 7 11 7a18.5 18.5 0 0 1-2.16 3.19m-6.72-1.07a3 3 0 1 1-4.24-4.24\"/>"
        "<line x1=\"1\" y1=\"1\" x2=\"23\" y2=\"23\"/></svg>"
        "</button>"
        "</div>"
        "<span class=\"fieldhint\">%s</span>"
        "</label>"
        "<div class=\"toolbar sub\"><button type=\"submit\">Save</button></div>"
        "</form></div>",
        setup_pw_set ? "\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2" : "not set",
        setup_pw_set ? "Already set on this board -- leave blank to keep it, or type a new one to replace it."
                     : "Not set yet.");
    return off;
}

static int append_dialcodes_tab(char *buf, size_t cap, int off)
{
    off = append_setup_password_card(buf, cap, off);

    dialplan_clip_entry_t clips[DIALPLAN_MAX_CLIP_ENTRIES];
    size_t nclips = dialplan_list_clip_entries(clips, DIALPLAN_MAX_CLIP_ENTRIES);

    char dialcodes_h2[48];
    snprintf(dialcodes_h2, sizeof(dialcodes_h2), i18n_str("dialcodes.title", "Dial codes (%u)"),
             (unsigned)nclips);
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<div class=\"card\"><h2>%s</h2>"
        "<p class=\"hint\">%s</p>",
        dialcodes_h2,
        i18n_str("dialcodes.hint",
                 "Recode which number plays a clip, edit its description, or remove it. "
                 "\"Built-in\" entries can be recoded but not removed. To change what a code "
                 "plays: remove it below, then add it again with the new audio."));

    if (nclips == 0) {
        off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
            "<div class=\"empty\">%s</div>", i18n_str("dialcodes.none_yet", "none yet"));
    } else {
        /* Three stacked lines per entry, not a <table> row -- this card
         * lives in a .cards grid alongside Operator/Record (narrow, as
         * little as 240px), and a 5-column table has no good way to fit
         * code/filename/play/description/remove side by side at that
         * width without either truncating everything or forcing the table
         * wider than the card (see style.css's .dialcode-item comment for
         * the fixed-layout attempt this replaced). Stacking reads fine at
         * any width instead: recode+builtin-or-remove, then description,
         * then filename+play. */
        off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
            "<div class=\"dialcode-list\">");
        for (size_t i = 0; i < nclips && (size_t)off < cap; i++) {
            char edesc[64 * 6];
            html_escape(clips[i].desc, edesc, sizeof(edesc));
            off += snprintf(buf + off, cap - off,
                "<div class=\"dialcode-item\">"
                "<div class=\"dc-line dc-line-code\">"
                "<form method=\"POST\" action=\"/dialcode_set_code\" class=\"dc-inline\">"
                "<input type=\"hidden\" name=\"old_code\" value=\"%s\">"
                "<input name=\"new_code\" value=\"%s\" pattern=\"[0-9]{2,6}\" maxlength=\"6\" "
                "size=\"6\" required>"
                "<button type=\"submit\">%s</button></form>",
                clips[i].code, clips[i].code, i18n_str("dialcodes.save", "save"));
            if (clips[i].builtin) {
                off += snprintf(buf + off, cap - off,
                    "<span class=\"pill off\">%s</span>", i18n_str("dialcodes.builtin", "built-in"));
            } else {
                char remove_confirm[64];
                snprintf(remove_confirm, sizeof(remove_confirm),
                         i18n_str("dialcodes.remove_confirm", "Remove dial code %s?"), clips[i].code);
                off += snprintf(buf + off, cap - off,
                    "<form method=\"POST\" action=\"/dialcode_delete/%s\" "
                    "onsubmit=\"return confirm('%s')\">"
                    "<button type=\"submit\">%s</button></form>",
                    clips[i].code, remove_confirm, i18n_str("dialcodes.remove", "remove"));
            }
            off += snprintf(buf + off, cap - off,
                "</div>"
                "<div class=\"dc-line\">"
                "<form method=\"POST\" action=\"/dialcode_set_desc\" class=\"dc-inline dc-grow\">"
                "<input type=\"hidden\" name=\"code\" value=\"%s\">"
                "<input name=\"desc\" value=\"%s\" maxlength=\"63\" class=\"dc-desc-input\">"
                "<button type=\"submit\">%s</button></form>"
                "</div>"
                "<div class=\"dc-line\"><code class=\"dc-filename\">%s</code></div>"
                "<div class=\"dc-line\">"
                "<audio controls preload=\"none\" src=\"/media/%s?inline=1\"></audio>"
                "</div></div>",
                clips[i].code, edesc, i18n_str("dialcodes.save", "save"),
                clips[i].filename, clips[i].filename);
        }
        off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0, "</div>" /* .dialcode-list */);
    }

    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<form method=\"POST\" action=\"/dialcode_upload\" enctype=\"multipart/form-data\" class=\"settings-form\">"
        "<h3>%s</h3>"
        "<div class=\"grid2\">"
        "<label>%s <input name=\"new_code\" pattern=\"[0-9]{2,6}\" "
        "maxlength=\"6\" required></label>"
        "<label>%s <input name=\"desc\" maxlength=\"63\"></label></div>"
        "<label>%s <input type=\"file\" name=\"file\" accept=\"audio/wav\" required></label>"
        "<div class=\"toolbar sub\"><button type=\"submit\" class=\"primary\">%s</button></div>"
        "</form></div>",
        i18n_str("dialcodes.add_title", "Add a code"),
        i18n_str("dialcodes.new_code_label", "New code (2-6 digits)"),
        i18n_str("dialcodes.desc_label", "Description (optional)"),
        i18n_str("dialcodes.audio_label", "Audio file (WAV)"),
        i18n_str("dialcodes.upload_button", "Upload + add dial code"));
    return off;
}

static int append_operator_tab(char *buf, size_t cap, int off,
                                dialplan_entry_info_t *entries, size_t n)
{
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<div class=\"card\"><h2>%s</h2>", i18n_str("operator.title", "Operator help"));
    for (size_t i = 0; i < n; i++) {
        if (strcmp(entries[i].code, "0") != 0) continue;
        off = append_guide_row(buf, cap, off, entries[i].code, entries[i].desc,
                                entries[i].implemented);
    }
    off = append_prompt_field(buf, cap, off, "operator.wav",
        i18n_str("operator.default_prompt",
                 "You've reached the operator line. To leave a message, dial 2-1-1. "
                 "For other features, please refer to your phone's guide."));
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<p class=\"hint\">%s</p>"
        "<form method=\"POST\" action=\"/operator_upload\" enctype=\"multipart/form-data\" "
        "class=\"settings-form\">"
        "<label>%s <input type=\"file\" name=\"file\" accept=\"audio/wav\" required></label>"
        "<div class=\"toolbar sub\"><button type=\"submit\" class=\"primary\">%s</button></div>"
        "</form></div>",
        i18n_str("operator.upload_hint",
                 "Or upload audio directly below (converted automatically to 8kHz mono) -- "
                 "either way replaces what plays."),
        i18n_str("operator.audio_label", "Replacement audio (WAV)"),
        i18n_str("operator.upload_button", "Upload"));
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
    char name[32];
    board_name_get(name, sizeof(name));

    bool r0 = voip_is_registered(0);
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<div class=\"card wide\"><h2>%s</h2>"
        "<div class=\"statusrow\"><span class=\"label\">%s</span><span>%s</span></div>"
        "<div class=\"statusrow\"><span class=\"label\">%s</span>"
        "<span class=\"pill %s\">%s</span></div>",
        i18n_str("settings.tab.voip", "VOIP"),
        i18n_str("voip.sip_server", "SIP server"),
        voip_server()[0] ? voip_server() : i18n_str("voip.not_set", "(not set)"),
        i18n_str("voip.line1", "Line 1"),
        r0 ? "on" : "off", r0 ? i18n_str("voip.registered", "registered") : i18n_str("voip.not_registered", "not registered"));
    /* Shown for a genuine 2-line board, but also whenever a line-2 account
     * is still configured on a 1-line board -- e.g. a board reconfigured
     * down from 2 lines to 1, or a stale account from before -- so there's
     * still a status pill (and, below, form fields) to clear it by. */
    if (board_hw_config_get().num_lines > 1 || voip_user(1)[0]) {
        bool r1 = voip_is_registered(1);
        off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
            "<div class=\"statusrow\"><span class=\"label\">%s</span>"
            "<span class=\"pill %s\">%s</span></div>",
            i18n_str("voip.line2", "Line 2"),
            r1 ? "on" : "off", r1 ? i18n_str("voip.registered", "registered") : i18n_str("voip.not_registered", "not registered"));
    }
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0, "</div>");

    /* Master "enable VOIP" switch -- gates sip_task() itself (voip.c), not
     * just this tab's own visibility. The status card above always shows
     * (so "disabled" is visible at a glance even collapsed); everything
     * below -- the actual config form, setup instructions, check-a-number
     * -- lives inside #voip-extra, hidden client-side when unchecked
     * (FEATURE_TOGGLE_JS) so toggling it is instant, no round trip. Saved
     * values persist in NVS regardless of this switch's state -- each
     * section below is its own <form>, so hiding it doesn't stop it from
     * submitting normally when visible. */
    bool voip_on = voip_is_enabled();
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<div class=\"card wide\">"
        "<form method=\"POST\" action=\"/voip_enable\" class=\"settings-form\">"
        "<label><input type=\"checkbox\" name=\"enabled\" value=\"1\" "
        "onchange=\"toggleSection(this,'voip-extra')\"%s> <b>Enable VOIP</b></label>"
        "<div class=\"toolbar sub\"><button type=\"submit\" class=\"primary\">Save</button></div>"
        "</form>"
        "<p class=\"hint\">Takes effect on reboot, like the server/extension settings below. "
        "Turning this off doesn't erase anything you've saved -- it just stops the board from "
        "registering with your SIP server until you turn it back on.</p>"
        "</div>"
        "<div id=\"voip-extra\"%s>",
        voip_on ? " checked" : "", voip_on ? "" : " hidden");

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
        "<label>SIP server (host or IP) <input name=\"server\" id=\"voip_server_input\" "
        "oninput=\"voipSyncCheckServer()\" value=\"%s\"></label>"
        "<label>Line 1 extension <input name=\"user0\" value=\"%s\"></label>"
        "<label>Line 1 password <input type=\"password\" name=\"pass0\" "
        "placeholder=\"leave blank to keep\"></label>",
        voip_server(), voip_user(0));
    /* Same "or a stale account exists" condition as the status pill above
     * -- otherwise a 1-line board with a leftover line-2 config (e.g.
     * reconfigured down from 2 lines) has no way to ever clear it: the
     * fields to submit blank would themselves be hidden. */
    if (board_hw_config_get().num_lines > 1 || voip_user(1)[0]) {
        off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
            "<label>Line 2 extension <input name=\"user1\" value=\"%s\"></label>"
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
    /* Same condition as the Line 2 form field above: a genuine 2-line board,
     * or a stale line-2 account still configured on a 1-line board. Doesn't
     * itself require voip_user(1) to be non-empty -- when it's empty, the
     * block below fills in placeholder digits rather than being skipped, so
     * the example still shows what a 2-line board's second block looks
     * like. */
    bool have_line1 = board_hw_config_get().num_lines > 1 || voip_user(1)[0];
    /* Falls back to placeholder digits if nothing's saved yet, rather than
     * rendering a broken example with an empty [] section name -- but the
     * whole point of this block is to need zero substitution once a real
     * extension IS saved, so say so either way. */
    bool using_placeholders = !voip_user(0)[0];
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "</ol>"
        "<p class=\"hint\">Prefer to add the line by hand instead of the downloadable script? "
        "Pick whichever matches your server -- both end up at the same config, just a different "
        "path to it.</p>"
        "<details><summary>Asterisk (plain -- edit config files directly)</summary><ol>"
        "<li>Pick a free extension -- \"Check a number\" below, or see "
        "<code>archive/VOIP_SETUP.md</code> for this project's shared numbering registry.</li>"
        "<li>SSH into the Asterisk server.</li>"
        "<li>Add an AOR + auth + endpoint trio to <code>/etc/asterisk/pjsip.conf</code> for this "
        "extension number -- or just download the setup script above and read it, it's a plain "
        "bash script that writes exactly this. %s Includes a <code>callerid</code> line so "
        "<code>pjsip show endpoints</code>/<code>show contacts</code> shows which board this is, "
        "rather than a bare interchangeable number (the extension itself stays plain digits "
        "either way -- that's the part a rotary/DTMF phone actually has to dial), and "
        "<code>rewrite_contact</code>/<code>rtp_symmetric</code> so this still works if the board "
        "ends up at a different site than the server (see \"Setting this up remotely?\" below):</li>"
        "</ol>",
        using_placeholders
            ? "No extension saved yet on this board, so this is a placeholder example (extension "
              "1234/1235) -- set and Save a real one in the fields above first, then reload this "
              "page for a paste-ready block using it, no substitution needed."
            : "Using this board's own currently-saved settings, so there's nothing left to "
              "substitute -- if you just changed the fields above, Save first and reload this "
              "page so this block picks up the new values.");

    /* Copy-paste example for the manual path, built from this board's
     * actual saved settings when there are any (else the placeholder
     * digits above) -- either way nothing generic to mentally substitute,
     * matching what "just copy and paste into the file" actually needs.
     * Escaped: this text lands inside <pre><code>, a text-node context
     * where an unescaped '<'/'&' in a board name or password isn't just
     * cosmetically wrong the way it is in a form field's value=""
     * attribute elsewhere on this page -- unlike those, this could break
     * the block's own markup or, with '<', inject something that isn't
     * inert here. */
    {
        const char *ext0 = voip_user(0)[0] ? voip_user(0) : "1234";
        const char *rawpass0 = voip_user(0)[0] ? voip_pass(0) : "your-line1-password";
        char ename[192], euser0[192], epass0[288];
        html_escape(name, ename, sizeof(ename));
        html_escape(ext0, euser0, sizeof(euser0));
        html_escape(rawpass0, epass0, sizeof(epass0));
        off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
            "<pre><code>[%s]\n"
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
            "rewrite_contact=yes\n"
            "rtp_symmetric=yes\n"
            "callerid=\"%s Line 0\" &lt;%s&gt;\n",
            euser0, euser0, euser0, epass0, euser0, euser0, euser0, ename, euser0);
        if (have_line1) {
            const char *ext1 = voip_user(1)[0] ? voip_user(1) : "1235";
            const char *rawpass1 = voip_user(1)[0] ? voip_pass(1) : "your-line2-password";
            char euser1[192], epass1[288];
            html_escape(ext1, euser1, sizeof(euser1));
            html_escape(rawpass1, epass1, sizeof(epass1));
            off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
                "\n[%s]\n"
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
                "rewrite_contact=yes\n"
                "rtp_symmetric=yes\n"
                "callerid=\"%s Line 1\" &lt;%s&gt;\n",
                euser1, euser1, euser1, epass1, euser1, euser1, euser1, ename, euser1);
        }
        off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0, "</code></pre>");
    }

    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<ol start=\"4\">"
        "<li>Add a matching <code>exten =&gt; &lt;number&gt;,1,Dial(PJSIP/&lt;number&gt;,20)</code> "
        "/ <code>same =&gt; n,Hangup()</code> entry to a <code>[rotary]</code> context in "
        "<code>extensions.conf</code> (create the context if it doesn't exist yet) -- again, "
        "using this board's actual extension(s):</li>"
        "</ol>");
    {
        const char *ext0 = voip_user(0)[0] ? voip_user(0) : "1234";
        char euser0[192];
        html_escape(ext0, euser0, sizeof(euser0));
        off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
            "<pre><code>[rotary]\n"
            "exten =&gt; %s,1,Dial(PJSIP/%s,30)\n"
            " same =&gt; n,Hangup()\n",
            euser0, euser0);
        if (have_line1) {
            const char *ext1 = voip_user(1)[0] ? voip_user(1) : "1235";
            char euser1[192];
            html_escape(ext1, euser1, sizeof(euser1));
            off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
                "exten =&gt; %s,1,Dial(PJSIP/%s,30)\n"
                " same =&gt; n,Hangup()\n",
                euser1, euser1);
        }
        off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0, "</code></pre>");
    }

    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<ol start=\"5\">"
        "<li><code>asterisk -rx \"pjsip reload\"</code> and "
        "<code>asterisk -rx \"dialplan reload\"</code>.</li>"
        "<li>Back here: same server/extension/password into the Line fields above, Save, reboot "
        "the phone, confirm \"registered\" above.</li>"
        "</ol></details>"

        "<details><summary>FreePBX (GUI)</summary>"
        "<p class=\"hint\">FreePBX manages its own copy of <code>pjsip.conf</code>/"
        "<code>extensions.conf</code> from its database and can overwrite hand edits on "
        "\"Apply Config\" -- use its GUI for this instead of the Asterisk steps above, don't mix "
        "the two for the same extension. Menu names/tabs shift a little between versions; look "
        "for these fields if something's not exactly where described.</p><ol>"
        "<li><b>Applications &rarr; Extensions &rarr; + Add Extension &rarr; Add New PJSIP "
        "Extension</b>.</li>"
        "<li><b>User Extension</b>: the plain-digit extension number (same one you'd put in the "
        "Line fields above -- \"Check a number\" below first).</li>"
        "<li><b>Display Name</b>: something that identifies this board, e.g. \"%s Line 0\" -- "
        "same purpose as the <code>callerid</code> line in the Asterisk path: so this extension "
        "doesn't look like an anonymous number if you're running more than one of these boards.</li>"
        "<li><b>Secret</b> (General tab): the line's password.</li>"
        "<li>If this board is at a different site than the FreePBX server: on the extension's "
        "<b>Advanced</b> tab, look for a NAT-related setting (varies by version -- \"NAT Mode\", "
        "sometimes just implied by <code>rewrite_contact</code>/<code>rtp_symmetric</code> "
        "already being on by default) and confirm it's enabled. Same reasoning as the Asterisk "
        "path's note on this -- see \"Setting this up remotely?\" below.</li>"
        "<li>Repeat for line 2 if this is a 2-line board.</li>"
        "<li>Click <b>Submit</b>, then the orange <b>Apply Config</b> bar at the top.</li>"
        "<li>Back here: same server/extension/password into the Line fields above, Save, reboot "
        "the phone, confirm \"registered\" above.</li>"
        "</ol></details>"

        "<details><summary>Setting this up remotely (board and server on different networks)?"
        "</summary>"
        "<p class=\"hint\">Both sides need their firewall/router configured, or registration "
        "and/or audio will fail even though everything above is correct:</p>"
        "<ul>"
        "<li><b>This board's side:</b> forward UDP <code>5060</code> (SIP) and UDP "
        "<code>4000-4002</code> (RTP -- line 1 uses 4000, line 2 uses 4002) from your router's "
        "WAN interface to this board's LAN IP.</li>"
        "<li><b>The server's side</b> (only if the Asterisk/FreePBX box is <em>also</em> behind a "
        "home-style router, not a cloud/VPS box with its own public IP): forward UDP "
        "<code>5060</code> plus its configured RTP range -- Asterisk's default is UDP "
        "<code>10000-20000</code> (<code>rtp.conf</code>; FreePBX: Settings &rarr; Asterisk SIP "
        "Settings &rarr; RTP tab), though many installs narrow that range down.</li>"
        "</ul>"
        "<p class=\"hint\">Port forwarding alone isn't quite enough, though: this board has no "
        "STUN/ICE, so it advertises its own <em>private</em> LAN IP in its SIP/SDP messages -- "
        "useless to a server on another network for sending audio back. "
        "<code>rewrite_contact=yes</code>/<code>rtp_symmetric=yes</code> (Asterisk) or the "
        "equivalent NAT setting (FreePBX) tells the server to ignore that and learn this board's "
        "real address from where its packets actually arrive from instead -- already included in "
        "the setup script and the copy-paste blocks above, just something to also set if "
        "configuring by GUI. This is the standard way any NAT'd device without STUN registers "
        "with a remote PBX, and works fine through an ordinary home router; a particularly strict "
        "\"symmetric NAT\" or carrier-grade NAT on either end can still defeat it, in which case a "
        "VPN (e.g. WireGuard/Tailscale) between the two sites is the more reliable fix.</p>"
        "<p class=\"hint\">Also worth knowing: this project's SIP/RTP has no TLS/SRTP, so a call "
        "placed over the open internet this way isn't encrypted end to end -- fine on a trusted "
        "network, worth keeping in mind on one you don't fully trust.</p>"
        "</details>"
        "</div>",
        name);

    /* 2. Check-before-you-commit: own number field, not tied to the Save
     * form's fields for user0/pass0/etc -- so you can try a few candidates
     * without touching your actual saved line config. Server-rendered
     * redirect-with-result, same no-JS pattern as the rest of this page.
     *
     * The server IS shared with the Save form above, deliberately -- but as
     * a hidden field kept in sync via voipSyncCheckServer() (called on
     * every keystroke in the Save form's server input, and once on load),
     * not the saved voip_server() value. Used to be the latter, which meant
     * typing a new server here to try before committing to it required
     * saving first just to check it -- exactly backwards for a "try before
     * you commit" tool. A visible second text field was tried before that
     * and reverted (read as two unrelated server settings with different
     * wording); mirroring the one you're already typing into avoids both
     * problems. voip_check_handler() still falls back to the saved
     * voip_server() if this arrives empty (no-JS). */
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<div class=\"card wide\"><h2>2. Check a number</h2>"
        "<p class=\"hint\">See <code>archive/VOIP_SETUP.md</code> for this project's shared numbering "
        "registry, then confirm a candidate is actually free here before you commit to it.</p>"
        "<form method=\"POST\" action=\"/voip_check\" class=\"settings-form\">"
        "<input type=\"hidden\" name=\"server\" id=\"voip_check_server\" value=\"%s\">"
        "<label>Number to check <input name=\"number\" placeholder=\"e.g. 1101\" "
        "pattern=\"[0-9]{2,6}\" required></label>"
        "<div class=\"toolbar sub\"><button type=\"submit\">Check availability</button></div>"
        "</form>",
        voip_server());
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
        "<p class=\"hint\">Sends a SIP OPTIONS to that number on "
        "<span id=\"voip_check_server_label\">%s</span> (whatever's in the SIP server field "
        "above right now -- no need to Save first). Only a clean "
        "404 Not Found counts as \"free\"; that's the one answer this can trust. On stock "
        "Asterisk/PJSIP (<code>identify_by=username</code>), an unrecognized number usually gets "
        "challenged for auth (401) exactly like a real endpoint with the wrong password would -- "
        "so \"in use, or not confirmable\" often just means Asterisk couldn't be bothered to say "
        "no, not that the number is actually taken. Treat a non-free result as inconclusive, and "
        "confirm on the server itself: "
        "<code>asterisk -rx \"pjsip show endpoint &lt;number&gt;\"</code>.</p>"
        "<script>"
        "function voipSyncCheckServer(){"
        "var v=document.getElementById('voip_server_input').value;"
        "document.getElementById('voip_check_server').value=v;"
        "document.getElementById('voip_check_server_label').textContent=v||'(no SIP server saved yet)';"
        "}"
        "voipSyncCheckServer();"
        "</script></div>"
        "</div>", /* closes #voip-extra, opened above the master enable switch */
        voip_server()[0] ? voip_server() : "(no SIP server saved yet)");

    return off;
}

/* Bluetooth Classic audio companion (BM64, or a second ESP32 running
 * Bluedroid) -- see archive/BLUETOOTH_AUDIO_UPGRADE.md and bt_companion.h.
 * No hardware exists on any board yet, so the master switch defaults off;
 * this tab exists now so there's a real, working place to flip it on and
 * see honest status once a daughter board is actually wired in, rather
 * than adding the UI at the same time as the hardware. Same master-switch
 * pattern as the VOIP tab above: only the enable checkbox shows when off,
 * everything else collapses into #bt-extra. */
/* "Module configuration" card: a stock BM64 ships routing audio to I2S for
 * the EVB's external DAC, so the analog pin this board taps stays silent
 * until a few EEPROM bytes are changed (BM64/BM64_AUDIO_DEBUG.md has the
 * full derivation). The check is keyed on the module's own BD address rather
 * than a plain "already done" flag, so swapping the BM64 or moving this
 * ESP32 to another board re-prompts instead of trusting a stale marker.
 * Deliberately a button, not something done on every boot: it costs two
 * module resets and ~15s of Bluetooth downtime. */
static int append_bt_config_card(char *buf, size_t cap, int off,
                                 const bt_companion_status_t *st)
{
    bm64_config_report_t rep;
    bool have_report = bt_companion_config_last_report(&rep);
    bool verified    = bt_companion_config_is_verified();

    const char *pill_cls  = verified ? "on" : "warn";
    const char *pill_text = verified
        ? i18n_str("bt.cfg_verified", "verified")
        : (st->bd_addr[0] ? i18n_str("bt.cfg_unverified", "not checked yet")
                          : i18n_str("bt.cfg_unknown", "unknown"));

    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<div class=\"card wide\"><h2>%s</h2>"
        "<div class=\"statusrow\"><span class=\"label\">%s</span>"
        "<span class=\"pill %s\">%s</span></div>"
        "<p class=\"hint\">%s</p>",
        i18n_str("bt.cfg_title", "Module configuration"),
        i18n_str("bt.cfg_label", "Audio output settings"),
        pill_cls, pill_text,
        i18n_str("bt.cfg_hint",
                 "A factory BM64 sends audio out over I2S to an external DAC, which this board "
                 "doesn't use -- it taps the module's analog output instead, so audio stays "
                 "silent until a few EEPROM bytes are corrected. This checks them and fixes any "
                 "that are wrong. Bluetooth drops for about 15 seconds and a paired phone will "
                 "need to reconnect."));

    if (have_report) {
        off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
            "<table class=\"filetable\"><thead><tr><th>%s</th><th>%s</th><th>%s</th>"
            "<th>%s</th><th>%s</th></tr></thead><tbody>",
            i18n_str("bt.cfg_col_addr", "Address"),
            i18n_str("bt.cfg_col_found", "Found"),
            i18n_str("bt.cfg_col_want", "Wanted"),
            i18n_str("bt.cfg_col_state", "State"),
            i18n_str("bt.cfg_col_what", "What it controls"));
        for (int i = 0; i < rep.n_items && i < BM64_CONFIG_ITEM_MAX; i++) {
            const bm64_config_item_t *it = &rep.items[i];
            const char *state = it->written
                ? i18n_str("bt.cfg_state_written", "fixed")
                : (it->found == it->expected ? i18n_str("bt.cfg_state_ok", "ok")
                                             : i18n_str("bt.cfg_state_bad", "wrong"));
            char what[160];
            html_escape(it->what ? it->what : "", what, sizeof(what));
            off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
                "<tr><td>0x%04X</td><td>%02X</td><td>%02X</td><td>%s</td><td>%s</td></tr>",
                it->addr, it->found, it->expected, state, what);
        }
        off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0, "</tbody></table>");

        if (rep.error[0]) {
            char esc[128];
            html_escape(rep.error, esc, sizeof(esc));
            off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
                "<p class=\"hint err\">%s</p>", esc);
        } else {
            off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
                "<p class=\"hint\">%s</p>",
                rep.ok ? i18n_str("bt.cfg_all_ok", "All bytes correct.")
                       : i18n_str("bt.cfg_still_wrong", "Some bytes are still wrong."));
        }
    }

    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<form method=\"POST\" action=\"/bt_config_check\" class=\"settings-form\">"
        "<div class=\"toolbar sub\"><button type=\"submit\"%s>%s</button></div>"
        "</form>"
        "</div>",
        st->link_up ? "" : " disabled",
        i18n_str("bt.cfg_button", "Check and repair module configuration"));
    return off;
}

static int append_bluetooth_tab(char *buf, size_t cap, int off)
{
    bool bt_on = bt_companion_enabled();
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<div class=\"card wide\"><h2>%s</h2>"
        "<form method=\"POST\" action=\"/bt_enable_set\" class=\"settings-form\">"
        "<label><input type=\"checkbox\" name=\"enabled\" value=\"1\" "
        "onchange=\"toggleSection(this,'bt-extra')\"%s> <b>%s</b></label>"
        "<div class=\"toolbar sub\"><button type=\"submit\" class=\"primary\">%s</button></div>"
        "</form>"
        "<p class=\"hint\">%s</p>"
        "</div>"
        "<div id=\"bt-extra\"%s>",
        i18n_str("settings.tab.bluetooth", "Bluetooth"),
        bt_on ? " checked" : "", i18n_str("bt.enable_label", "I have a Bluetooth companion board wired in"),
        i18n_str("general.button.save", "Save"),
        i18n_str("bt.enable_hint",
                 "Turn this on once a Bluetooth Classic audio companion board (see "
                 "<code>archive/BLUETOOTH_AUDIO_UPGRADE.md</code> in the repo) is actually wired "
                 "to this board's GPIO16/17 UART header -- leave it off until then. Unlike VOIP's "
                 "switch, this takes effect immediately, no reboot: it just starts or stops "
                 "talking over that UART."),
        bt_on ? "" : " hidden");

    bt_companion_status_t st = bt_companion_get_status();
    const char *link_pill = st.link_up ? "on" : "off";
    const char *link_text = st.link_up
        ? i18n_str("bt.link_up", "responding")
        : i18n_str("bt.link_down", "not responding");
    const char *bt_pill = st.bt_connected ? "on" : "off";
    const char *bt_conn_text = st.bt_connected ? i18n_str("bt.connected", "connected")
                                                : i18n_str("bt.not_connected", "not connected");
    char paired[BT_COMPANION_NAME_MAX * 6];
    html_escape(st.bt_connected ? st.paired_name : i18n_str("bt.none_paired", "(none)"),
                paired, sizeof(paired));
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<div class=\"card wide\"><h2>%s</h2>"
        "<div class=\"statusrow\"><span class=\"label\">%s</span>"
        "<span id=\"bt-link-pill\" class=\"pill %s\">%s</span></div>"
        "<div class=\"statusrow\"%s id=\"bt-addr-row\"><span class=\"label\">%s</span>"
        "<span id=\"bt-addr\">%s</span></div>"
        "<div class=\"statusrow\"%s id=\"bt-paired-row\"><span class=\"label\">%s</span>"
        "<span id=\"bt-paired-pill\" class=\"pill %s\">%s</span></div>"
        "<div class=\"statusrow\"%s id=\"bt-paired-name-row\"><span class=\"label\">%s</span>"
        "<span id=\"bt-paired-name\">%s</span></div>",
        i18n_str("bt.status_title", "Status"),
        i18n_str("bt.link_label", "Companion board"), link_pill, link_text,
        (st.link_up && st.bd_addr[0]) ? "" : " hidden",
        i18n_str("bt.own_addr_label", "This module's Bluetooth address"), st.bd_addr,
        st.link_up ? "" : " hidden",
        i18n_str("bt.paired_phone_label", "Paired phone/laptop"), bt_pill, bt_conn_text,
        st.link_up ? "" : " hidden",
        i18n_str("bt.paired_name_label", "Name"), paired);

    /* Live status: polls the shared /api/status JSON (already carries
     * bt_link_up/bt_connected/bt_paired_name, see build_status_json())
     * instead of a dedicated endpoint. The pairing button gets its own
     * transient "Pairing..." / "Paired!" states so clicking it actually
     * shows something happened -- previously it just re-rendered the same
     * static page on submit, with no feedback while the phone/laptop side
     * hadn't paired yet. */
    char js_link_up[48], js_link_down[48], js_connected[48], js_not_connected[48],
         js_none_paired[48], js_pair_btn[64], js_pairing[64], js_paired[48];
    js_escape(i18n_str("bt.link_up", "responding"), js_link_up, sizeof(js_link_up));
    js_escape(i18n_str("bt.link_down", "not responding"), js_link_down, sizeof(js_link_down));
    js_escape(i18n_str("bt.connected", "connected"), js_connected, sizeof(js_connected));
    js_escape(i18n_str("bt.not_connected", "not connected"), js_not_connected, sizeof(js_not_connected));
    js_escape(i18n_str("bt.none_paired", "(none)"), js_none_paired, sizeof(js_none_paired));
    js_escape(i18n_str("bt.pair_button", "Start pairing"), js_pair_btn, sizeof(js_pair_btn));
    js_escape(i18n_str("bt.pair_button_pairing", "Pairing... check your phone/laptop"),
              js_pairing, sizeof(js_pairing));
    js_escape(i18n_str("bt.pair_button_paired", "Paired!"), js_paired, sizeof(js_paired));

    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<form method=\"POST\" action=\"/bt_pair\" class=\"settings-form\" id=\"bt-pair-form\">"
        "<div class=\"toolbar sub\"><button type=\"submit\" id=\"bt-pair-btn\"%s>%s</button></div>"
        "</form>"
        "<p class=\"hint\" id=\"bt-pair-hint\">%s</p>"
        "</div>"
        "<script>(function(){"
        "var I={"
        "linkUp:'%s',linkDown:'%s',connected:'%s',notConnected:'%s',nonePaired:'%s',"
        "pairBtn:'%s',pairing:'%s',paired:'%s'"
        "};"
        "var linkPill=document.getElementById('bt-link-pill');"
        "var addrRow=document.getElementById('bt-addr-row');"
        "var addrEl=document.getElementById('bt-addr');"
        "var pairedRow=document.getElementById('bt-paired-row');"
        "var pairedPill=document.getElementById('bt-paired-pill');"
        "var nameRow=document.getElementById('bt-paired-name-row');"
        "var nameEl=document.getElementById('bt-paired-name');"
        "var btn=document.getElementById('bt-pair-btn');"
        "var form=document.getElementById('bt-pair-form');"
        "var pairing=false, pairingTimeout=null;"
        "function setBtnState(state){"
        "btn.classList.remove('is-busy','is-active');"
        "if(state==='pairing'){btn.disabled=true;btn.classList.add('is-busy');btn.textContent=I.pairing;}"
        "else if(state==='paired'){btn.disabled=false;btn.classList.add('is-active');btn.textContent=I.paired;}"
        "else{btn.disabled=(linkPill.className.indexOf('on')<0);btn.textContent=I.pairBtn;}"
        "}"
        "function poll(){"
        "fetch('/api/status').then(function(r){return r.json();}).then(function(d){"
        "var up=!!d.bt_link_up, conn=!!d.bt_connected;"
        "linkPill.className='pill '+(up?'on':'off');linkPill.textContent=up?I.linkUp:I.linkDown;"
        "addrRow.hidden=!(up&&d.bt_addr); if(d.bt_addr)addrEl.textContent=d.bt_addr;"
        "pairedRow.hidden=!up; nameRow.hidden=!up;"
        "pairedPill.className='pill '+(conn?'on':'off');pairedPill.textContent=conn?I.connected:I.notConnected;"
        "nameEl.textContent=conn?(d.bt_paired_name||''):I.nonePaired;"
        "if(!up){btn.disabled=true;}"
        "if(pairing&&conn){"
        "pairing=false; if(pairingTimeout)clearTimeout(pairingTimeout);"
        "setBtnState('paired');"
        "setTimeout(function(){if(!pairing)setBtnState('idle');},4000);"
        "} else if(!pairing){"
        "setBtnState('idle');"
        "}"
        "}).catch(function(){});"
        "}"
        "form.addEventListener('submit',function(ev){"
        "ev.preventDefault();"
        "pairing=true; setBtnState('pairing');"
        "fetch('/bt_pair',{method:'POST'}).catch(function(){});"
        "if(pairingTimeout)clearTimeout(pairingTimeout);"
        "pairingTimeout=setTimeout(function(){pairing=false;setBtnState('idle');},45000);"
        "});"
        "setInterval(poll,3000);"
        "})();</script>",
        st.link_up ? "" : " disabled",
        i18n_str("bt.pair_button", "Start pairing"),
        st.link_up
            ? i18n_str("bt.pair_hint", "Puts the companion board into pairing mode -- on your "
                                        "phone/laptop, look for it in the Bluetooth device list "
                                        "the same way you'd pair any headset.")
            : i18n_str("bt.pair_hint_down",
                       "Greyed out because the companion board isn't responding yet -- check the "
                       "UART wiring (GPIO16/17, crossed TX/RX) once it's connected."),
        js_link_up, js_link_down, js_connected, js_not_connected, js_none_paired,
        js_pair_btn, js_pairing, js_paired);

    off = append_bt_config_card(buf, cap, off, &st);

    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0, "</div>"); /* closes #bt-extra */
    return off;
}

static int append_record_tab(char *buf, size_t cap, int off,
                              dialplan_entry_info_t *entries, size_t n)
{
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<div class=\"card\"><h2>%s</h2>", i18n_str("record.title", "Record message"));
    for (size_t i = 0; i < n; i++) {
        if (strcmp(entries[i].code, "211") != 0) continue;
        off = append_guide_row(buf, cap, off, entries[i].code, entries[i].desc,
                                entries[i].implemented);
    }
    uint32_t max_ms = board_record_max_ms_get(120000);
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<p class=\"hint\">%s</p>"
        "<form method=\"POST\" action=\"/record_max_ms\" class=\"settings-form\">"
        "<label>%s "
        "<input type=\"number\" name=\"seconds\" value=\"%u\" min=\"%u\" max=\"%u\" required></label>"
        "<div class=\"toolbar sub\"><button type=\"submit\" class=\"primary\">%s</button></div>"
        "</form>",
        i18n_str("record.hint",
                 "Dial 211: if the prompt below is generated, it plays first, then a beep, then "
                 "recording starts. Speak, then hang up (or pause ~8 s / hit the max length "
                 "below, at which point recording just stops silently -- no warning beeps). "
                 "Mention the time limit in the prompt text below if callers should know it up "
                 "front. The message is saved as an 8 kHz mono WAV in "
                 "<a href=\"/recordings\">Recordings</a> / <a href=\"/settings?tab=storage\">"
                 "Settings &rarr; Storage</a>."),
        i18n_str("record.max_length_label", "Max recording length (seconds)"),
        (unsigned)(max_ms / 1000),
        (unsigned)(BOARD_RECORD_MAX_MS_MIN / 1000), (unsigned)(BOARD_RECORD_MAX_MS_MAX / 1000),
        i18n_str("general.button.save", "Save"));
    off = append_prompt_field(buf, cap, off, RECORD_PROMPT_NAME, NULL);
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0, "</div>");
    return off;
}

/* Firmware version + OTA upload -- pushes a .bin (the exact same file
 * `idf.py build` already produces, that you'd otherwise flash over USB)
 * into the inactive OTA slot and reboots into it. See ota_upload_handler()
 * for the actual streaming write, partitions.csv for the two-slot layout,
 * and CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE (sdkconfig.defaults) for the
 * auto-revert safety net if the new image never confirms itself healthy.
 * USB flashing (`idf.py flash`) still works exactly as before, unchanged
 * -- this is an additional way to update, not a replacement, and is still
 * the only option for a board that's never been flashed at all. */
static int append_firmware_card(char *buf, size_t cap, int off)
{
    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();
    char built[48];
    snprintf(built, sizeof(built), "%s %s", app->date, app->time);

    /* "Revert to previous version" only makes sense -- and is only offered
     * -- when the *other* OTA slot actually holds a confirmed-good image
     * (ESP_OTA_IMG_VALID). A board that's only ever been flashed once, or
     * whose other slot is still ESP_OTA_IMG_NEW/PENDING_VERIFY/INVALID,
     * has nothing safe to fall back to -- esp_ota_mark_app_invalid_
     * rollback_and_reboot() would just fail, so there's no point offering
     * a button that can't work. This is a deliberate manual escape hatch
     * distinct from the automatic crash/hang rollback in app_main.c: that
     * one only catches a new image that never finishes booting; this one
     * is for "it booted fine but I don't want it" -- a field board that's
     * behaving worse after an update, someone unsure a change is safe. */
    const esp_partition_t *other = esp_ota_get_next_update_partition(NULL);
    esp_ota_img_states_t other_state = ESP_OTA_IMG_UNDEFINED;
    bool can_revert = other && running && other != running &&
        esp_ota_get_state_partition(other, &other_state) == ESP_OK &&
        other_state == ESP_OTA_IMG_VALID;

    char revert_form[768] = "";
    if (can_revert) {
        snprintf(revert_form, sizeof(revert_form),
            "<form method=\"POST\" action=\"/ota_revert\" class=\"settings-form\" "
            "onsubmit=\"return confirm('%s')\">"
            "<div class=\"toolbar sub\"><button type=\"submit\" class=\"danger\">%s</button></div>"
            "</form>",
            i18n_str("firmware.revert_confirm",
                     "Revert to the previous firmware version and reboot right now?"),
            i18n_str("firmware.revert_button", "Revert to previous version"));
    }

    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<div class=\"card\"><h2>%s</h2>"
        "<div class=\"statusrow\"><span class=\"label\">%s</span><span>%s</span></div>"
        "<div class=\"statusrow\"><span class=\"label\">%s</span><span>%s</span></div>"
        "<div class=\"statusrow\"><span class=\"label\">%s</span><span>%s</span></div>"
        "<form method=\"POST\" action=\"/ota_upload\" enctype=\"multipart/form-data\" "
        "class=\"settings-form\" onsubmit=\"return confirm('%s')\">"
        "<label>%s <input type=\"file\" name=\"file\" accept=\".bin\" required></label>"
        "<div class=\"toolbar sub\"><button type=\"submit\" class=\"primary\">%s</button></div>"
        "</form>"
        "%s"
        "<p class=\"hint\">%s</p>"
        "</div>",
        i18n_str("firmware.title", "Firmware"),
        i18n_str("firmware.version_label", "Version"), app->version,
        i18n_str("firmware.built_label", "Built"), built,
        i18n_str("firmware.slot_label", "Running from"), running ? running->label : "?",
        i18n_str("firmware.confirm",
                 "Upload this firmware image and reboot into it? Make sure it's a build of "
                 "this same board type -- there's no way to check that before writing it."),
        i18n_str("firmware.file_label", "Firmware file (.bin, from idf.py build)"),
        i18n_str("firmware.button", "Upload and reboot"),
        revert_form,
        i18n_str("firmware.hint",
                 "Pushes the exact file <code>idf.py build</code> already produces "
                 "(<code>build/rotary_phone_gateway.bin</code>) -- the same one you'd otherwise "
                 "flash over USB. Writes to the inactive slot while this one keeps running, then "
                 "reboots into it. If the new image crashes or hangs before finishing boot, it "
                 "automatically reverts to this version on the next boot -- no USB cable needed "
                 "to recover. USB flashing (<code>idf.py flash</code>) still works exactly as "
                 "before; this is an additional option, not a replacement, and is still how you "
                 "flash a board for the very first time."));
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
                "<div class=\"statusrow\"><span class=\"label\">%s</span>"
                "<span class=\"pill on\">%s %s</span></div>",
                i18n_str("hardware.microsd", "microSD"),
                i18n_str("hardware.microsd.present", "present,"),
                fmt_bytes(storage_cap_bytes_sd(), sdc, sizeof(sdc)));
        } else {
            snprintf(sd_row, sizeof(sd_row),
                "<div class=\"statusrow\"><span class=\"label\">%s</span>"
                "<span class=\"pill off\">%s</span></div>",
                i18n_str("hardware.microsd", "microSD"),
                i18n_str("hardware.microsd.absent", "not detected -- inserted &amp; FAT-formatted?"));
        }
    }

    char fl[16], ru[16], rf[16], su[16], sf[16];
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<div class=\"card\"><h2>%s</h2>"
        "<div class=\"statusrow\"><span class=\"label\">%s</span><span>%s</span></div>"
        "<div class=\"statusrow\"><span class=\"label\">%s</span><span>%s</span></div>"
        "<div class=\"statusrow\"><span class=\"label\">%s</span>"
        "<span><code>%s</code></span></div>"
        "<div class=\"statusrow\"><span class=\"label\">%s</span><span>ESP-IDF %s</span></div>"
        "<div class=\"statusrow\"><span class=\"label\">%s</span><span>%s</span></div>"
        "<div class=\"statusrow\"><span class=\"label\">%s</span><span>%s / %s</span></div>"
        "%s"
        "<div class=\"statusrow\"><span class=\"label\">%s</span>"
        "<span>%s / %s%s</span></div>"
        "<p class=\"hint\">%s</p></div>",
        i18n_str("hardware.title", "Hardware"),
        i18n_str("hardware.type", "Type"), hw,
        i18n_str("hardware.codec_detected", "Codec (detected)"), board_codec_name(detected),
        i18n_str("hardware.serial", "Serial (chip MAC)"), serial,
        i18n_str("hardware.firmware", "Firmware"), esp_get_idf_version(),
        i18n_str("hardware.flash_total", "Flash (total)"), fmt_bytes(flash_bytes, fl, sizeof(fl)),
        i18n_str("hardware.ram", "RAM used / free"),
        fmt_bytes(ram_used, ru, sizeof(ru)), fmt_bytes(ram_free, rf, sizeof(rf)), sd_row,
        i18n_str("hardware.storage", "Storage used / free"),
        fmt_bytes(st_used, su, sizeof(su)), fmt_bytes(st_free, sf, sizeof(sf)),
        storage_using_sd() ? " (SD)" : "",
        i18n_str("hardware.hint",
                 "Type is fixed at build time (<code>CONFIG_BOARD_TYPE_*</code>) since GPIO "
                 "routing is per-PCB; the detected codec is probed live on the I2C bus and warns "
                 "in the log if it disagrees. Serial is the factory MAC -- unique per chip, "
                 "survives reflash. Flash is the whole chip; Storage is whichever backend is "
                 "primary (the microSD card above, if present, otherwise internal flash) -- see "
                 "<a href=\"/settings?tab=storage\">Settings &rarr; Storage</a> for both listed "
                 "separately. RAM is live internal heap. Uptime/WiFi are on the <a href=\"/\">"
                 "Status</a> page."));

    if (board_type_mismatch()) {
        char recorded[16];
        board_type_recorded(recorded, sizeof(recorded));
        char confirm_msg[300];
        snprintf(confirm_msg, sizeof(confirm_msg),
            i18n_str("hardware.board_type_confirm",
                     "Confirm this board is really %s now (was %s)? Only do this if the "
                     "hardware itself genuinely changed -- if you flashed the wrong firmware "
                     "by mistake, reflash the correct build instead."),
            board_pcb_variant_name(), recorded[0] ? recorded : "(unknown)");
        off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
            "<div class=\"card\"><h2>%s</h2>"
            "<p class=\"hint warn-text\">%s</p>"
            "<form method=\"POST\" action=\"/board_type_confirm\" class=\"settings-form\" "
            "onsubmit=\"return confirm('%s')\">"
            "<div class=\"toolbar sub\"><button type=\"submit\" class=\"danger\">%s</button></div>"
            "</form></div>",
            i18n_str("hardware.board_type_mismatch_title", "Board type mismatch"),
            i18n_str("hardware.board_type_mismatch_hint",
                     "This is only safe to confirm if this physical board genuinely is the type "
                     "this firmware was built for -- e.g. old hardware retired and replaced with "
                     "a different variant. If this firmware just got flashed onto the wrong "
                     "board by mistake, reflash the correct build instead of confirming."),
            confirm_msg,
            i18n_str("hardware.board_type_confirm_button", "Confirm board type changed"));
    }

    idle_power_cfg_t ip = idle_power_get();
    char ip_status[64];
    if (ip.enabled) {
        snprintf(ip_status, sizeof(ip_status), i18n_str("idlepower.status_on", "on, after %u min (%s)"),
                 (unsigned)ip.minutes,
                 ip.max_savings ? i18n_str("idlepower.max_savings", "max savings")
                                : i18n_str("idlepower.balanced", "balanced"));
    } else {
        snprintf(ip_status, sizeof(ip_status), "%s", i18n_str("general.offhook.status_off", "off"));
    }
    char opt_balanced[48], opt_max_savings[48];
    snprintf(opt_balanced, sizeof(opt_balanced), i18n_str("idlepower.opt_balanced", "Balanced (~%dms delay)"),
             IDLE_POWER_INTERVAL_MS_BALANCED + 80);
    snprintf(opt_max_savings, sizeof(opt_max_savings),
             i18n_str("idlepower.opt_max_savings", "Max savings (~%dms delay)"),
             IDLE_POWER_INTERVAL_MS_MAX_SAVINGS + 80);
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<div class=\"card\"><h2>%s</h2>"
        "<div class=\"statusrow\"><span class=\"label\">%s</span><span>%s</span></div>"
        "<form method=\"POST\" action=\"/idle_power\" class=\"settings-form\">"
        "<label><input type=\"checkbox\" name=\"enabled\" value=\"1\"%s> %s</label>"
        "<label>%s "
        "<input type=\"number\" name=\"minutes\" value=\"%u\" min=\"%d\" max=\"%d\" required></label>"
        "<label>%s"
        "<select name=\"max_savings\">"
        "<option value=\"0\"%s>%s</option>"
        "<option value=\"1\"%s>%s</option>"
        "</select></label>"
        "<div class=\"toolbar sub\"><button type=\"submit\" class=\"primary\">%s</button></div>"
        "</form>",
        i18n_str("idlepower.title", "Low-power idle mode"),
        i18n_str("general.current", "Current"), ip_status,
        ip.enabled ? " checked" : "", i18n_str("general.offhook.enabled", "Enabled"),
        i18n_str("idlepower.minutes_label", "Minutes idle before sleeping"),
        (unsigned)ip.minutes, IDLE_POWER_MINUTES_MIN, IDLE_POWER_MINUTES_MAX,
        i18n_str("idlepower.preset_label", "Duty-cycle preset"),
        !ip.max_savings ? " selected" : "", opt_balanced,
        ip.max_savings ? " selected" : "", opt_max_savings,
        i18n_str("general.button.save", "Save"));

    char idlepower_hint[420];
    snprintf(idlepower_hint, sizeof(idlepower_hint),
        i18n_str("idlepower.hint",
                 "Approximately %.2f&nbsp;W of power savings in the selected preset, but expect "
                 "up to ~%dms delay the first time you pick up the handset after %u idle "
                 "minutes; fully responsive for the rest of that call and for %u minutes after. "
                 "Both presets use the same idle-time trigger above -- only the delay/savings "
                 "tradeoff while asleep changes, so it's safe to switch and compare without "
                 "waiting out the idle timer again."),
        (double)idle_power_estimated_savings_w(), idle_power_worst_case_latency_ms(),
        (unsigned)ip.minutes, (unsigned)ip.minutes);
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<p class=\"hint\">%s</p></div>", idlepower_hint);

    off = append_firmware_card(buf, cap, off);

    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<div class=\"card\"><h2>%s</h2>"
        "<form method=\"POST\" action=\"/wifi_reprovision\" class=\"settings-form\" "
        "onsubmit=\"return confirm('%s')\">"
        "<div class=\"toolbar sub\"><button type=\"submit\">%s\xe2\x80\xa6</button></div>"
        "</form>"
        "<p class=\"hint\">%s</p>"
        "<form method=\"POST\" action=\"/factory_reset\" class=\"settings-form\" "
        "onsubmit=\"return confirm('%s')\">"
        "<div class=\"toolbar sub\"><button type=\"submit\" class=\"danger\">"
        "%s\xe2\x80\xa6</button></div>"
        "</form>"
        "<p class=\"hint\">%s</p>"
        "</div>",
        i18n_str("reset.title", "Reset"),
        i18n_str("reset.wifi.confirm",
                 "Clear the saved WiFi network and reboot into setup mode? "
                 "Every other setting stays as-is."),
        i18n_str("reset.wifi.button", "Re-provision WiFi"),
        i18n_str("reset.wifi.hint",
                 "Clears the saved WiFi network only and reboots into setup mode (the "
                 "<code>AnalogRedial-XXXX</code> network) -- everything else (board name, dial "
                 "codes, VoIP, recordings) is untouched. Use this when moving the board to a "
                 "different WiFi network. Forgot to click this before moving the board, and can't "
                 "reach this page anymore? Hold the <b>BOOT</b> button on the ESP32 module for "
                 "3&nbsp;seconds <em>while it's already running</em> (not during power-on, which "
                 "instead puts the chip in USB flashing mode) -- does the exact same thing, no web "
                 "UI needed."),
        i18n_str("reset.factory.confirm",
                 "Reset ALL settings to factory defaults and reboot? This clears WiFi, board "
                 "name, dial codes, VoIP, and every other saved setting. Uploaded audio and "
                 "recordings are NOT deleted. This cannot be undone."),
        i18n_str("reset.factory.button", "Reset to factory settings"),
        i18n_str("reset.factory.hint",
                 "Erases every saved setting (WiFi, board name, dial codes, VoIP, idle-power, "
                 "log level) and reboots to a genuinely out-of-box state -- for handing the "
                 "board to someone else. Uploaded audio and recordings are files, not settings, "
                 "and are <strong>not</strong> deleted -- clear those individually from Settings "
                 "\xe2\x86\x92 Storage first if you want a truly clean board."));

    return off;
}

/* Friendly label + one-line description for a partition, by its
 * partitions.csv label -- everything else (a partition added later without
 * updating this list) just shows its raw label instead, not an error. */
static const char *partition_friendly_name(const char *label)
{
    if (strcmp(label, "nvs") == 0)       return i18n_str("memmap.nvs", "Settings (NVS)");
    if (strcmp(label, "phy_init") == 0)  return i18n_str("memmap.phy_init", "WiFi radio calibration data");
    if (strcmp(label, "factory") == 0)   return i18n_str("memmap.app", "App code");
    if (strcmp(label, "storage") == 0)   return i18n_str("memmap.storage", "Storage (recordings/uploads)");
    if (strcmp(label, "bundled") == 0)   return i18n_str("memmap.bundled", "Bundled assets (built-in clips)");
    return label;
}

/* Live flash/RAM breakdown -- every number here is read from the running
 * device (esp_partition_find() walks the actual flashed partition table,
 * not a copy of partitions.csv baked in here), so it stays correct even if
 * that file changes later without anyone updating this card. Answers "how
 * full is each partition" and "would an Nmb flash chip fit this layout"
 * with real numbers instead of a guess. */
static int append_memory_map_card(char *buf, size_t cap, int off)
{
    size_t ram_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    size_t ram_free  = esp_get_free_heap_size();
    size_t ram_used  = (ram_total > ram_free) ? ram_total - ram_free : 0;
    uint32_t flash_bytes = 0;
    esp_flash_get_size(NULL, &flash_bytes);

    /* Separate buffers, not one reused across calls -- fmt_bytes() calls as
     * sibling arguments to the same snprintf() run in an unspecified order,
     * so a shared buffer can end up holding the LAST call's text by the
     * time snprintf reads an EARLIER %s from it (a real, if cosmetic, bug
     * this card shipped with initially: RAM total showed the flash size). */
    char ru[16], rt[16], ft[16];
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<div class=\"card wide\"><h2>%s</h2>"
        "<div class=\"statusrow\"><span class=\"label\">%s</span>"
        "<span>%s / %s %s (%u%%)</span></div>"
        "<div class=\"membar\"><div class=\"membarfill\" style=\"width:%u%%\"></div></div>"
        "<div class=\"statusrow\" style=\"margin-top:10px\">"
        "<span class=\"label\">%s</span><span>%s</span></div>",
        i18n_str("memmap.title", "Memory map"),
        i18n_str("memmap.ram", "RAM"),
        fmt_bytes(ram_used, ru, sizeof(ru)), fmt_bytes(ram_total, rt, sizeof(rt)),
        i18n_str("memmap.used", "used"),
        ram_total ? (unsigned)(100ULL * ram_used / ram_total) : 0,
        ram_total ? (unsigned)(100ULL * ram_used / ram_total) : 0,
        i18n_str("hardware.flash_total", "Flash (total)"), fmt_bytes(flash_bytes, ft, sizeof(ft)));

    /* Walk every partition actually in the flashed table ONCE, copying what
     * we need out -- partition.c's esp_partition_next() invalidates every
     * existing copy of the iterator it's called on AND already releases it
     * internally the moment it reaches the end of the list (see
     * esp_partition_next()'s "it->next_item == NULL" branch in partition.c),
     * returning NULL only after that free. So this loop's own `it` is
     * already gone by the time it terminates normally -- an extra
     * esp_partition_iterator_release(it) after the loop double-frees it
     * (this crashed with a TLSF double-free assert the first two times
     * around: once from re-walking `it` a second time in a separate loop,
     * then again from this). Don't call release() at all here; the loop
     * already owns that. Collecting into this small fixed array up front
     * also means nothing needs `it` again afterward anyway -- there are
     * only ever a handful of partitions. */
    esp_partition_t parts[MEMORY_MAP_MAX_PARTS];
    size_t n_parts = 0;
    uint32_t allocated = 0;
    uint32_t lowest_addr = flash_bytes;
    esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);
    for (esp_partition_iterator_t cur = it; cur; cur = esp_partition_next(cur)) {
        const esp_partition_t *p = esp_partition_get(cur);
        allocated += p->size;
        if (p->address < lowest_addr) lowest_addr = p->address;
        if (n_parts < MEMORY_MAP_MAX_PARTS) parts[n_parts++] = *p;
    }
    if (lowest_addr > 0 && lowest_addr < flash_bytes) allocated += lowest_addr;

    if (lowest_addr > 0) {
        char ls[16];
        off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
            "<div class=\"statusrow indent\"><span class=\"label\">%s</span>"
            "<span>%s (%s)</span></div>",
            i18n_str("memmap.bootloader", "Bootloader + partition table"),
            fmt_bytes(lowest_addr, ls, sizeof(ls)), i18n_str("memmap.fixed", "fixed"));
    }

    for (size_t i = 0; i < n_parts; i++) {
        const esp_partition_t *p = &parts[i];
        bool have_used = false;
        uint64_t used = 0;

        if (strcmp(p->label, "storage") == 0) {
            used = storage_used_bytes_internal();
            have_used = true;
        } else if (strcmp(p->label, "bundled") == 0) {
            storage_entry_t *entries = malloc(STORAGE_LIST_MAX * sizeof(storage_entry_t));
            if (entries) {
                size_t n = storage_list_bundled(entries, STORAGE_LIST_MAX);
                for (size_t j = 0; j < n; j++) used += entries[j].size;
                free(entries);
                have_used = true;
            }
        } else if (strcmp(p->label, "nvs") == 0) {
            nvs_stats_t st;
            if (nvs_get_stats(NULL, &st) == ESP_OK && st.total_entries) {
                /* NVS entries are fixed 32-byte pages -- an approximation
                 * (a value can span several), but close enough to size this
                 * partition against, and the % used is exact either way. */
                used = (uint64_t)st.used_entries * 32;
                have_used = true;
            }
        } else if (p->type == ESP_PARTITION_TYPE_APP) {
            /* ota_0/ota_1: real on-flash image size (header + segments +
             * appended hash), not just "it's an app slot, who knows how
             * much of it is spoken for." esp_image_get_metadata() only
             * parses headers to add up segment lengths (do_verify=false,
             * skip_check_checksum=true internally) -- it does NOT hash or
             * verify the whole ~1.4MB image, so this is cheap enough to
             * call synchronously on every page load, unlike a full
             * esp_ota_*-style validation pass would be. A never-written
             * slot (blank/erased flash, e.g. ota_1 before its first OTA
             * push) fails header parsing here -- left as have_used=false,
             * which correctly falls through to the plain "capacity" row
             * below instead of claiming a false 0% used. */
            esp_partition_pos_t pos = { .offset = p->address, .size = p->size };
            esp_image_metadata_t meta;
            if (esp_image_get_metadata(&pos, &meta) == ESP_OK) {
                used = meta.image_len;
                have_used = true;
            }
        }

        char cs[16], us[16];
        unsigned pct = (p->size && have_used) ? (unsigned)(100ULL * used / p->size) : 0;
        if (have_used) {
            off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
                "<div class=\"statusrow indent\"><span class=\"label\">%s</span>"
                "<span>%s / %s (%u%%)</span></div>"
                "<div class=\"membar\"><div class=\"membarfill\" style=\"width:%u%%\"></div></div>",
                partition_friendly_name(p->label), fmt_bytes((size_t)used, us, sizeof(us)),
                fmt_bytes(p->size, cs, sizeof(cs)), pct, pct);
        } else {
            off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
                "<div class=\"statusrow indent\"><span class=\"label\">%s</span>"
                "<span>%s %s</span></div>",
                partition_friendly_name(p->label), fmt_bytes(p->size, cs, sizeof(cs)),
                i18n_str("memmap.capacity", "capacity"));
        }
    }

    uint32_t unallocated = (flash_bytes > allocated) ? flash_bytes - allocated : 0;
    char us[16], fs[16];
    /* This hint's translated text still carries a %s (the allocated-bytes
     * figure) -- render into a scratch buffer first, same two-step pattern
     * used elsewhere in this file for the same reason. */
    char fit8mb_hint[500];
    snprintf(fit8mb_hint, sizeof(fit8mb_hint),
        (allocated <= 8u * 1024 * 1024)
            ? i18n_str("memmap.fits_8mb",
                       "This board ships with a 16 MB flash chip; only %s of it is actually "
                       "spoken for by the partition table above -- an 8 MB chip would fit this "
                       "exact layout comfortably.")
            : i18n_str("memmap.no_fit_8mb",
                       "This board ships with a 16 MB flash chip; %s of it is actually spoken "
                       "for by the partition table above -- an 8 MB chip would NOT fit this "
                       "exact layout as flashed today. The Storage and/or Bundled assets "
                       "partitions would need to shrink first (see their rows above for how "
                       "full they actually are right now)."),
        fmt_bytes(allocated, fs, sizeof(fs)));
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<div class=\"statusrow indent\"><span class=\"label\">%s</span>"
        "<span>%s %s</span></div>"
        "<p class=\"hint\">%s</p></div>",
        i18n_str("memmap.unallocated", "Unallocated"), fmt_bytes(unallocated, us, sizeof(us)),
        i18n_str("memmap.unallocated_hint", "free for future use (more/bigger partitions, ...)"),
        fit8mb_hint);

    return off;
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
        "<div class=\"card\"><h2>%s</h2>"
        "<div class=\"statusrow\"><span class=\"label\">%s</span><span>%s%s</span></div>",
        i18n_str("hwconfig.title", "Line 2 / microSD configuration"),
        i18n_str("hwconfig.current", "Current setting"),
        cfg.num_lines > 1 ? i18n_str("hwconfig.2line", "2-line board (jumpers + 2nd AG1171 populated)")
                           : (cfg.has_microsd ? i18n_str("hwconfig.1line_sd", "1-line board, microSD populated")
                                               : i18n_str("hwconfig.1line_nosd", "1-line board, no microSD")),
        confirmed ? "" : i18n_str("hwconfig.unconfirmed",
                                  " -- <strong>unconfirmed default, please check below</strong>"));

    if (!confirmed) {
        board_hw_evidence_t ev = board_hw_detect_evidence();
        off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
            "<div class=\"statusrow\"><span class=\"label\">%s</span>"
            "<span>%s</span></div>",
            i18n_str("hwconfig.card_detect", "microSD card detect"),
            ev.card_present ? i18n_str("hwconfig.card_present", "card physically present")
                            : i18n_str("hwconfig.card_absent", "no card detected"));
        if (ev.valid) {
            off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
                "<div class=\"statusrow\"><span class=\"label\">%s</span>"
                "<span>%s</span></div>",
                i18n_str("hwconfig.evidence", "Advisory evidence"),
                ev.looks_like_line2
                    ? i18n_str("hwconfig.evidence_line2", "looks more like a populated line 2")
                    : i18n_str("hwconfig.evidence_no_line2",
                               "looks more like an unpopulated line 2 (no SD hardware would "
                               "explain this either)"));
        }
    }

    /* Two orthogonal selects rather than one flattened variant list: what
     * GPIO18-21 are wired to (line 2 XOR microSD -- a real either/or) and
     * whether a BM64 companion is fitted, which is independent of both. The
     * old single dropdown would have needed an entry per combination, and
     * the board generation isn't in here at all because it's a compile-time
     * choice (CONFIG_BOARD_TYPE_*) -- it's shown above and confirmed
     * separately, not selected. */
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<form method=\"POST\" action=\"/board_hw_set\" class=\"settings-form\">"
        "<label>%s"
        "<select name=\"lines\">"
        "<option value=\"2line\"%s>%s</option>"
        "<option value=\"1line_sd\"%s>%s</option>"
        "<option value=\"1line_nosd\"%s>%s</option>"
        "</select></label>"
        "<label>%s"
        "<select name=\"bt\">"
        "<option value=\"0\"%s>%s</option>"
        "<option value=\"1\"%s>%s</option>"
        "</select></label>"
        "<button type=\"submit\">%s</button>"
        "</form>"
        "<p class=\"hint\">%s %s</p>"
        "<p class=\"hint\">%s</p></div>",
        i18n_str("hwconfig.variant_label", "GPIO18-21 wired to"),
        cfg.num_lines > 1 ? " selected" : "",
        i18n_str("hwconfig.2line", "A second phone line (jumpers + 2nd AG1171 populated)"),
        (cfg.num_lines == 1 && cfg.has_microsd) ? " selected" : "",
        i18n_str("hwconfig.opt_1line_sd", "microSD slot (one phone line only)"),
        (cfg.num_lines == 1 && !cfg.has_microsd) ? " selected" : "",
        i18n_str("hwconfig.opt_1line_nosd", "Neither (one phone line, no microSD)"),
        i18n_str("hwconfig.bt_label", "Bluetooth companion module"),
        !cfg.has_bt_module ? " selected" : "",
        i18n_str("hwconfig.bt_no", "Not fitted"),
        cfg.has_bt_module ? " selected" : "",
        i18n_str("hwconfig.bt_yes", "BM64 module fitted"),
        i18n_str("hwconfig.button", "Save (takes effect on reboot)"),
        i18n_str("hwconfig.hint",
                 "This board can carry a second phone line <em>or</em> a microSD card, never "
                 "both -- pick jumpers and a second AG1171 chip for line 2, or leave the "
                 "jumpers off to use the microSD slot instead. Firmware can't see which one is "
                 "actually populated, so a wrong answer here drives real hardware as the wrong "
                 "thing"),
        confirmed
            ? i18n_str("hwconfig.hint_confirmed", "-- please double-check before changing it")
            : i18n_str("hwconfig.hint_unconfirmed",
                       "-- the setting above was pre-filled from onboard evidence and should "
                       "already work, but please confirm it matches what you actually built"),
        i18n_str("hwconfig.bt_hint",
                 "The Bluetooth setting is about whether the hardware is physically present, "
                 "which is separate from switching the feature on in Settings -> Bluetooth. "
                 "The companion's audio is always line 0 and its UART is always GPIO16/17, so "
                 "it works alongside either choice above -- but entering the module's EEPROM "
                 "programming mode temporarily borrows line 1's control pins, so that step "
                 "needs a board without a second line."));

    return off;
}

/* Diagnostics tab: runtime log level (and, in test builds, test-mode
 * controls -- see Phase 2). */
/* Guided hardware bring-up check (selftest.h). Structured around the lesson
 * from 2026-09-26: the hook path and the audio path fail independently, so
 * each line gets one check for each, and a pass on one says nothing about the
 * other. Three step kinds render differently -- automatic ones are already
 * judged, ACTION steps ask the operator to do something physical and have
 * firmware verify it, CONFIRM steps can only be judged by ear. */
static int append_selftest_card(char *buf, size_t cap, int off)
{
    const selftest_report_t *rep = selftest_report();
    char board_id[16] = {0};
    selftest_board_id(board_id, sizeof(board_id));

    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<div class=\"card wide\"><h2>%s</h2>"
        "<div class=\"statusrow\"><span class=\"label\">%s</span><span>%s</span></div>"
        "<p class=\"hint\">%s</p>"
        "<form method=\"POST\" action=\"/selftest_begin\" class=\"settings-form\">"
        "<div class=\"toolbar sub\"><button type=\"submit\" class=\"primary\">%s</button></div>"
        "</form>",
        i18n_str("diag.selftest.title", "Hardware bring-up check"),
        i18n_str("diag.selftest.board_id", "Board ID"), board_id,
        i18n_str("diag.selftest.hint",
                 "Checks a freshly-built board one subsystem at a time. Hook detection and "
                 "audio are tested separately on each line, because they fail independently -- "
                 "a line can detect the handset perfectly and still pass no audio, or pass "
                 "audio while never seeing the hook. Results are stored on this board, so a "
                 "later failure can be compared against a run that passed."),
        i18n_str("diag.selftest.start", "Run automatic checks"));

    if (rep->n > 0) {
        off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
            "<table class=\"filetable\"><thead><tr><th>%s</th><th>%s</th><th>%s</th></tr>"
            "</thead><tbody>",
            i18n_str("diag.selftest.col_check", "Check"),
            i18n_str("diag.selftest.col_result", "Result"),
            i18n_str("diag.selftest.col_what", "Detail / what to do"));

        for (int i = 0; i < rep->n; i++) {
            const selftest_check_t *c = &rep->checks[i];
            const char *badge, *label;
            switch (c->result) {
                case SELFTEST_PASS: badge = "on";  label = i18n_str("diag.selftest.pass", "pass"); break;
                case SELFTEST_FAIL: badge = "off"; label = i18n_str("diag.selftest.fail", "FAIL"); break;
                case SELFTEST_SKIP: badge = "";    label = i18n_str("diag.selftest.skip", "n/a"); break;
                default:            badge = "warn"; label = i18n_str("diag.selftest.todo", "to do"); break;
            }
            char detail[SELFTEST_DETAIL_MAX * 2];
            html_escape(c->detail[0] ? c->detail : (c->instruction ? c->instruction : ""),
                        detail, sizeof(detail));

            off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
                "<tr><td>%s</td><td><span class=\"pill %s\">%s</span></td><td>%s",
                c->title, badge, label, detail);

            /* Controls only where a human still has something to do. */
            if (c->result == SELFTEST_PENDING && c->kind == SELFTEST_ACTION) {
                off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
                    "<form method=\"POST\" action=\"/selftest_action\" class=\"settings-form\">"
                    "<input type=\"hidden\" name=\"id\" value=\"%s\">"
                    "<div class=\"toolbar sub\"><button type=\"submit\">%s</button></div></form>",
                    c->id, i18n_str("diag.selftest.watch", "Watch for it now (15s)"));
            } else if (c->result == SELFTEST_PENDING && c->kind == SELFTEST_CONFIRM) {
                if (strncmp(c->id, "ring", 4) == 0) {
                    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
                        "<form method=\"POST\" action=\"/selftest_stimulus\" class=\"settings-form\">"
                        "<input type=\"hidden\" name=\"id\" value=\"%s\">"
                        "<div class=\"toolbar sub\"><button type=\"submit\">%s</button></div></form>",
                        c->id, i18n_str("diag.selftest.ring_now", "Ring it now"));
                }
                off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
                    "<form method=\"POST\" action=\"/selftest_answer\" class=\"settings-form\">"
                    "<input type=\"hidden\" name=\"id\" value=\"%s\">"
                    "<div class=\"toolbar sub\">"
                    "<button type=\"submit\" name=\"ok\" value=\"1\">%s</button> "
                    "<button type=\"submit\" name=\"ok\" value=\"0\">%s</button>"
                    "</div></form>",
                    c->id, i18n_str("diag.selftest.yes", "Yes"),
                    i18n_str("diag.selftest.no", "No"));
            }
            off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0, "</td></tr>");
        }
        off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
            "</tbody></table>"
            "<form method=\"POST\" action=\"/selftest_save\" class=\"settings-form\">"
            "<div class=\"toolbar sub\"><button type=\"submit\" class=\"primary\">%s</button></div>"
            "</form>",
            i18n_str("diag.selftest.save", "Record this run on the board"));
    }

    /* Prior runs: the whole reason results live on the board rather than on
     * whoever ran the test -- "did this ever pass?" months later. */
    selftest_run_t hist[SELFTEST_HISTORY_MAX];
    size_t n_hist = selftest_history_get(hist, SELFTEST_HISTORY_MAX);
    if (n_hist > 0) {
        off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
            "<h3>%s</h3><table class=\"filetable\"><thead><tr><th>%s</th><th>%s</th></tr>"
            "</thead><tbody>",
            i18n_str("diag.selftest.history", "Previous runs"),
            i18n_str("diag.selftest.when", "When"),
            i18n_str("diag.selftest.outcome", "Outcome"));
        for (size_t i = 0; i < n_hist; i++) {
            char when[40];
            if (hist[i].unix_time) {
                time_t t = (time_t)hist[i].unix_time;
                struct tm tmv;
                gmtime_r(&t, &tmv);
                strftime(when, sizeof(when), "%Y-%m-%d %H:%M UTC", &tmv);
            } else {
                snprintf(when, sizeof(when), "%s",
                         i18n_str("diag.selftest.no_clock", "date unknown (clock unsynced)"));
            }
            off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
                "<tr><td>%s</td><td>%u pass, %u fail, %u n/a, %u unanswered</td></tr>",
                when, hist[i].n_pass, hist[i].n_fail, hist[i].n_skip, hist[i].n_pending);
        }
        off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0, "</tbody></table>");
    }

    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0, "</div>");
    return off;
}

static int append_diag_tab(char *buf, size_t cap, int off)
{
    static const char *LEVELS[] = { "none", "error", "warn", "info", "debug", "verbose" };
    esp_log_level_t cur = logctl_get_level();

    off = append_selftest_card(buf, cap, off);

    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<div class=\"card\"><h2>%s</h2>"
        "<div class=\"statusrow\"><span class=\"label\">%s</span>"
        "<span class=\"pill on\">%s</span></div>"
        "<form method=\"POST\" action=\"/log_level\" class=\"settings-form\">"
        "<label>%s <select name=\"level\">",
        i18n_str("diag.logging.title", "Logging"),
        i18n_str("diag.logging.current", "Current level"), logctl_level_name(cur),
        i18n_str("diag.logging.level_label", "Level"));
    for (size_t i = 0; i < sizeof(LEVELS) / sizeof(LEVELS[0]); i++) {
        off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
            "<option value=\"%s\"%s>%s</option>",
            LEVELS[i], ((size_t)cur == i) ? " selected" : "", LEVELS[i]);
    }
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "</select></label>"
        "<div class=\"toolbar sub\"><button type=\"submit\" class=\"primary\">%s</button></div>"
        "</form>"
        "<p class=\"hint\">%s</p></div>",
        i18n_str("diag.logging.set_level", "Set level"),
        i18n_str("diag.logging.hint",
                 "Applies immediately and persists across reboots (NVS). <code>debug</code> "
                 "logs rotary pulse timing, hook-switch make/break with durations, dialed-digit "
                 "collection, and telephony state changes -- watch it live in the log below, or "
                 "over the serial console (<code>log_level debug</code>). WiFi/PHY noise stays "
                 "at warn so it doesn't bury the app's own debug output."));

    /* Test mode + audio loopback + SD self-test (DVT harness controls; also
     * usable by hand here). */
    bool tmode = telephony_test_enabled();
    bool loop  = audio_test_loopback();
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<div class=\"card\"><h2>%s</h2>"
        "<div class=\"statusrow\"><span class=\"label\">%s</span>"
        "<span class=\"pill %s\">%s</span></div>"
        "<div class=\"toolbar sub\">"
        "<form method=\"POST\" action=\"/test/mode\" style=\"display:inline\">"
        "<input type=\"hidden\" name=\"on\" value=\"1\"><button type=\"submit\">%s</button></form>"
        "<form method=\"POST\" action=\"/test/mode\" style=\"display:inline\">"
        "<input type=\"hidden\" name=\"on\" value=\"0\"><button type=\"submit\">%s</button></form>"
        "</div>"
        "<div class=\"statusrow\"><span class=\"label\">%s</span>"
        "<span class=\"pill %s\">%s</span></div>"
        "<div class=\"toolbar sub\">"
        "<form method=\"POST\" action=\"/test/loopback\" style=\"display:inline\">"
        "<input type=\"hidden\" name=\"on\" value=\"1\"><button type=\"submit\">%s</button></form>"
        "<form method=\"POST\" action=\"/test/loopback\" style=\"display:inline\">"
        "<input type=\"hidden\" name=\"on\" value=\"0\"><button type=\"submit\">%s</button></form>"
        "</div>"
        "<div class=\"toolbar sub\">"
        "<form method=\"POST\" action=\"/test/sd\" style=\"display:inline\">"
        "<button type=\"submit\">%s</button></form>"
        "</div>"
        "<p class=\"hint\">%s</p></div>",
        i18n_str("diag.test.title", "Test mode"),
        i18n_str("diag.test.injection", "Injection"),
        tmode ? "on" : "off", tmode ? "ENABLED" : "off",
        i18n_str("aec.enable", "Enable"), i18n_str("aec.disable", "Disable"),
        i18n_str("diag.test.loopback", "Audio TX&rarr;RX loopback"),
        loop  ? "on" : "off", loop  ? "ENABLED" : "off",
        i18n_str("aec.enable", "Enable"), i18n_str("aec.disable", "Disable"),
        i18n_str("diag.test.sd_selftest", "Run microSD self-test"),
        i18n_str("diag.test.hint",
                 "Test mode lets the design-verification harness simulate a handset (off-hook, "
                 "hang-up, dialed digits) without physical dialing. Loopback feeds synthesized "
                 "TX audio back into the RX detector so a played tone/DTMF is confirmable "
                 "without a handset. Leave both <b>off</b> for normal use. Full protocol + host "
                 "runner: <code>firmware/test/</code>."));

    int diag_num_lines = board_hw_config_get().num_lines;
    char diag_line_opts[128] = "";
    {
        int o = 0;
        for (int l = 0; l < diag_num_lines; l++) {
            o += snprintf(diag_line_opts + o, sizeof(diag_line_opts) - o,
                           "<option value=\"%d\">Line %d</option>", l, l);
        }
    }

    /* Tone generator: same audio_tx_burst() the off-hook alert and AEC
     * echo-probe/sweep bring-up commands already use (see bringup.c's
     * `tone` console command, the same feature, console-only until now) --
     * a real, distinct tone out the earpiece confirms the DAC/amp path end
     * to end, which loopback above deliberately doesn't (that stays inside
     * the chip, digital TX->RX, never touches the codec at all). */
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<div class=\"card\"><h2>%s</h2>"
        "<form method=\"POST\" action=\"/test/tone\" class=\"settings-form\">"
        "<label>%s <select name=\"line\">%s</select></label>"
        "<label>%s <input type=\"number\" name=\"hz\" value=\"1000\" min=\"20\" max=\"20000\" required></label>"
        "<label>%s <input type=\"number\" name=\"ms\" value=\"1000\" min=\"50\" max=\"5000\" required></label>"
        "<div class=\"toolbar sub\"><button type=\"submit\" class=\"primary\">%s</button></div>"
        "</form>"
        "<p class=\"hint\">%s</p></div>",
        i18n_str("diag.tone.title", "Tone generator"),
        i18n_str("status.line.title_bare", "Line"), diag_line_opts,
        i18n_str("diag.tone.hz_label", "Frequency (Hz)"),
        i18n_str("diag.tone.ms_label", "Duration (ms, max 5000)"),
        i18n_str("diag.tone.button", "Play"),
        i18n_str("diag.tone.hint",
                 "Plays a single-frequency tone out the selected line's earpiece, straight "
                 "through the real DAC/amp path (not the digital loopback above) -- pick up "
                 "that line's handset first to hear it. Same underlying call as the bring-up "
                 "console's <code>tone</code> command."));

    /* Mic record test: captures raw uplink PCM (audio_rx_read(), the exact
     * path AEC/VoIP uplink also reads from) straight to a WAV via the same
     * write-header-now-patch-sizes-later pattern used elsewhere in this
     * file (convert_and_write()/build_wav_header() above) -- an actual
     * listen-back test of the mic path end to end, not just a live level
     * number, which wouldn't tell you if what's picked up actually sounds
     * right (muffled, clipping, wrong gain). Fixed filename per line
     * (overwrites on each run, like the AM greeting) so this doesn't pile
     * up recordings under a real user-facing name. */
    for (int l = 0; l < diag_num_lines; l++) {
        char fname[32];
        snprintf(fname, sizeof(fname), "diag_mic_test_l%d.wav", l);
        uint8_t probe; size_t got;
        bool have = (storage_read(fname, &probe, sizeof(probe), &got) == ESP_OK);
        char player_row[220] = "";
        if (have) {
            snprintf(player_row, sizeof(player_row),
                "<div class=\"statusrow\"><span class=\"label\">%s</span>"
                "<span><audio controls preload=\"none\" src=\"/media/%s?inline=1\"></audio></span></div>",
                i18n_str("diag.mic.last", "Last recording"), fname);
        }
        char line_h3[24];
        snprintf(line_h3, sizeof(line_h3), i18n_str("status.line.title", "Line %d"), l);
        off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
            "%s<h3>%s</h3>"
            "<form method=\"POST\" action=\"/test/mic_record\" class=\"settings-form\">"
            "<input type=\"hidden\" name=\"line\" value=\"%d\">"
            "<label>%s <input type=\"number\" name=\"secs\" value=\"3\" min=\"1\" max=\"8\" required></label>"
            "<div class=\"toolbar sub\"><button type=\"submit\" class=\"primary\">%s</button></div>"
            "</form>%s",
            (l == 0) ? "<div class=\"card\"><h2>Mic record test</h2>" : "",
            line_h3, l,
            i18n_str("diag.mic.secs_label", "Seconds to record"),
            i18n_str("diag.mic.button", "Record"),
            player_row);
    }
    if (diag_num_lines > 0) {
        off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
            "<p class=\"hint\">%s</p></div>",
            i18n_str("diag.mic.hint",
                     "Pick up that line's handset (off-hook) BEFORE submitting -- the mic path "
                     "isn't routed on-hook, same requirement as the Echo Cancellation Measure "
                     "button above, so this just silently records digital silence otherwise (a "
                     "flat, all-zero file, not a crash or error -- easy to mistake for the test "
                     "not working when it's really just an on-hook capture). Records straight "
                     "from that line's mic (through the AEC/uplink audio path, same source VoIP "
                     "would encode) to a WAV, blocking the page for the duration -- speak into "
                     "the handset while it's running. Play it back above to check it sounds "
                     "right (level, clarity, no clipping). Each run overwrites "
                     "that line's previous test recording."));
    }

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

/* Tiny in-RAM cache of computed durations, keyed on name+size (size doubles
 * as a cheap invalidation check -- a file getting replaced under the same
 * name almost always changes size; the rare same-size replacement shows a
 * stale duration until reboot, an acceptable display nicety tradeoff, not
 * something worth a real mtime/hash check for). Round-robins over a fixed
 * array rather than anything fancier -- a personal phone's file count fits
 * easily, and even a full linear scan of it is nothing next to one SD file
 * open. Cleared on reboot only (no persistence): recomputing after a
 * restart is cheap relative to the flash wear or NVS-slot churn of
 * persisting it. */
#define WAV_DURATION_CACHE_N 48
typedef struct {
    bool   used;
    char   name[STORAGE_MAX_NAME];
    size_t size;
    char   dur[16];
} wav_duration_cache_entry_t;
static wav_duration_cache_entry_t s_dur_cache[WAV_DURATION_CACHE_N];
static size_t s_dur_cache_next; /* round-robin eviction cursor */

static bool dur_cache_get(const char *name, size_t size, char *out, size_t out_len)
{
    for (size_t i = 0; i < WAV_DURATION_CACHE_N; i++) {
        if (s_dur_cache[i].used && s_dur_cache[i].size == size &&
            strcmp(s_dur_cache[i].name, name) == 0) {
            strncpy(out, s_dur_cache[i].dur, out_len - 1);
            out[out_len - 1] = '\0';
            return true;
        }
    }
    return false;
}

static void dur_cache_put(const char *name, size_t size, const char *dur)
{
    /* Overwrite an existing entry for this name (a stale size means the
     * file changed) if there is one, else claim the next round-robin slot
     * -- keeps at most one entry per name rather than accumulating dead
     * ones for files that get re-recorded under the same name. */
    for (size_t i = 0; i < WAV_DURATION_CACHE_N; i++) {
        if (s_dur_cache[i].used && strcmp(s_dur_cache[i].name, name) == 0) {
            s_dur_cache[i].size = size;
            strncpy(s_dur_cache[i].dur, dur, sizeof(s_dur_cache[i].dur) - 1);
            s_dur_cache[i].dur[sizeof(s_dur_cache[i].dur) - 1] = '\0';
            return;
        }
    }
    wav_duration_cache_entry_t *e = &s_dur_cache[s_dur_cache_next];
    s_dur_cache_next = (s_dur_cache_next + 1) % WAV_DURATION_CACHE_N;
    e->used = true;
    strncpy(e->name, name, sizeof(e->name) - 1);
    e->name[sizeof(e->name) - 1] = '\0';
    e->size = size;
    strncpy(e->dur, dur, sizeof(e->dur) - 1);
    e->dur[sizeof(e->dur) - 1] = '\0';
}

/* Reads a stored WAV's header to compute its duration, formats it, and
 * caches the result (see dur_cache_get/put above) so a page revisited
 * without that file changing skips the read+parse entirely. On a cache
 * miss: tries WAV_DURATION_SCAN_BYTES (512) first -- this project's own
 * files (recorder.c output, converted uploads) have a plain ~44-byte
 * canonical header, so that's normally enough on the first try, which
 * matters a lot on this board's SPI-mode microSD (real per-file-open
 * overhead, not just transfer time) -- and only reaches for the full
 * UPLOAD_HEADER_SCAN_BYTES (8192) if that comes up short. That fallback is
 * load-bearing, not defensive: this project's own operator.wav is
 * generated with a "FLLR" filler chunk pushing the data chunk out to
 * ~4096 bytes (confirmed live, 512 alone silently produced a blank
 * duration for it), so some real files here genuinely need the bigger
 * scan. audio_wav_parse() is a tolerant chunk walker (see its own doc
 * comment) that returns false either way if the buffer runs out before
 * finding `data` -- can't distinguish "just needs more bytes" from
 * "not a WAV at all" from that alone, so the fallback re-reads for both,
 * but only the genuinely-large-header case pays for it twice; a
 * corrupt/non-WAV file just fails fast both times. Both scan sizes are
 * heap-allocated, never a stack array (see UPLOAD_HEADER_SCAN_BYTES's own
 * doc comment on why that matters on this task specifically). Blank
 * output (not an error) for anything that never parses as PCM WAV -- this
 * is a display nicety, not something worth failing the whole page render
 * over. */
static void wav_duration_str(const char *name, size_t size, char *out, size_t out_len)
{
    out[0] = '\0';
    if (dur_cache_get(name, size, out, out_len)) return;

    audio_wav_info_t wav;
    bool ok = false;
    size_t scan_sizes[2] = { WAV_DURATION_SCAN_BYTES, UPLOAD_HEADER_SCAN_BYTES };
    for (size_t pass = 0; pass < 2 && !ok; pass++) {
        uint8_t *header = malloc(scan_sizes[pass]);
        if (!header) break;
        size_t got = 0;
        esp_err_t err = storage_read(name, header, scan_sizes[pass], &got);
        bool short_read = (got < scan_sizes[pass]); /* true EOF -- see below */
        if (err == ESP_OK && got > 0) {
            ok = audio_wav_parse(header, got, &wav) && wav.sample_rate != 0 &&
                 wav.channels != 0 && wav.bits_per_sample != 0;
        }
        free(header);
        /* A short read means true EOF -- a bigger buffer can't find bytes
         * that aren't in the file, so don't bother with the fallback pass
         * in that case (also covers the storage_read() failure/empty case,
         * where got stays 0 < scan_sizes[pass]). */
        if (!ok && short_read) break;
    }
    if (!ok) return;

    double secs = (double)wav.data_bytes /
        ((double)wav.sample_rate * wav.channels * (wav.bits_per_sample / 8));
    if (secs >= 60.0) {
        snprintf(out, out_len, "%u:%02u", (unsigned)(secs / 60), (unsigned)secs % 60);
    } else {
        snprintf(out, out_len, "%.1fs", secs);
    }
    dur_cache_put(name, size, out);
}

/* Parses a recording filename in telephony.c's rec_name_timestamped()/
 * rec_name_voicemail() format -- "rec_YYYYMMDD_HHMMSSZ.wav" or
 * "voicemail_YYYYMMDD_HHMMSSZ.wav" once the system clock has synced via
 * SNTP -- into a readable "YYYY-MM-DD HH:MM UTC" timestamp. Before sync,
 * those functions instead name the file "rec_boot<ms>.wav"/
 * "voicemail_boot<ms>.wav" (no real date embedded, just a boot-relative tag
 * to avoid collisions); for that shape this reports "recorded before clock
 * sync" rather than trying to parse a date out of it. Blank output (not an
 * error) for anything matching neither shape -- a display nicety for the
 * Recordings page's "Recorded" column, not worth failing the row render
 * over. */
static void format_recording_timestamp(const char *name, char *out, size_t len)
{
    if (len == 0) return;
    out[0] = '\0';

    const char *p;
    if (strncmp(name, "rec_", 4) == 0) p = name + 4;
    else if (strncmp(name, "voicemail_", 10) == 0) p = name + 10;
    else return;

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
/* "rec_" as `name_prefix` is a sentinel for "any user recording" -- both
 * is_user_recording() prefixes ("rec_" and "voicemail_"), not a literal
 * strncmp -- so the Recordings page (the only caller that passes it, see
 * append_storage_table()'s own doc comment) shows voicemails alongside 211
 * archives without every other name_prefix caller needing to know that. */
static bool storage_table_matches(const char *name, const char *name_prefix)
{
    if (!name_prefix || !name_prefix[0]) return true;
    if (strcmp(name_prefix, "rec_") == 0) return is_user_recording(name);
    return strncmp(name, name_prefix, strlen(name_prefix)) == 0;
}

static int append_storage_table(char *buf, size_t cap, int off,
                                storage_entry_t *entries, size_t n,
                                const char *name_prefix, bool show_recorded,
                                const char *return_to)
{
    size_t shown = 0;
    for (size_t i = 0; i < n; i++) {
        if (!storage_table_matches(entries[i].name, name_prefix)) continue;
        shown++;
    }
    if (shown == 0) {
        off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
            "<div class=\"empty\">%s</div>", i18n_str("dialcodes.none_yet", "none yet"));
        return off;
    }
    char recorded_th[48] = "";
    if (show_recorded) {
        snprintf(recorded_th, sizeof(recorded_th), "<th>%s</th>",
                 i18n_str("storage.col_recorded", "Recorded"));
    }
    /* One <form> wraps the whole table so a header "select all" checkbox
     * and a "Delete selected" button can bulk-POST to /media_delete_bulk,
     * while each row's own delete button keeps working unchanged by
     * overriding the target via formaction/formmethod (HTML5 lets a
     * specific submit button redirect its own submission elsewhere without
     * needing a nested <form>, which isn't legal HTML). No id needed on
     * the form/table -- the header checkbox and the bulk button both find
     * their own table via closest(), so this is safe to render more than
     * once on the same page (Settings -> Storage's SD + internal tables). */
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<form method=\"POST\" action=\"/media_delete_bulk\">"
        "<input type=\"hidden\" name=\"return_to\" value=\"%s\">"
        "<table><tr><th><input type=\"checkbox\" title=\"%s\" "
        "onclick=\"this.closest('table').querySelectorAll('input[name=names]')."
        "forEach(function(c){c.checked=this.checked}, this)\"></th>"
        "<th>%s</th>%s<th>%s</th><th>%s</th><th>%s</th><th></th><th></th></tr>",
        return_to, i18n_str("storage.select_all", "Select all"),
        i18n_str("storage.col_name", "Name"), recorded_th,
        i18n_str("storage.col_size", "Size"), i18n_str("storage.col_length", "Length"),
        i18n_str("storage.col_play", "Play"));
    for (size_t i = 0; i < n && off > 0 && (size_t)off < cap; i++) {
        if (!storage_table_matches(entries[i].name, name_prefix)) continue;
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
            if (is_wav_name(entries[i].name)) wav_duration_str(entries[i].name, entries[i].size, dur, sizeof(dur));
            else dur[0] = '\0';
        } else {
            play[0] = '\0';
            dur[0] = '\0';
        }
        if (show_recorded) format_recording_timestamp(entries[i].name, recorded, sizeof(recorded));
        char recorded_td[48] = "";
        if (show_recorded) snprintf(recorded_td, sizeof(recorded_td), "<td>%s</td>", recorded);
        char delete_confirm[STORAGE_MAX_NAME + 16];
        snprintf(delete_confirm, sizeof(delete_confirm), i18n_str("storage.delete_confirm", "Delete %s?"),
                 entries[i].name);
        off += snprintf(buf + off, cap - off,
            "<tr><td><input type=\"checkbox\" name=\"names\" value=\"%s\"></td>"
            "<td><code>%s</code></td>%s<td>%s</td><td>%s</td><td>%s</td>"
            "<td><a href=\"/media/%s\">%s</a></td>"
            "<td><button type=\"submit\" formaction=\"/media_delete/%s\" formmethod=\"POST\" "
            "onclick=\"return confirm('%s')\">%s</button></td></tr>",
            entries[i].name, entries[i].name, recorded_td,
            fmt_bytes(entries[i].size, sz, sizeof(sz)), dur, play,
            entries[i].name, i18n_str("storage.download", "download"),
            entries[i].name, delete_confirm, i18n_str("storage.delete", "delete"));
    }
    /* "Delete selected" -- also doubles as "delete all" (check the header
     * box, then click this) rather than needing a second, separate button
     * and endpoint for that. Confirms with the actual count so there's no
     * doubt how many are about to go; refuses to submit with zero checked
     * rather than silently no-op'ing on the server. */
    char bulk_confirm_js[256];
    snprintf(bulk_confirm_js, sizeof(bulk_confirm_js),
        "var n=this.closest('table').querySelectorAll('input[name=names]:checked').length;"
        "if(!n){alert('%s');return false;}"
        "return confirm(n+' %s');",
        i18n_str("storage.bulk_none_selected", "No files selected."),
        i18n_str("storage.bulk_confirm_suffix", "file(s) will be deleted. Continue?"));
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<tr><td colspan=\"8\"><button type=\"submit\" onclick=\"%s\">%s</button></td></tr>",
        bulk_confirm_js, i18n_str("storage.delete_selected", "Delete selected"));
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0, "</table></form>");
    return off;
}

/* Last self-test result, shown on the Storage tab's card-info card (below)
 * so it doesn't just vanish into a plain-text response the moment either
 * self-test button is clicked. Lives here (not sdcard.c) since it's purely
 * a display concern -- see sdcard_selftest()'s own doc comment for why
 * this write/read/verify/speed round-trip is the practical stand-in for
 * the SMART-style health check SD cards don't actually expose. Not
 * persisted across reboot; harmless race if two requests land at once
 * (worst case, one's display update briefly loses to the other's), not
 * worth a lock for a status string only one admin is likely looking at at
 * a time. Set from test_sd_handler()/sd_selftest_ui_handler() below, via
 * their shared run_sd_selftest() helper. */
static char s_sd_selftest_result[96] = "";

/* JEDEC-assigned SD manufacturer IDs, the common ones -- see sdcard.h's
 * sdcard_info_t doc comment. Short/curated on purpose, same reasoning as
 * UI_LANGUAGES/UI_TIMEZONES above: an ID missing here just shows as its
 * raw hex value instead of a name, a safe, obvious gap rather than a wrong
 * guess. Not an official/complete JEDEC list -- assembled from what
 * publicly circulates in SD card identification tools and driver source. */
typedef struct { int id; const char *name; } sd_mfg_t;
static const sd_mfg_t SD_MANUFACTURERS[] = {
    { 0x01, "Panasonic" }, { 0x02, "Kioxia/Toshiba" }, { 0x03, "SanDisk" },
    { 0x1b, "Samsung" }, { 0x1d, "AData" }, { 0x27, "Phison" },
    { 0x28, "Lexar" }, { 0x31, "Silicon Power" }, { 0x41, "Kingston" },
    { 0x74, "Transcend" }, { 0x82, "Sony" },
};
#define SD_MANUFACTURERS_N (sizeof(SD_MANUFACTURERS) / sizeof(SD_MANUFACTURERS[0]))

static const char *sd_manufacturer_name(int mfg_id)
{
    for (size_t i = 0; i < SD_MANUFACTURERS_N; i++) {
        if (SD_MANUFACTURERS[i].id == mfg_id) return SD_MANUFACTURERS[i].name;
    }
    return NULL;
}

/* SD Association's own capacity tiers (not derived from the CSD version
 * field -- that only tells byte- vs. block-addressing, not which marketing
 * tier a card's label says -- capacity is what "SDHC"/"SDXC" actually mean
 * on a real card). Boundaries per the spec: SDSC <=2GB, SDHC <=32GB, SDXC
 * up to 2TB. */
static const char *sd_type_name(uint64_t bytes)
{
    if (bytes > 32ULL * 1024 * 1024 * 1024) return "SDXC";
    if (bytes > 2ULL * 1024 * 1024 * 1024) return "SDHC";
    return "SDSC";
}

/* microSD card identification + last self-test result + the Format button.
 * Read-only identification (name/manufacturer/type/speed) is always safe to
 * show; Format is real, irreversible data loss, gated by its own confirm()
 * -- same pattern as Factory Reset (Settings -> Hardware). */
static int append_sdcard_info_card(char *buf, size_t cap, int off)
{
    sdcard_info_t info;
    if (!sdcard_get_info(&info)) return off;

    const char *mfg = sd_manufacturer_name(info.mfg_id);
    char mfg_str[24];
    if (mfg) snprintf(mfg_str, sizeof(mfg_str), "%s", mfg);
    else snprintf(mfg_str, sizeof(mfg_str), "unknown (0x%02x)", info.mfg_id);

    char cap_str[16];
    fmt_bytes(info.capacity_bytes, cap_str, sizeof(cap_str));

    char selftest_row[200] = "";
    if (s_sd_selftest_result[0]) {
        snprintf(selftest_row, sizeof(selftest_row),
            "<div class=\"statusrow\"><span class=\"label\">%s</span><span>%s</span></div>",
            i18n_str("storage.sd_selftest_label", "Last self-test"), s_sd_selftest_result);
    }

    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<div class=\"card wide\" style=\"margin-top:16px\"><h2>%s</h2>"
        "<div class=\"statusrow\"><span class=\"label\">%s</span><span>%s</span></div>"
        "<div class=\"statusrow\"><span class=\"label\">%s</span><span>%s</span></div>"
        "<div class=\"statusrow\"><span class=\"label\">%s</span><span>%s, %s</span></div>"
        "<div class=\"statusrow\"><span class=\"label\">%s</span>"
        "<span>%d-bit @ %d MHz (rated up to %d MHz)</span></div>"
        "%s"
        "<div class=\"toolbar sub\">"
        "<form method=\"POST\" action=\"/sd_selftest_ui\" style=\"display:inline\">"
        "<button type=\"submit\">%s</button></form>"
        "<form method=\"POST\" action=\"/sd_format\" style=\"display:inline\" "
        "onsubmit=\"return confirm('%s')\">"
        "<button type=\"submit\" class=\"danger\">%s</button></form>"
        "</div>"
        "<p class=\"hint\">%s</p></div>",
        i18n_str("storage.sd_info_title", "microSD card"),
        i18n_str("storage.sd_product", "Product name"), info.name[0] ? info.name : "(blank)",
        i18n_str("storage.sd_manufacturer", "Manufacturer"), mfg_str,
        i18n_str("storage.sd_type", "Type"), sd_type_name(info.capacity_bytes), cap_str,
        i18n_str("storage.sd_bus", "Bus"),
        info.bus_width, info.real_freq_khz / 1000, info.max_freq_khz / 1000,
        selftest_row,
        i18n_str("storage.sd_selftest_button", "Run self-test"),
        i18n_str("storage.sd_format_confirm",
                 "Format the microSD card? This permanently erases EVERYTHING on it -- "
                 "recordings, uploads, all of it. This cannot be undone."),
        i18n_str("storage.sd_format_button", "Format card"),
        i18n_str("storage.sd_info_hint",
                 "Product name and manufacturer come from the card's own CID register -- not a "
                 "marketing name, just whatever the manufacturer programmed there. Type (SDSC/"
                 "SDHC/SDXC) is capacity-based per the SD Association's own tiers. Bus is always "
                 "1-bit here (SPI mode, not native SD 4-bit) -- expect a few MB/s even on a fast "
                 "card. Self-test writes, reads back, and verifies a 128KB file, timing both "
                 "directions -- SD cards have no portable SMART-style health check, so a card "
                 "that's failing or unusually slow tends to show up here as a mismatch or a low "
                 "throughput number instead."));
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

    /* storage.hint carries its own %s (the board's IP, for the curl
     * example) -- render into its own buffer first, same two-step pattern
     * used elsewhere in this file for a translated string with a
     * placeholder of its own. */
    char storage_hint[500];
    snprintf(storage_hint, sizeof(storage_hint),
        i18n_str("storage.hint",
                 "Recordings, uploaded prompt/clip WAVs, anything else. Upload with "
                 "<code>curl -X POST --data-binary @file.wav http://%s/media/&lt;name&gt;</code> "
                 "or <code>firmware/bringup/gen_prompt.py</code>. <code>operator.wav</code> and "
                 "<code>clip_*.wav</code> are never auto-deleted to make room for recordings -- "
                 "only explicit deletes (below) remove them. Prompt/clip uploads always go to "
                 "internal flash (so pulling the card mid-use can't take dial tone or the "
                 "operator message with it) -- only 211 recordings prefer the card."),
        ip);
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<div id=\"storage\" class=\"card wide\" style=\"margin-top:16px\"><h2>%s</h2>"
        "<p class=\"hint\">%s%s</p></div>",
        i18n_str("storage.title", "Storage"), storage_hint,
        sd ? i18n_str("storage.sd_present_hint",
                 " A microSD card is present and takes priority for new recordings -- internal "
                 "flash is never evicted to make room on the card, and stays listed below in "
                 "its own section so anything saved before the card was inserted is still "
                 "reachable.") : "");

    /* Configured for SD but nothing's mounted right now -- either a card
     * was never inserted, the last mount attempt failed (bad format, wiring,
     * a failing card), or it was just swapped in and sdcard_hotplug_start()
     * (sdcard.c) hasn't picked it up yet (a couple of seconds on boards with
     * a card-detect pin to watch). append_sdcard_info_card() itself renders
     * nothing at all in this state (sdcard_get_info() needs a mounted
     * card), so without this, a failed/pending mount is silently invisible
     * here -- this is what "show inserted in web UI" actually needed. */
    if (!sd && board_hw_config_get().has_microsd) {
        if (sdcard_needs_format()) {
            /* sdcard_needs_format() means the card talked fine over SPI --
             * this really is a wrong-filesystem problem (exFAT, a
             * multi-partition card like a Raspberry Pi OS image, or blank/
             * corrupted), not wiring -- so offer the thing that actually
             * fixes it instead of a plain retry, which would just fail the
             * same way again. */
            off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
                "<div class=\"card wide\" style=\"margin-top:16px\"><h2>%s</h2>"
                "<p class=\"hint warn-text\">%s</p>"
                "<div class=\"toolbar sub\">"
                "<form method=\"POST\" action=\"/sd_force_format\" style=\"display:inline\" "
                "onsubmit=\"return confirm('%s')\">"
                "<button type=\"submit\" class=\"danger\">%s</button></form>"
                "</div></div>",
                i18n_str("storage.sd_info_title", "microSD card"),
                i18n_str("storage.sd_wrong_fs_hint",
                         "Card detected and responding fine, but it isn't formatted as a plain "
                         "FAT32 filesystem this board can use -- exFAT, a multi-partition card "
                         "(e.g. a Raspberry Pi OS image), or something similar. This isn't a "
                         "wiring problem; reformatting is the fix."),
                i18n_str("storage.sd_force_format_confirm",
                         "Format this microSD card as FAT32? This permanently erases EVERYTHING "
                         "currently on it. This cannot be undone."),
                i18n_str("storage.sd_force_format_button", "Format as FAT32"));
        } else {
            off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
                "<div class=\"card wide\" style=\"margin-top:16px\"><h2>%s</h2>"
                "<p class=\"hint\">%s</p>"
                "<div class=\"toolbar sub\">"
                "<form method=\"POST\" action=\"/sd_selftest_ui\" style=\"display:inline\">"
                "<button type=\"submit\">%s</button></form>"
                "</div></div>",
                i18n_str("storage.sd_info_title", "microSD card"),
                i18n_str("storage.sd_not_mounted_hint",
                         "Configured for a microSD card, but none is currently mounted. If a "
                         "card is inserted, this usually means the last mount attempt failed "
                         "(wiring, or a failing card -- check the activity/boot log for the "
                         "exact error), or it was just inserted and hasn't been picked up yet -- "
                         "rechecked automatically every couple of seconds. Reload this page in a "
                         "moment, or force an immediate retry below."),
                i18n_str("storage.sd_retry_button", "Retry mount"));
        }
    }

    if (sd) {
        off = append_sdcard_info_card(buf, cap, off);
        storage_entry_t *sd_entries = malloc(STORAGE_LIST_MAX * sizeof(storage_entry_t));
        size_t sd_n = sd_entries ? storage_list_sd(sd_entries, STORAGE_LIST_MAX) : 0;
        char su[16], sc[16];
        off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
            "<div class=\"card wide\" style=\"margin-top:16px\"><h2>microSD (%u %s)</h2>"
            "<p class=\"hint\">%s / %s %s. %s</p>",
            (unsigned)sd_n, sd_n == 1 ? i18n_str("storage.file", "file") : i18n_str("storage.files", "files"),
            fmt_bytes(storage_used_bytes_sd(), su, sizeof(su)),
            fmt_bytes(storage_cap_bytes_sd(), sc, sizeof(sc)),
            i18n_str("storage.used", "used"),
            i18n_str("storage.sd_new_here", "New recordings and uploads go here."));
        off = append_storage_table(buf, cap, off, sd_entries, sd_n, NULL, false, "/settings?tab=storage");
        free(sd_entries);
        off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0, "</div>");
    }

    storage_entry_t *in_entries = malloc(STORAGE_LIST_MAX * sizeof(storage_entry_t));
    size_t in_n = in_entries ? storage_list_internal(in_entries, STORAGE_LIST_MAX) : 0;
    char isu[16], isc[16];
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<div class=\"card wide\" style=\"margin-top:16px\"><h2>%s (%u %s)</h2>"
        "<p class=\"hint\">%s / %s %s.%s</p>",
        sd ? i18n_str("storage.internal_flash", "Internal flash") : i18n_str("storage.title", "Storage files"),
        (unsigned)in_n, in_n == 1 ? i18n_str("storage.file", "file") : i18n_str("storage.files", "files"),
        fmt_bytes(storage_used_bytes_internal(), isu, sizeof(isu)),
        fmt_bytes(storage_cap_bytes_internal(), isc, sizeof(isc)),
        i18n_str("storage.used", "used"),
        sd ? "" : i18n_str("storage.capped_hint",
                 " Capped at this size -- oldest recording evicted first once it's hit "
                 "(uploaded clips/operator.wav are never auto-evicted)."));
    off = append_storage_table(buf, cap, off, in_entries, in_n, NULL, false, "/settings?tab=storage");
    free(in_entries);
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
     * not back to General (the unknown-tab fallback below). ("storage" is
     * its own tab again -- see SETTINGS_TABS's comment -- so it needs no
     * alias here anymore.) */
    if (strcmp(tab, "operator") == 0 || strcmp(tab, "record") == 0) {
        strncpy(tab, "dialcodes", sizeof(tab) - 1);
    }

    bool known_tab = false;
    for (size_t i = 0; i < SETTINGS_TABS_N; i++) {
        if (strcmp(SETTINGS_TABS[i].id, tab) == 0) { known_tab = true; break; }
    }
    if (!known_tab) strncpy(tab, "general", sizeof(tab));

    dialplan_entry_info_t entries[40];
    size_t n = dialplan_list(entries, 40);

    /* Sized for the busiest tab. Diagnostics needs room for the full log
     * panel (moved here from the Status page, see append_diag_tab()) --
     * each raw log line can roughly triple in size once wrapped in
     * .logline/.t/.k/.m markup. +3000 for the Tone generator + Mic record
     * test cards added on top of the original Logging/Test mode cards the
     * flat 6144 base below was sized for -- 2 lines' worth of mic-record
     * cards (each with its own form + conditional <audio> player row). */
    size_t log_room = (strcmp(tab, "diag") == 0) ? 8200 * 3 + 3000 : 0;
    /* Storage sizes off the REAL file count, not STORAGE_LIST_MAX's
     * worst case -- this used to just assume up to STORAGE_LIST_MAX files
     * on each backend (a flat ~68KB), which sounded conservative but
     * wasn't: once append_storage_table() grew a bulk-delete <form> (with
     * a real per-row cost of ~525 bytes -- see recordings_page_handler's
     * matching comment -- not the ~340 originally guessed), the same
     * worst-case math needed ~120KB, and a single malloc() that size
     * reliably 500'd this handler even with a small, ordinary real file
     * count. One heap-allocated STORAGE_LIST_MAX-sized scratch array,
     * reused sequentially for both backends purely to count, costs a
     * second SPIFFS/FATFS walk (append_storage_section() does its own
     * listing right after this) but keeps the allocation honest instead
     * of pessimistic -- and it's heap, not stack, so it doesn't reopen the
     * stack-overflow history STORAGE_LIST_MAX's own doc comment
     * describes. */
    size_t storage_room = 0;
    if (strcmp(tab, "storage") == 0) {
        storage_entry_t *count_tmp = malloc(STORAGE_LIST_MAX * sizeof(storage_entry_t));
        size_t sd_n = (count_tmp && storage_sd_present()) ? storage_list_sd(count_tmp, STORAGE_LIST_MAX) : 0;
        size_t in_n = count_tmp ? storage_list_internal(count_tmp, STORAGE_LIST_MAX) : 0;
        free(count_tmp);
        /* 600/row (measured worst case) + 2048 fixed per backend actually
         * present (each renders its own full bulk-delete <form>, ~746
         * bytes measured) -- not per possible backend, so a card-less
         * board (the common case) doesn't pay for the SD form it never
         * renders. SD's own fixed share is bumped to 4096 (was 2048) to
         * also cover append_sdcard_info_card() -- product/manufacturer/
         * type/bus rows, self-test result, and the self-test + format
         * buttons, on top of the bulk-delete form already accounted for.
         * +1200 covers the "configured for SD but not mounted" fallback
         * card (append_storage_section()) on a board that's configured for
         * a card but doesn't currently have one mounted -- sized for its
         * larger of two variants (sdcard_needs_format()'s force-format
         * confirm dialog + danger button, bigger than the plain retry
         * button the other variant renders). */
        storage_room = (sd_n + in_n) * 600 + (storage_sd_present() ? 4096 : 0) + 2048 +
            (!storage_sd_present() && board_hw_config_get().has_microsd ? 1200 : 0);
    }
    /* Hardware tab lost its heaviest content (speaker volume/echo/answering
     * machine, all per-line cards -- see append_phone_settings_tab()) to
     * the Phone Settings tab, but keeps the memory map card and several
     * partition rows, so this stays generous rather than being re-tuned
     * down to the byte. General's room covers the WIFI_SCAN_JS on its WiFi
     * card. Phone Settings inherits the per-line-card weight hw_room used
     * to carry, doubled on a 2-line board, plus Echo Cancellation's and
     * Answering Machine's own <script> blocks. */
    size_t hw_room = (strcmp(tab, "hardware") == 0)
        ? 6000 + (board_type_mismatch() ? 900 : 0) /* +memory map card, several partition rows,
                                                      * +the mismatch-confirm card if showing */
        : 0;
    size_t general_room = (strcmp(tab, "general") == 0) ? 3200 : 0; /* +quiet hours card's timezone dropdown */
    size_t phone_room = (strcmp(tab, "phone") == 0) ? 10000 : 0; /* +2x per-line speaker/echo/AM cards, 2 <script> blocks, AM inactive-hint, tone-region card */
    size_t voip_room = (strcmp(tab, "voip") == 0) ? 6500 : 0; /* +copy-paste config examples, FreePBX + remote-deployment sections, enable-VOIP card */
    /* This tab's `n*220` share (n = ALL dialplan entries, not just clips)
     * was never actually enough for it: each dial-code row is TWO forms
     * (recode + description) plus an audio player -- measured at up to
     * ~950 bytes/row with a full 63-char description and long filename,
     * not 220 -- and on top of that this tab also renders the Setup
     * Password card and the Operator/Record prompt-text cards (each with
     * a gen_prompt.py --sync command string once a password is set),
     * none of which the shared base accounted for at all. Confirmed
     * truncating real device output mid-textarea, well before the Save
     * button or the generate-command hint, on a board with only 5 codes
     * -- not a rare worst case. DIALPLAN_MAX_CLIP_ENTRIES (24) is small
     * and NVS-backed (not a filesystem walk), so sizing for that many
     * outright is cheap and avoids a second listing call just to count. */
    size_t dialcodes_room = (strcmp(tab, "dialcodes") == 0) ? DIALPLAN_MAX_CLIP_ENTRIES * 950 + 6000 : 0;
    size_t cap = 6144 + n * 220 + storage_room + log_room + hw_room + general_room + voip_room + dialcodes_room + phone_room
        + (board_type_mismatch() ? 700 : 0); /* append_page_head()'s board-type hazard banner */
    char *buf = malloc(cap);
    if (!buf) { httpd_resp_send_500(req); return ESP_FAIL; }

    int off = append_page_head(buf, cap, 0, i18n_str("nav.settings", "Settings"), "/settings", false);
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "</header><main class=\"settings-main\">");
    off = append_settings_tabs(buf, cap, off, tab);
    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0,
        "<script>%s</script><div class=\"cards\">", FEATURE_TOGGLE_JS);

    if (strcmp(tab, "general") == 0) off = append_general_tab(buf, cap, off);
    else if (strcmp(tab, "phone") == 0) off = append_phone_settings_tab(buf, cap, off);
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
    else if (strcmp(tab, "bluetooth") == 0) off = append_bluetooth_tab(buf, cap, off);
    else if (strcmp(tab, "diag") == 0) off = append_diag_tab(buf, cap, off);
    /* Storage: the full SD+internal file manager, kept separate from
     * Hardware -- see SETTINGS_TABS's comment for why (its per-file
     * WAV-duration parsing is the slowest single thing in Settings, and
     * bundling it with anything else means paying that cost on every visit
     * to the other tab too). */
    else if (strcmp(tab, "storage") == 0) off = append_storage_section(buf, cap, off);
    /* Hardware: stats + the SMD line2/microSD config card -- everything
     * else that landed here (Answering Machine, Speaker, Echo, idle-power)
     * is cheap NVS-backed config, not file I/O. */
    else {
        off = append_hw_config_tab(buf, cap, off);
        off = append_hardware_tab(buf, cap, off);
        off = append_memory_map_card(buf, cap, off);
    }

    off += snprintf(buf + off, ((size_t)off < cap) ? cap - off : 0, "</div>" /* .cards */);

    if (off > 0 && (size_t)off < cap) {
        off += snprintf(buf + off, cap - off, "</main></body></html>");
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, buf, clamp_send_len(off, cap));
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

/* GET /api/wifi_scan -- nearby SSIDs for the setup/settings forms' dropdown,
 * as {"networks":[{"ssid":"...","rssi":-42,"secured":true}, ...]}. Works
 * during provisioning too (the setup AP runs AP+STA so scanning still
 * works -- see wifi_scan()'s doc comment). A failed scan reports an empty
 * list plus "error":true rather than an HTTP error, so the page's JS can
 * just fall back to manual entry. */
#define WIFI_SCAN_MAX 24

static esp_err_t wifi_scan_json_handler(httpd_req_t *req)
{
    wifi_scan_result_t results[WIFI_SCAN_MAX];
    int n = wifi_scan(results, WIFI_SCAN_MAX);

    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(root, "networks");
    for (int i = 0; i < n; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "ssid", results[i].ssid);
        cJSON_AddNumberToObject(item, "rssi", results[i].rssi);
        cJSON_AddBoolToObject(item, "secured", results[i].secured);
        cJSON_AddItemToArray(arr, item);
    }
    if (n < 0) cJSON_AddBoolToObject(root, "error", true);

    char *text = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, text ? text : "{\"networks\":[]}");
    cJSON_Delete(root);
    free(text);
    return ESP_OK;
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
        /* Name the actual address it'll try, rather than a vague "its .local
         * name" -- the no-instructions gap this used to leave was confusing
         * on first setup, before a board name is even chosen. Still hedge:
         * wifi_apply_hostname() bumps this to <host>1.local etc. if another
         * board already answers for it, and .local resolution itself isn't
         * fully reliable on every OS/router/network even when nothing's
         * wrong -- give the router's client list as a same-weight fallback,
         * not a last resort, and say what to actually look for there
         * (wifi_init() now sets this same name as the DHCP hostname, so it's
         * not just a MAC address in that list). */
        char host[64];
        board_hostname(host, sizeof(host));
        char page[1500];
        snprintf(page, sizeof(page),
            "<!doctype html><meta charset=\"utf-8\">"
            "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
            "<link rel=\"icon\" href=\"/favicon.ico\" sizes=\"any\">"
            "<link rel=\"icon\" type=\"image/svg+xml\" href=\"/static/favicon.svg\">"
            "<link rel=\"stylesheet\" href=\"/static/style.css\">"
            "<main class=\"auth-page\"><div class=\"auth-card\">"
            "<h1>Saved \xe2\x80\x94 connecting\xe2\x80\xa6</h1>"
            "<p class=\"hint\">The phone is rebooting to join your network. This setup "
            "network will disappear -- reconnect your phone/laptop to your normal WiFi, "
            "then browse to:</p>"
            "<p class=\"hint\"><code>http://%s.local</code></p>"
            "<p class=\"hint\">.local addresses don't work reliably on every network -- if "
            "that doesn't load within a minute or two, don't wait it out: open your WiFi "
            "router's admin page and look at its list of connected devices instead. This "
            "board will show up there as <code>%s</code> (or <code>%s1</code>, <code>%s2</code> "
            "etc. if another board already answered to that name) -- open its IP address "
            "directly. This works every time, .local or not, so it's worth trying first if "
            "you have more than one of these boards. Rename this board once you're in, under "
            "Settings \xe2\x86\x92 General, so both its .local address and its router listing "
            "stay easy to tell apart from any others.</p></div></main>",
            host, host, host, host);
        httpd_resp_set_type(req, "text/html");
        httpd_resp_sendstr(req, page);
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

/* 303-redirects back to whichever page submitted this form (the Referer
 * header) if one was sent, else `fallback`. Several settings forms now
 * appear on more than one page -- e.g. board name and hardware type appear
 * both on their normal Settings tab AND on the Status page's first-boot
 * banner (append_first_boot_banner()) -- so a fixed redirect target would
 * always bounce back to Settings, forcing a trip back to Status to fill in
 * whichever field wasn't done yet. `fallback` only matters for direct
 * API-style POSTs with no Referer at all (e.g. a script), not real browser
 * form submissions. */
static void redirect_to_referer_or(httpd_req_t *req, const char *fallback)
{
    char referer[128];
    httpd_resp_set_status(req, "303 See Other");
    if (httpd_req_get_hdr_value_str(req, "Referer", referer, sizeof(referer)) == ESP_OK) {
        httpd_resp_set_hdr(req, "Location", referer);
    } else {
        httpd_resp_set_hdr(req, "Location", fallback);
    }
    httpd_resp_send(req, NULL, 0);
}

/* Shared by /wifi_reprovision and /factory_reset: a short confirmation page,
 * then reboot once the response has flushed (reboot_task, above). */
static esp_err_t send_reboot_page(httpd_req_t *req, const char *title, const char *body_html)
{
    char page[900];
    snprintf(page, sizeof(page),
        "<!doctype html><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
        "<link rel=\"icon\" href=\"/favicon.ico\" sizes=\"any\">"
        "<link rel=\"icon\" type=\"image/svg+xml\" href=\"/static/favicon.svg\">"
        "<link rel=\"stylesheet\" href=\"/static/style.css\">"
        "<main class=\"auth-page\"><div class=\"auth-card\">"
        "<h1>%s</h1>%s</div></main>",
        title, body_html);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, page);
    xTaskCreate(reboot_task, "reboot", 2048, NULL, tskIDLE_PRIORITY + 1, NULL);
    return ESP_OK;
}

/* POST /wifi_reprovision -- clears only the saved WiFi credentials and
 * reboots into setup-AP mode. Every other setting (board name, dial codes,
 * VoIP, idle-power, uploaded audio/recordings) is untouched. For moving the
 * board to a different WiFi network, or re-testing the setup flow, without
 * starting over on everything else. */
static esp_err_t wifi_reprovision_handler(httpd_req_t *req)
{
    ESP_LOGW(TAG, "web UI: re-provision WiFi requested");
    esp_err_t err = wifi_clear_credentials();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wifi_clear_credentials failed: %s", esp_err_to_name(err));
    }
    return send_reboot_page(req, "WiFi cleared \xe2\x80\x94 rebooting\xe2\x80\xa6",
        "<p class=\"hint\">Every other setting is untouched. In a few seconds, look for "
        "the <code>AnalogRedial-XXXX</code> setup network to reconnect it.</p>");
}

/* POST /factory_reset -- erases the ENTIRE NVS partition (WiFi, board
 * name/hw config, dial codes, prompts, VoIP, idle-power, log level -- every
 * setting this firmware persists) and reboots to a genuinely out-of-box
 * state. Deliberately does NOT touch uploaded audio or recordings -- those
 * live as files on internal flash/SPIFFS or microSD, not NVS settings; clear
 * those individually from Settings -> Storage first if handing the board to
 * someone else. */
static esp_err_t factory_reset_handler(httpd_req_t *req)
{
    ESP_LOGW(TAG, "web UI: factory reset requested -- erasing all settings");
    esp_err_t err = nvs_flash_erase();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_flash_erase failed: %s", esp_err_to_name(err));
    }
    return send_reboot_page(req, "Factory reset \xe2\x80\x94 rebooting\xe2\x80\xa6",
        "<p class=\"hint\">All settings are erased -- WiFi, board name, dial codes, VoIP, "
        "everything. Uploaded audio and recordings are untouched. In a few seconds, look "
        "for the <code>AnalogRedial-XXXX</code> setup network.</p>");
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

    /* Also appears on the Status page's first-boot banner
     * (append_first_boot_banner()), not just Settings -> General -- bounce
     * back to wherever it was actually submitted from, so confirming this
     * from the banner returns to Status (where the hardware-type field
     * might still be waiting) instead of yanking to Settings. */
    redirect_to_referer_or(req, "/settings?tab=general");
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

    /* Reachable from the header picker on every page, not just Settings ->
     * General, so bounce back to wherever it was actually used. */
    redirect_to_referer_or(req, "/settings?tab=general");
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

/* POST /tone_lang_set -- form body [enabled=1]. Unchecked checkbox submits
 * nothing, same convention as offhook_alert_handler above. */
static esp_err_t tone_lang_set_handler(httpd_req_t *req)
{
    char body[64] = {0};
    if (req->content_len > 0 && req->content_len < sizeof(body)) {
        int r = httpd_req_recv(req, body, req->content_len);
        if (r <= 0) { httpd_resp_send_500(req); return ESP_FAIL; }
        body[r] = '\0';
    }
    char enabled[8] = {0};
    form_field(body, "enabled", enabled, sizeof(enabled));
    board_tone_follow_language_set(enabled[0] == '1');

    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/settings?tab=phone");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* POST /timezone_set -- form body tz=... (a POSIX TZ string; blank = UTC). */
static esp_err_t timezone_set_handler(httpd_req_t *req)
{
    if (req->content_len >= WIFI_FORM_MAX_BODY) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad form body");
        return ESP_OK;
    }
    char body[WIFI_FORM_MAX_BODY] = {0};
    if (req->content_len > 0) {
        int r = httpd_req_recv(req, body, req->content_len);
        if (r <= 0) { httpd_resp_send_500(req); return ESP_FAIL; }
        body[r] = '\0';
    }

    char tz[BOARD_TZ_MAX] = {0};
    form_field(body, "tz", tz, sizeof(tz));
    board_timezone_set(tz); /* only fails on an NVS write error, nothing form-validatable */

    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/settings?tab=general");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* POST /quiet_hours -- form body [enabled=1&]start=HH:MM&end=HH:MM (the
 * values an <input type="time"> submits). An unchecked checkbox submits
 * nothing at all, same as offhook_alert_handler above -- missing `enabled`
 * means "off". */
static bool parse_hhmm(const char *s, uint8_t *h, uint8_t *m)
{
    unsigned hh, mm;
    if (sscanf(s, "%u:%u", &hh, &mm) != 2 || hh > 23 || mm > 59) return false;
    *h = (uint8_t)hh;
    *m = (uint8_t)mm;
    return true;
}

static esp_err_t quiet_hours_handler(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len >= WIFI_FORM_MAX_BODY) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad form body");
        return ESP_OK;
    }
    char body[WIFI_FORM_MAX_BODY];
    int r = httpd_req_recv(req, body, req->content_len);
    if (r <= 0) { httpd_resp_send_500(req); return ESP_FAIL; }
    body[r] = '\0';

    char enabled[8] = {0}, start[8] = {0}, end[8] = {0}, tz[BOARD_TZ_MAX] = {0};
    form_field(body, "enabled", enabled, sizeof(enabled));
    form_field(body, "start", start, sizeof(start));
    form_field(body, "end", end, sizeof(end));
    /* Timezone now lives in this same form (see append_general_tab's Quiet
     * Hours card) -- applied here too rather than needing its own submit.
     * Only fails on an NVS write error, nothing form-validatable, same as
     * timezone_set_handler(). */
    if (form_field(body, "tz", tz, sizeof(tz))) board_timezone_set(tz);

    board_quiet_hours_t cfg = { .enabled = (enabled[0] == '1') };
    if (!parse_hhmm(start, &cfg.start_hour, &cfg.start_min) ||
        !parse_hhmm(end, &cfg.end_hour, &cfg.end_min) ||
        board_quiet_hours_set(cfg) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad start/end time");
        return ESP_OK;
    }

    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/settings?tab=general");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* POST /voip_enable -- form body enabled=1 (checkbox present) or nothing at
 * all (unchecked, with no other field on this form -- unlike idle_power/
 * quiet_hours/offhook_alert below, an all-off submit here is a genuinely
 * empty body, not something to reject as malformed). */
static esp_err_t voip_enable_handler(httpd_req_t *req)
{
    char body[64] = {0};
    if (req->content_len > 0 && req->content_len < sizeof(body)) {
        int r = httpd_req_recv(req, body, req->content_len);
        if (r <= 0) { httpd_resp_send_500(req); return ESP_FAIL; }
        body[r] = '\0';
    }
    char enabled[8] = {0};
    form_field(body, "enabled", enabled, sizeof(enabled));
    voip_set_enabled(enabled[0] == '1');

    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/settings?tab=voip");
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

    char enabled[8] = {0}, minutes[12] = {0}, max_savings[8] = {0};
    form_field(body, "enabled", enabled, sizeof(enabled));
    form_field(body, "minutes", minutes, sizeof(minutes));
    form_field(body, "max_savings", max_savings, sizeof(max_savings));

    idle_power_cfg_t cfg = {
        .enabled = (enabled[0] == '1'),
        .minutes = minutes[0] ? (uint16_t)strtoul(minutes, NULL, 10) : 0,
        .max_savings = (max_savings[0] == '1'),
    };
    esp_err_t err = idle_power_set(cfg);
    if (err != ESP_OK) {
        /* idle_power_set() only ever returns ESP_ERR_INVALID_ARG for a
         * genuinely out-of-range value -- anything else (an NVS write
         * failure, say) got mislabeled "minutes out of range" here before,
         * which sent someone chasing an off-by-one in a form field that was
         * never the actual problem. Log what really happened; only claim
         * it's the range specifically when it is. */
        ESP_LOGW(TAG, "idle_power_set failed: %s (minutes='%s' -> %u)",
                 esp_err_to_name(err), minutes, (unsigned)cfg.minutes);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
            err == ESP_ERR_INVALID_ARG ? "minutes out of range" : "failed to save (see device log)");
        return ESP_OK;
    }

    httpd_resp_set_status(req, "303 See Other");
    /* This card actually lives on the Hardware tab (see
     * append_hardware_tab()) -- fixing a stale redirect left over from
     * before it moved there from General. */
    httpd_resp_set_hdr(req, "Location", "/settings?tab=hardware");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* POST /answering_machine -- form body line=N, enabled=1 (checkbox),
 * rings=N, max_secs=N, other_line=1 (checkbox, 2-line boards only). */
static esp_err_t answering_machine_handler(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len >= WIFI_FORM_MAX_BODY) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad form body");
        return ESP_OK;
    }
    char body[WIFI_FORM_MAX_BODY];
    int r = httpd_req_recv(req, body, req->content_len);
    if (r <= 0) { httpd_resp_send_500(req); return ESP_FAIL; }
    body[r] = '\0';

    char lstr[4] = {0}, enabled[8] = {0}, rings[8] = {0}, max_secs[12] = {0}, other[8] = {0};
    form_field(body, "line", lstr, sizeof(lstr));
    form_field(body, "enabled", enabled, sizeof(enabled));
    form_field(body, "rings", rings, sizeof(rings));
    form_field(body, "max_secs", max_secs, sizeof(max_secs));
    form_field(body, "other_line", other, sizeof(other));

    int line = lstr[0] ? atoi(lstr) : -1;
    board_am_cfg_t cfg = {
        .enabled = (enabled[0] == '1'),
        .rings = rings[0] ? (uint8_t)strtoul(rings, NULL, 10) : 0,
        .max_ms = max_secs[0] ? (uint32_t)strtoul(max_secs, NULL, 10) * 1000 : 0,
        .other_line = (other[0] == '1'),
    };
    if (line < 0 || line >= 2 || board_am_set(line, cfg) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad line or values out of range");
        return ESP_OK;
    }

    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/settings?tab=hardware");
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

    /* "lines" is the GPIO18-21 either/or; "bt" is independent of it. Accepts
     * the field under its old name "variant" too, so the first-boot banner's
     * form (and any bookmarked POST) keeps working. */
    char variant[16] = {0}, bt[4] = {0};
    form_field(body, "lines", variant, sizeof(variant));
    if (!variant[0]) form_field(body, "variant", variant, sizeof(variant));
    form_field(body, "bt", bt, sizeof(bt));

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
    /* A form that doesn't carry the field at all (the first-boot banner)
     * mustn't silently clear a module someone already declared fitted. */
    cfg.has_bt_module = bt[0] ? (bt[0] == '1') : board_hw_config_get().has_bt_module;
    board_hw_config_set(cfg);

    /* Also appears on the Status page's first-boot banner -- see
     * board_name_handler's matching comment just above. */
    redirect_to_referer_or(req, "/settings?tab=hardware");
    return ESP_OK;
}

/* POST /board_type_confirm -- records the currently-running binary's
 * compile-time board type (board_pcb_variant_name()) as correct for this
 * physical board, clearing board_type_mismatch(). Only reachable when a
 * mismatch is already showing (the Hardware tab only renders this card
 * then) -- the confirm() dialog on the form itself is the only real guard,
 * same pattern as /sd_format. */
static esp_err_t board_type_confirm_handler(httpd_req_t *req)
{
    esp_err_t err = board_type_confirm();
    if (err == ESP_OK) {
        activity_log("Board type confirmed after a mismatch");
    } else {
        ESP_LOGW(TAG, "board_type_confirm failed: %s", esp_err_to_name(err));
    }
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/settings?tab=hardware");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* POST /bt_enable_set -- form body [enabled=1]. Master switch for the
 * Bluetooth companion link (bt_companion_set_enabled()) -- unlike VOIP's
 * equivalent, this takes effect immediately (starts/stops the UART task
 * right away), so no "reboot to apply" wording here. */
static esp_err_t bt_enable_set_handler(httpd_req_t *req)
{
    if (req->content_len >= WIFI_FORM_MAX_BODY) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad form body");
        return ESP_OK;
    }
    char body[WIFI_FORM_MAX_BODY] = {0};
    if (req->content_len > 0) {
        int r = httpd_req_recv(req, body, req->content_len);
        if (r <= 0) { httpd_resp_send_500(req); return ESP_FAIL; }
        body[r] = '\0';
    }
    char enabled[8] = {0};
    form_field(body, "enabled", enabled, sizeof(enabled));
    bt_companion_set_enabled(enabled[0] == '1');

    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/settings?tab=bluetooth");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* POST /bt_pair -- no form body. Tells the companion board to enter
 * pairing mode; a no-op 400 if it's not enabled/linked (button is
 * server-rendered disabled in that case too, see append_bluetooth_tab(),
 * but a direct POST could still reach here). */
static esp_err_t bt_pair_handler(httpd_req_t *req)
{
    esp_err_t err = bt_companion_start_pairing();
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "companion board not connected");
        return ESP_OK;
    }
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/settings?tab=bluetooth");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* POST /bt_config_check -- no form body. Checks (and repairs) the BM64's
 * EEPROM bytes needed for analog audio out. Deliberately synchronous even
 * though it takes ~15 seconds: it's a rare maintenance action, and the
 * report it leaves behind is what the page renders after the redirect
 * (bt_companion_config_last_report()). Bluetooth drops for the duration --
 * IBDK mode involves two module resets -- so any paired phone disconnects
 * and has to reconnect afterward. */
static esp_err_t bt_config_check_handler(httpd_req_t *req)
{
    esp_err_t err = bt_companion_config_check(true, NULL);
    if (err == ESP_ERR_NOT_SUPPORTED) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "this board has no GPIO control of the BM64 mode pins");
        return ESP_OK;
    }
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/settings?tab=bluetooth");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* ---- hardware bring-up check (selftest.h) ----
 * All five are plain form POSTs that redirect back to the Diagnostics tab, so
 * the walkthrough works without JavaScript and each step's result is visible
 * in the re-rendered table. */

static esp_err_t selftest_begin_handler(httpd_req_t *req)
{
    selftest_begin();
    redirect_to_referer_or(req, "/settings?tab=diag");
    return ESP_OK;
}

/* Reads the "id" form field into out. Returns false if absent/too long. */
static bool selftest_form_id(httpd_req_t *req, char *out, size_t out_len, char *body, size_t body_cap)
{
    if (req->content_len == 0 || req->content_len >= body_cap) return false;
    int r = httpd_req_recv(req, body, req->content_len);
    if (r <= 0) return false;
    body[r] = '\0';
    form_field(body, "id", out, out_len);
    return out[0] != '\0';
}

/* Blocks up to 15s waiting for the physical event (a hook change), which is
 * within httpd's socket timeout. */
static esp_err_t selftest_action_handler(httpd_req_t *req)
{
    char body[WIFI_FORM_MAX_BODY], id[SELFTEST_ID_MAX] = {0};
    if (!selftest_form_id(req, id, sizeof(id), body, sizeof(body))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing id");
        return ESP_OK;
    }
    selftest_run_action(id, 15000);
    redirect_to_referer_or(req, "/settings?tab=diag");
    return ESP_OK;
}

static esp_err_t selftest_stimulus_handler(httpd_req_t *req)
{
    char body[WIFI_FORM_MAX_BODY], id[SELFTEST_ID_MAX] = {0};
    if (!selftest_form_id(req, id, sizeof(id), body, sizeof(body))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing id");
        return ESP_OK;
    }
    selftest_stimulus(id);
    redirect_to_referer_or(req, "/settings?tab=diag");
    return ESP_OK;
}

static esp_err_t selftest_answer_handler(httpd_req_t *req)
{
    char body[WIFI_FORM_MAX_BODY], id[SELFTEST_ID_MAX] = {0}, ok[4] = {0};
    if (req->content_len == 0 || req->content_len >= sizeof(body)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad form body");
        return ESP_OK;
    }
    int r = httpd_req_recv(req, body, req->content_len);
    if (r <= 0) { httpd_resp_send_500(req); return ESP_FAIL; }
    body[r] = '\0';
    form_field(body, "id", id, sizeof(id));
    form_field(body, "ok", ok, sizeof(ok));
    if (id[0]) selftest_answer(id, ok[0] == '1');
    redirect_to_referer_or(req, "/settings?tab=diag");
    return ESP_OK;
}

static esp_err_t selftest_save_handler(httpd_req_t *req)
{
    selftest_history_append();
    redirect_to_referer_or(req, "/settings?tab=diag");
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

/* POST /voip_check -- form body number=..&server=.. . `server` comes from
 * the Check form's hidden field, kept in sync with whatever's currently
 * typed in the Save form's server field (voipSyncCheckServer() in
 * append_voip_tab()) -- so typing a new server and checking a number
 * against it doesn't require saving that server first. Falls back to the
 * currently-saved voip_server() if `server` arrives empty (no JS). Uses
 * voip_probe_number()'s best-effort SIP OPTIONS probe (blocks up to ~2s),
 * which can only ever confirm "free", never "taken" for sure -- see that
 * function's own doc comment. Redirects back to the VOIP tab with the
 * result in the query string, same request/redirect/re-render pattern as
 * every other form on this page -- no
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

    char number[8] = {0}, server_buf[64] = {0};
    form_field(body, "number", number, sizeof(number));
    form_field(body, "server", server_buf, sizeof(server_buf));
    const char *server = server_buf[0] ? server_buf : voip_server();

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

/* POST /test/tone  body line=N&hz=N&ms=N -- play a single-frequency burst
 * out that line's earpiece (audio_tx_burst(), the same call bringup.c's
 * console `tone` command and the off-hook alert/AEC bring-up sweeps use --
 * see append_diag_tab()'s doc comment). Amplitude fixed at 0.6, matching
 * cmd_tone()'s own default -- not exposed as a field, one less way to
 * accidentally drive the amp too hard from a web form. Clamped server-side
 * (not just via the <input min/max>, which a direct POST bypasses) since
 * this actually drives real hardware for real time. */
static esp_err_t test_tone_handler(httpd_req_t *req)
{
    char body[64], line_s[8] = {0}, hz_s[16] = {0}, ms_s[16] = {0};
    if (!read_form_body(req, body, sizeof(body))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body"); return ESP_OK;
    }
    form_field(body, "line", line_s, sizeof(line_s));
    form_field(body, "hz", hz_s, sizeof(hz_s));
    form_field(body, "ms", ms_s, sizeof(ms_s));

    int line = line_s[0] ? (int)strtol(line_s, NULL, 10) : 0;
    float hz = hz_s[0] ? strtof(hz_s, NULL) : 1000.0f;
    long ms = ms_s[0] ? strtol(ms_s, NULL, 10) : 1000;
    if (line < 0 || line >= board_hw_config_get().num_lines) line = 0;
    if (hz < 20.0f) hz = 20.0f;
    if (hz > 20000.0f) hz = 20000.0f;
    if (ms < 50) ms = 50;
    if (ms > 5000) ms = 5000;

    audio_tx_burst(line, hz, 0.0f, 0.6f, (uint32_t)ms);
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/settings?tab=diag");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* Defined further down (used there for upload conversion); forward-declared
 * here so test_mic_record_handler() can reuse the exact same WAV-header
 * logic instead of a third copy of it. */
static void wr_u32le(uint8_t *p, uint32_t v);
static void build_wav_header(uint8_t *h, uint32_t rate, uint32_t data_bytes);

/* POST /test/mic_record  body line=N&secs=N -- captures `secs` seconds of
 * raw uplink PCM (audio_rx_read(), engine rate) to "diag_mic_test_l<N>.wav"
 * via the same write-header/patch-sizes-after pattern used for uploads
 * above (build_wav_header()/wr_u32le()) -- blocks the request for the
 * capture duration, same tradeoff AEC's Measure button already makes for a
 * ~2s blocking read; this just has a longer, explicit, user-chosen cap
 * (8s) instead of a fixed short one. 4KB read chunks so a slow/failed
 * storage write can't build up an unbounded in-RAM buffer for a card-free
 * board's larger internal-flash writes. */
static esp_err_t test_mic_record_handler(httpd_req_t *req)
{
    char body[64], line_s[8] = {0}, secs_s[8] = {0};
    if (!read_form_body(req, body, sizeof(body))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body"); return ESP_OK;
    }
    form_field(body, "line", line_s, sizeof(line_s));
    form_field(body, "secs", secs_s, sizeof(secs_s));

    int line = line_s[0] ? (int)strtol(line_s, NULL, 10) : 0;
    long secs = secs_s[0] ? strtol(secs_s, NULL, 10) : 3;
    if (line < 0 || line >= board_hw_config_get().num_lines) line = 0;
    if (secs < 1) secs = 1;
    if (secs > 8) secs = 8;

    uint32_t rate = audio_sample_rate();
    uint32_t total_samples = (uint32_t)secs * rate;
    char fname[32];
    snprintf(fname, sizeof(fname), "diag_mic_test_l%d.wav", line);

    storage_writer_t w;
    if (storage_write_begin_local(fname, 44 + (size_t)total_samples * 2, &w) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "couldn't open storage for writing");
        return ESP_OK;
    }
    uint8_t hdr[44];
    build_wav_header(hdr, rate, total_samples * 2);
    storage_write_chunk(&w, hdr, sizeof(hdr));

    int16_t chunk[2048];
    uint32_t remaining = total_samples;
    while (remaining > 0) {
        size_t want = remaining < (sizeof(chunk) / sizeof(chunk[0])) ? remaining : (sizeof(chunk) / sizeof(chunk[0]));
        size_t got = audio_rx_read(line, chunk, want, 1000);
        if (got == 0) break; /* timed out -- write whatever we already have rather than hang */
        storage_write_chunk(&w, chunk, got * 2);
        remaining -= (uint32_t)got;
    }
    storage_write_end(&w);

    uint32_t actual_samples = total_samples - remaining;
    uint8_t sz[4];
    wr_u32le(sz, 36 + actual_samples * 2); storage_patch(fname, 4, sz, 4);
    wr_u32le(sz, actual_samples * 2);      storage_patch(fname, 40, sz, 4);

    activity_log("Line %d: mic test recording (%lds)", line, secs);
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

/* POST /test/record  body ms=N & line=N -- starts an ordinary rec_* mic
 * (uplink) capture, the same recorder_start() the answering machine uses,
 * visible on the Recordings tab when done. Added for testing the Bluetooth
 * companion's analog bridge (see archive/BLUETOOTH_AUDIO_UPGRADE.md):
 * whatever a paired phone sends over HFP downlink and the ESP32 drives onto
 * the line via its DAC lands at the same SLIC tap this line's own mic input
 * reads from, so this is a way to capture that without a physical handset
 * or fighting the bringup console's serial flakiness (see
 * firmware/README.md's bring-up notes) over USB. Requires test mode on,
 * same as /test/inject. */
static esp_err_t test_record_handler(httpd_req_t *req)
{
    char body[64], ms_s[8] = {0}, line_s[8] = {0};
    if (!read_form_body(req, body, sizeof(body))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body"); return ESP_OK;
    }
    if (!telephony_test_enabled()) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "test mode is off (POST /test/mode on=1 first)");
        return ESP_OK;
    }
    form_field(body, "ms", ms_s, sizeof(ms_s));
    form_field(body, "line", line_s, sizeof(line_s));
    int ms = ms_s[0] ? atoi(ms_s) : 5000;
    int line = line_s[0] ? atoi(line_s) : 0;
    if (ms <= 0 || ms > 60000 || line < 0 || line > 1) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad ms (1-60000) or line (0-1)"); return ESP_OK;
    }
    char name[STORAGE_MAX_NAME];
    /* Spelled "line%d", not "l%d" -- the old "rec_l0_..." shape elsewhere
     * in the codebase reads as the digits "10" at a glance, not "line 0"
     * (see telephony.c's rec_name_timestamped()); this diagnostic-only
     * endpoint keeps a line disambiguator (unlike the user-facing 211/
     * voicemail names, it's genuinely useful here since either line can be
     * captured on demand), just spelled unambiguously. */
    snprintf(name, sizeof(name), "rec_test_line%d_%lu.wav", line,
             (unsigned long)(esp_timer_get_time() / 1000000));
    esp_err_t err = recorder_start(line, name, (uint32_t)ms, 0, NULL, NULL);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, esp_err_to_name(err)); return ESP_OK;
    }
    char resp[STORAGE_MAX_NAME + 16];
    snprintf(resp, sizeof(resp), "started %s\n", name);
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

/* POST /test/sd -- run the microSD write/read/verify self-test, mounting
 * first if needed. Plain-text result the DVT harness greps for "ok"
 * (verify.py's t_sd) -- keep that substring in both outcomes' text if
 * editing this. */
/* Shared by both /test/sd (plain-text, DVT harness) and /sd_selftest_ui
 * (redirects back to Settings -> Storage) -- mounts if needed, runs the
 * test, and updates s_sd_selftest_result either way so the Storage tab's
 * card-info card reflects it on next render regardless of which caller
 * asked for it. */
static esp_err_t run_sd_selftest(void)
{
    if (!sdcard_mounted()) sdcard_init();
    if (!sdcard_mounted()) {
        snprintf(s_sd_selftest_result, sizeof(s_sd_selftest_result), "not mounted");
        return ESP_ERR_INVALID_STATE;
    }
    float wk = 0, rk = 0;
    esp_err_t err = sdcard_selftest(&wk, &rk);
    if (err == ESP_OK) {
        snprintf(s_sd_selftest_result, sizeof(s_sd_selftest_result),
                 "ok -- %.0f KB/s write, %.0f KB/s read", (double)wk, (double)rk);
    } else {
        snprintf(s_sd_selftest_result, sizeof(s_sd_selftest_result), "FAILED -- see device log");
    }
    return err;
}

/* POST /test/sd -- plain-text result the DVT harness greps for "ok"
 * (verify.py's t_sd) -- keep that substring in every outcome's text if
 * editing this. Diagnostics tab's own button still points here, unchanged. */
static esp_err_t test_sd_handler(httpd_req_t *req)
{
    esp_err_t err = run_sd_selftest();
    if (err == ESP_ERR_INVALID_STATE) { httpd_resp_sendstr(req, "sd: not mounted\n"); return ESP_OK; }
    char resp[128];
    snprintf(resp, sizeof(resp), "sd: %s\n", s_sd_selftest_result);
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

/* POST /sd_selftest_ui -- same test as /test/sd, but for the Storage tab's
 * "Run self-test" button: redirects back so the result shows inline in
 * append_sdcard_info_card() instead of navigating to a bare text response.
 * Kept separate from /test/sd rather than branching that handler on an
 * Accept header or similar -- the DVT harness's contract there (plain text,
 * "ok" substring) is worth not touching at all. */
static esp_err_t sd_selftest_ui_handler(httpd_req_t *req)
{
    run_sd_selftest();
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/settings?tab=storage");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* POST /sd_format -- wipes the mounted microSD card. Real, irreversible
 * data loss -- the Storage tab's own confirm() dialog is the only guard,
 * this handler itself just does it. */
static esp_err_t sd_format_handler(httpd_req_t *req)
{
    esp_err_t err = sdcard_format();
    if (err == ESP_OK) {
        s_sd_selftest_result[0] = '\0'; /* stale after a wipe -- re-run to get a fresh number */
        ESP_LOGI(TAG, "SD card formatted via web UI");
        activity_log("microSD card formatted");
    } else {
        ESP_LOGW(TAG, "SD format failed: %s", esp_err_to_name(err));
    }
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/settings?tab=storage");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* POST /sd_force_format -- for a card sdcard_needs_format() flagged (talks
 * fine over SPI, no valid FAT filesystem): formats and mounts it in one
 * step, since /sd_format above requires an already-mounted card, which this
 * one by definition isn't. Real, irreversible data loss, same as
 * /sd_format -- the Storage tab's own confirm() dialog is the only guard. */
static esp_err_t sd_force_format_handler(httpd_req_t *req)
{
    esp_err_t err = sdcard_force_format();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "SD card reformatted (was unmounted/wrong filesystem) via web UI");
        activity_log("microSD card reformatted as FAT32 (was unrecognized)");
    } else {
        ESP_LOGW(TAG, "SD force-format failed: %s", esp_err_to_name(err));
    }
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/settings?tab=storage");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* ---- Speaker (earpiece) volume: live tuning + persistence ----
 * Webui counterpart to the bring-up `spkamp` console command -- same
 * ES8388 LOUT1VOL/ROUT1VOL convention (0=mute, 33=0dB, 1dB/step), same
 * per-line NVS persistence (board_speaker_volume_get/set). ES8388/SMD2
 * only: LOUT1VOL/ROUT1VOL doesn't exist on the THT board's WM8731. */
#if CONFIG_BOARD_TYPE_SMD2 || CONFIG_BOARD_TYPE_SMD4 || CONFIG_BOARD_TYPE_SMD5 || CONFIG_BOARD_TYPE_SMD5_CHEAP
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
#endif /* CONFIG_BOARD_TYPE_SMD2 || CONFIG_BOARD_TYPE_SMD4 || CONFIG_BOARD_TYPE_SMD5 || CONFIG_BOARD_TYPE_SMD5_CHEAP */

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
            /* This board advertises its own LAN IP in SDP/Contact (no
             * STUN/ICE -- see voip.c's own "no TLS/SRTP" doc comment) --
             * fine on the same LAN as the server, but if this board is at a
             * remote site, that private address is useless to the server
             * for return traffic. These two tell Asterisk to ignore what
             * the board *claims* its address is and instead learn the real
             * one from where REGISTER/RTP packets actually arrive from
             * (standard practice for any client behind NAT that can't
             * self-report a public address -- most consumer routers don't
             * allow that without STUN). Harmless on a LAN-only setup too,
             * so always on rather than conditional. */
            "rewrite_contact=yes\n"
            "rtp_symmetric=yes\n"
            /* Purely a display label for the server admin's own benefit --
             * shows up in "pjsip show endpoints"/"show contacts" so several
             * boards' extensions don't all look like bare interchangeable
             * numbers. Doesn't touch the extension itself: that stays plain
             * digits (dialable from a rotary/DTMF phone), unlike a scheme
             * that puts the board name IN the extension/username (e.g.
             * "dualphone2-1234") -- letters and hyphens aren't dialable at
             * all from a physical phone, so that string could never be
             * something another line actually dials to reach this one. */
            "callerid=\"%s Line %d\" <%s>\n"
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
            name, line, user,           /* callerid="name Line N" <user> */
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
    httpd_resp_send(req, buf, clamp_send_len(off, 4096));
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

/* POST /media_delete_bulk -- the "select multiple, delete selected" form
 * from append_storage_table()'s checkboxes: body is a normal urlencoded
 * POST with one repeated "names=<file>" field per checked box plus
 * "return_to" (same allowlisted-redirect handling as media_delete_handler()
 * just above). Heap-allocated receive buffer sized to the real body
 * (capped) instead of a stack buffer -- with up to STORAGE_LIST_MAX (100)
 * rows each contributing "names=<32 chars>&", a full "select all" on a
 * large listing is a few KB, too much to put on the httpd worker's stack
 * (see STORAGE_LIST_MAX's own doc comment on a prior real stack overflow
 * here). Deletes whatever names parse out and don't care about ordering or
 * duplicates; a name that fails to delete (already gone, bad name) is
 * logged and skipped rather than aborting the rest of the batch. */
#define MEDIA_DELETE_BULK_MAX_BODY 8192

static esp_err_t media_delete_bulk_handler(httpd_req_t *req)
{
    if (req->content_len <= 0) {
        httpd_resp_set_status(req, "303 See Other");
        httpd_resp_set_hdr(req, "Location", "/settings?tab=storage");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }
    size_t body_cap = (size_t)req->content_len;
    if (body_cap > MEDIA_DELETE_BULK_MAX_BODY) body_cap = MEDIA_DELETE_BULK_MAX_BODY;
    char *body = malloc(body_cap + 1);
    if (!body) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    size_t total = 0;
    while (total < body_cap) {
        int r = httpd_req_recv(req, body + total, body_cap - total);
        if (r <= 0) break;
        total += (size_t)r;
    }
    body[total] = '\0';

    int deleted = 0, failed = 0;
    const char *p = body;
    while (p && *p) {
        const char *amp = strchr(p, '&');
        size_t plen = amp ? (size_t)(amp - p) : strlen(p);
        if (plen > 6 && strncmp(p, "names=", 6) == 0) {
            char name[STORAGE_MAX_NAME];
            size_t vlen = plen - 6;
            if (vlen >= sizeof(name)) vlen = sizeof(name) - 1;
            memcpy(name, p + 6, vlen);
            name[vlen] = '\0';
            url_decode(name);
            if (name[0]) {
                if (storage_delete(name) == ESP_OK) {
                    deleted++;
                    ESP_LOGI(TAG, "deleted '%s' via web UI (bulk)", name);
                } else {
                    failed++;
                    ESP_LOGW(TAG, "bulk delete '%s' failed", name);
                }
            }
        }
        p = amp ? amp + 1 : NULL;
    }
    ESP_LOGI(TAG, "bulk delete: %d removed, %d failed", deleted, failed);

    char return_to[32] = "";
    form_field(body, "return_to", return_to, sizeof(return_to));
    free(body);

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
    storage_entry_t *entries = malloc(STORAGE_LIST_MAX * sizeof(storage_entry_t));
    size_t n = entries ? storage_list(entries, STORAGE_LIST_MAX) : 0;
    cJSON *arr = cJSON_CreateArray();
    for (size_t i = 0; i < n; i++) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "name", entries[i].name);
        cJSON_AddNumberToObject(e, "size", (double)entries[i].size);
        cJSON_AddItemToArray(arr, e);
    }
    free(entries);
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

    /* Existence check + streaming both go through storage_read_open() --
     * used to be a separate storage_list()-based pre-check first, which
     * looks similar but silently disagrees with it: storage_list() dispatches
     * to WHICHEVER backend is primary (SD when mounted, else internal,
     * storage.c) and only that one, while prompt/clip uploads always land on
     * internal flash specifically (storage_write_begin_local(), so pulling
     * the card mid-use can't take dial tone or the operator message with it)
     * -- storage_read_open() correctly checks both backends via
     * resolve_existing_path(), but the old pre-check here didn't, so any
     * internal-flash-only file 404'd here the instant a card was mounted,
     * even though it played back fine moments earlier with no card in.
     * Confirmed live 2026-09-17: a freshly-generated record_prompt.wav
     * (internal-only, no bundled default to mask it) 404'd on a board with
     * an SD card mounted; operator.wav on the same board happened to still
     * "work" only because a bundled default of that exact name also exists
     * (firmware/assets/operator.wav) for the bundled-assets fallback to
     * find -- masking the same bug rather than avoiding it. */
    storage_reader_t r;
    if (storage_read_open(name, &r) != ESP_OK) {
        httpd_resp_send_404(req);
        return ESP_OK;
    }
    size_t total = storage_read_size(&r);

    /* Stream in small chunks rather than malloc(size) + one-shot send --
     * clips can be a few hundred KB and the C6's heap doesn't reliably have
     * that much contiguous free RAM alongside WiFi/audio buffers (a single
     * malloc(338748) failed outright in testing). */

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

/* POST /ota_upload -- multipart, field "file" (a .bin from `idf.py build`).
 * Streams straight into the inactive OTA slot via esp_ota_write() --
 * 2KB chunks, never buffers the whole ~1.4MB image in RAM, same reasoning
 * as every other upload in this file (see multipart.h's doc comment).
 * esp_ota_end() validates the image is a genuine, uncorrupted app image
 * before this commits to booting it. Reboots on success; if the new image
 * never reaches esp_ota_mark_app_valid_cancel_rollback() (app_main.c) --
 * crashes, hangs, whatever -- CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
 * reverts to this slot automatically on the next boot. */
static esp_err_t ota_upload_handler(httpd_req_t *req)
{
    multipart_reader_t mp;
    if (multipart_reader_init(&mp, req) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "expected multipart/form-data");
        return ESP_FAIL;
    }

    char part_name[32], filename[64];
    bool wrote = false;
    bool led_set = false; /* only restore the LED below if OTA_UPDATING was actually entered */
    const char *err_msg = "no file uploaded";
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);

    while (multipart_next_part(&mp, part_name, sizeof(part_name), filename, sizeof(filename)) == ESP_OK) {
        if (strcmp(part_name, "file") == 0 && filename[0]) {
            if (!target) {
                err_msg = "no inactive OTA partition available (see partitions.csv)";
                break;
            }
            esp_ota_handle_t ota_handle = 0;
            esp_err_t err = esp_ota_begin(target, OTA_SIZE_UNKNOWN, &ota_handle);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
                err_msg = "failed to start writing to flash (see device log)";
                break;
            }
            /* Visible "don't unplug me" signal for the whole write -- set
             * back to BOARD_LED_OK below on any failure path (upload only
             * ever reaches here with WiFi/webui already up, so that's a
             * safe state to restore); a successful write reboots into the
             * new image shortly after anyway, restarting at BOOTING on its
             * own without needing an explicit reset here. */
            board_led_set_state(BOARD_LED_OTA_UPDATING);
            led_set = true;
            uint8_t chunk[2048];
            size_t n;
            bool write_failed = false;
            while ((n = multipart_read_body(&mp, chunk, sizeof(chunk))) > 0) {
                if (esp_ota_write(ota_handle, chunk, n) != ESP_OK) { write_failed = true; break; }
            }
            if (write_failed) {
                esp_ota_abort(ota_handle);
                err_msg = "write to flash failed partway through (see device log)";
                break;
            }
            err = esp_ota_end(ota_handle);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
                err_msg = (err == ESP_ERR_OTA_VALIDATE_FAILED)
                    ? "that file isn't a valid firmware image"
                    : "failed to finalize the write (see device log)";
                break;
            }
            err = esp_ota_set_boot_partition(target);
            if (err != ESP_OK) {
                err_msg = "image written but couldn't be set as next boot -- try again";
                break;
            }
            wrote = true;
        } else {
            uint8_t scratch[128];
            while (multipart_read_body(&mp, scratch, sizeof(scratch)) > 0) { } /* discard */
        }
    }
    multipart_reader_close(&mp);

    if (!wrote) {
        if (led_set) board_led_set_state(BOARD_LED_OK); /* only if OTA_UPDATING was actually entered */
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, err_msg);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req,
        "<!doctype html><meta charset=\"utf-8\"><title>Updating\xe2\x80\xa6</title>"
        "<meta http-equiv=\"refresh\" content=\"20;url=/settings?tab=hardware\">"
        "<p>Firmware written -- rebooting into it now. This page retries in 20s; if the board "
        "doesn't come back, the new image failed to start and it auto-reverted to the version "
        "you were already running.</p>");
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(300)); /* let the response above actually reach the browser first */
    esp_restart();
    return ESP_OK;
}

/* POST /ota_revert -- manual "go back to the previous firmware" button
 * (Settings -> Hardware -> Firmware, only rendered by append_firmware_card()
 * when the other slot is confirmed ESP_OTA_IMG_VALID). Distinct from the
 * automatic rollback in app_main.c: that one only fires for a new image
 * that never finishes booting; this is for an image that boots fine but
 * still isn't wanted (worse behavior, a field report, second thoughts) --
 * esp_ota_mark_app_invalid_rollback_and_reboot() marks the *currently
 * running* slot invalid and reboots straight into the previous one. It
 * only returns (rather than rebooting) on failure -- e.g. the other slot
 * turned out not to be valid after all -- so this handler only ever sends
 * a response in that error case. */
static esp_err_t ota_revert_handler(httpd_req_t *req)
{
    esp_err_t err = esp_ota_mark_app_invalid_rollback_and_reboot();
    ESP_LOGE(TAG, "ota revert failed: %s", esp_err_to_name(err));
    httpd_resp_set_type(req, "text/plain");
    char msg[160];
    snprintf(msg, sizeof(msg),
             "Revert failed: %s -- no confirmed-good previous firmware slot to fall back to.",
             esp_err_to_name(err));
    httpd_resp_send(req, msg, HTTPD_RESP_USE_STRLEN);
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

    /* This is the generic "write any named file to storage" endpoint --
     * gen_prompt.py --upload uses it for prompt/clip WAVs, and
     * upload_i18n.py (firmware/bringup/) reuses it as-is for i18n string
     * table JSON files (firmware/assets/i18n/), rather than needing a
     * second upload endpoint. prompts_mark_generated() only means anything
     * for an actual audio file (see prompts.c) -- skip it for anything
     * else so a JSON upload doesn't leave a bogus prompts-tracking entry
     * behind. */
    if (is_wav_name(name) || is_mp3_name(name)) {
        prompts_mark_generated(name);
    }
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

    /* Bounce back to whichever page sent the form -- this one endpoint
     * serves three different prompt fields (Operator, Record message, and
     * each Dial Codes row), all on the Dial Codes & Prompts settings tab. */
    redirect_to_referer_or(req, "/settings");
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
 * (voip_setup_password(), Settings -> Dial Codes & Prompts) -- same "per board, shared with
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
                                 "set a setup password first (Settings -> Dial Codes & Prompts) "
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

/* httpd's connection-open hook -- see idle_power_note_activity()'s doc
 * comment and this call site's own comment in webui_start() for why. Must
 * return ESP_OK (any other value tells httpd to close the socket
 * immediately); nothing here can fail. */
static esp_err_t webui_conn_open(httpd_handle_t hd, int sockfd)
{
    (void)hd; (void)sockfd;
    idle_power_note_activity();
    return ESP_OK;
}

esp_err_t webui_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    /* HTTPD_DEFAULT_CONFIG() leaves max_open_sockets at 7 (3 of which are
     * reserved for the server's own internal use, per that field's doc
     * comment in esp_http_server.h -- 4 actually available to clients) and
     * lru_purge_enable at false. Without LRU purging, once those 4 sockets
     * are all holding open (idle keep-alive) connections, every subsequent
     * accept() just fails outright -- "httpd_accept_conn: error in accept"
     * in the log -- rather than the server reclaiming the oldest idle one,
     * and the whole web UI stays unreachable until a client-side timeout
     * eventually closes something. A page pulling in several sub-resources
     * (the Settings page's per-recording <audio> elements, style.css, the
     * page itself) can open more parallel/keep-alive connections than that
     * per browser tab already; this is the officially documented fix
     * (esp_http_server.h's own httpd_sess_iterate() comment: "allow the
     * http server to close our socket if needed (lru_purge_enable)"), not
     * a workaround. Bumping max_open_sockets too, both within
     * CONFIG_LWIP_MAX_SOCKETS' current headroom (10) -- see sdkconfig. */
    config.lru_purge_enable = true;
    config.max_open_sockets = 8;
    /* Any new connection to this server counts as "someone's using the web
     * UI right now" -- see idle_power_note_activity()'s doc comment for
     * why this matters (it postpones/undoes WiFi power-save so browsing
     * Settings doesn't itself get slow). A new httpd connection is a close
     * enough proxy for real activity: a page's own GET plus its
     * style.css/audio sub-requests all open one each in practice, and even
     * a fully idle browser tab's keep-alive connections get recycled often
     * enough (5s recv/send_wait_timeout above) that this fires well within
     * one idle_power TICK_MS of any actual click. */
    config.open_fn = webui_conn_open;
    /* Default is 8 -- we register 65 routes below (two share the media
     * path, one GET, one POST, counted separately) and it keeps growing.
     * Past the cap, httpd_register_uri_handler() just fails per-route, which
     * used to go unchecked below, silently dropping the last couple of
     * routes -- the loop below now at least logs it, but this has already
     * been quietly blown through once (64, with the previous count of 65
     * routes -- the Bluetooth companion merge plus one new small feature
     * pushed it over with zero warning until a live boot log showed
     * ESP_ERR_HTTPD_HANDLERS_FULL). Leaving real headroom this time instead
     * of matching the count exactly. */
    config.max_uri_handlers = 96;
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
     * block). Confirmed live (2026-08ish): the Dial Codes tab, back when it
     * had a per-row "use existing file" <select> (append_file_select(),
     * removed since -- see this file's own "append_file_select() ...
     * removed" comment), reliably crashed/rebooted a board with a microSD
     * card mounted while the identical request against a board with no
     * card worked fine. Root cause was stack-allocated storage_entry_t
     * arrays stacking up under FATFS/SDMMC (a known heavier stack consumer
     * than SPIFFS in ESP-IDF) on top of settings_page_handler()'s and its
     * callees' own locals -- not confirmed via addr2line the way the MP3
     * case above was (no crash log was available at the time), but stack
     * exhaustion was the only thing that explained "card mounted" being
     * the deciding factor.
     *
     * That specific code path is gone now, and every storage_entry_t
     * listing array in this file is heap-allocated (STORAGE_LIST_MAX's own
     * doc comment), which was motivated by this exact history -- but this
     * task still walks FATFS/SDMMC for every Storage/Recordings/media
     * request, so the underlying "FATFS eats more stack than SPIFFS"
     * exposure hasn't gone away, just the one array-heavy trigger for it.
     * 16 KB is a generous, cheap (one-time, single always-running task)
     * margin against that -- no reason to walk it back now. */
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
        { .uri = "/static/favicon.svg", .method = HTTP_GET, .handler = favicon_svg_handler },
        { .uri = "/favicon.ico",     .method = HTTP_GET, .handler = favicon_ico_handler },
        { .uri = "/api/status",      .method = HTTP_GET, .handler = status_json_handler },
        { .uri = "/api/log",         .method = HTTP_GET, .handler = log_handler },
        { .uri = "/api/recordings",  .method = HTTP_GET, .handler = recordings_json_handler },
        { .uri = "/media/*",         .method = HTTP_GET, .handler = media_handler },
        { .uri = "/media/*",         .method = HTTP_POST, .handler = media_upload_handler },
        { .uri = "/media_delete/*",  .method = HTTP_POST, .handler = media_delete_handler },
        { .uri = "/media_delete_bulk", .method = HTTP_POST, .handler = media_delete_bulk_handler },
        { .uri = "/mode_set/*",      .method = HTTP_POST, .handler = mode_set_handler },
        { .uri = "/wifi_set",        .method = HTTP_POST, .handler = wifi_set_handler },
        { .uri = "/api/wifi_scan",   .method = HTTP_GET,  .handler = wifi_scan_json_handler },
        { .uri = "/ota_upload",      .method = HTTP_POST, .handler = ota_upload_handler },
        { .uri = "/ota_revert",      .method = HTTP_POST, .handler = ota_revert_handler },
        { .uri = "/wifi_reprovision", .method = HTTP_POST, .handler = wifi_reprovision_handler },
        { .uri = "/factory_reset",   .method = HTTP_POST, .handler = factory_reset_handler },
        { .uri = "/board_name",      .method = HTTP_POST, .handler = board_name_handler },
        { .uri = "/language_set",    .method = HTTP_POST, .handler = language_set_handler },
        { .uri = "/board_hw_set",    .method = HTTP_POST, .handler = board_hw_set_handler },
        { .uri = "/board_type_confirm", .method = HTTP_POST, .handler = board_type_confirm_handler },
        { .uri = "/offhook_alert",   .method = HTTP_POST, .handler = offhook_alert_handler },
        { .uri = "/timezone_set",    .method = HTTP_POST, .handler = timezone_set_handler },
        { .uri = "/tone_lang_set",   .method = HTTP_POST, .handler = tone_lang_set_handler },
        { .uri = "/quiet_hours",     .method = HTTP_POST, .handler = quiet_hours_handler },
        { .uri = "/voip_enable",     .method = HTTP_POST, .handler = voip_enable_handler },
        { .uri = "/idle_power",      .method = HTTP_POST, .handler = idle_power_settings_handler },
        { .uri = "/answering_machine", .method = HTTP_POST, .handler = answering_machine_handler },
        { .uri = "/record_max_ms",   .method = HTTP_POST, .handler = record_max_ms_handler },
        { .uri = "/dialcode_set_code",      .method = HTTP_POST, .handler = dialcode_set_code_handler },
        { .uri = "/dialcode_set_desc",      .method = HTTP_POST, .handler = dialcode_set_desc_handler },
        { .uri = "/dialcode_delete/*",      .method = HTTP_POST, .handler = dialcode_delete_handler },
        { .uri = "/dialcode_upload",        .method = HTTP_POST, .handler = dialcode_upload_handler },
        { .uri = "/operator_upload",        .method = HTTP_POST, .handler = operator_upload_handler },
        { .uri = "/prompt_text_set",        .method = HTTP_POST, .handler = prompt_text_set_handler },
        { .uri = "/api/prompt_requests",    .method = HTTP_GET,  .handler = prompt_requests_json_handler },
        { .uri = "/bt_enable_set",           .method = HTTP_POST, .handler = bt_enable_set_handler },
        { .uri = "/bt_pair",                 .method = HTTP_POST, .handler = bt_pair_handler },
        { .uri = "/bt_config_check",         .method = HTTP_POST, .handler = bt_config_check_handler },
        { .uri = "/selftest_begin",          .method = HTTP_POST, .handler = selftest_begin_handler },
        { .uri = "/selftest_action",         .method = HTTP_POST, .handler = selftest_action_handler },
        { .uri = "/selftest_stimulus",       .method = HTTP_POST, .handler = selftest_stimulus_handler },
        { .uri = "/selftest_answer",         .method = HTTP_POST, .handler = selftest_answer_handler },
        { .uri = "/selftest_save",           .method = HTTP_POST, .handler = selftest_save_handler },
        { .uri = "/voip_set",               .method = HTTP_POST, .handler = voip_set_handler },
        { .uri = "/voip_check",             .method = HTTP_POST, .handler = voip_check_handler },
        { .uri = "/voip_setup_password",    .method = HTTP_POST, .handler = voip_setup_password_handler },
        { .uri = "/log_level",              .method = HTTP_POST, .handler = log_level_handler },
        { .uri = "/test/mode",              .method = HTTP_POST, .handler = test_mode_handler },
        { .uri = "/test/loopback",          .method = HTTP_POST, .handler = test_loopback_handler },
        { .uri = "/test/tone",              .method = HTTP_POST, .handler = test_tone_handler },
        { .uri = "/test/mic_record",        .method = HTTP_POST, .handler = test_mic_record_handler },
        { .uri = "/test/inject",            .method = HTTP_POST, .handler = test_inject_handler },
        { .uri = "/test/record",            .method = HTTP_POST, .handler = test_record_handler },
        { .uri = "/test/sd",                .method = HTTP_POST, .handler = test_sd_handler },
        { .uri = "/sd_selftest_ui",         .method = HTTP_POST, .handler = sd_selftest_ui_handler },
        { .uri = "/sd_format",              .method = HTTP_POST, .handler = sd_format_handler },
        { .uri = "/sd_force_format",        .method = HTTP_POST, .handler = sd_force_format_handler },
#if CONFIG_BOARD_TYPE_SMD2 || CONFIG_BOARD_TYPE_SMD4 || CONFIG_BOARD_TYPE_SMD5 || CONFIG_BOARD_TYPE_SMD5_CHEAP
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

#include "livestream.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>

#include "activity.h"
#include "audio.h"
#include "callcap.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "idle_power.h"
#include "lwip/sockets.h"
#include "nvs.h"

static const char *TAG = "livestream";

#define OUT_RATE    16000
#define MAX_OUT     480                 /* output frames per audio pass, upper bound */
#define SB_BYTES    (8 * 1024)          /* 256 ms mono, 128 ms stereo: sent in chunks, the backlog stays small */
#define MAX_BACKLOG_MS 200              /* more than this queued: drop the excess, so the delay can't grow */
#define CHUNK_MS    40                  /* send this much at a time: 10 ms segments ran into lwIP's
                                         * per-connection segment limit and fell behind */
#define NVS_NS      "livestream"

/* ---- shared with the audio task ----------------------------------------- */

static StreamBufferHandle_t s_sb;       /* allocated once, never freed: a tap may still be running when removed */
static uint32_t s_decim = 3, s_rate = OUT_RATE;
static volatile bool s_stereo;
static volatile uint32_t s_dropped_frames, s_full_frames, s_in_frames;
static int16_t s_dl[MAX_OUT];
static size_t  s_dl_n;
static int16_t s_out[MAX_OUT * 2];

static inline int16_t avg_decim(const int16_t *p, uint32_t d)
{
    int32_t acc = 0;
    for (uint32_t k = 0; k < d; k++) acc += p[k];
    return (int16_t)(acc / (int32_t)d);
}

/* Runs first in each audio pass: what the earpiece is about to hear. */
static void dl_tap(int line, const int16_t *mono, size_t n, void *ctx)
{
    (void)line; (void)ctx;
    size_t m = n / s_decim;
    if (m > MAX_OUT) m = MAX_OUT;
    for (size_t i = 0; i < m; i++) s_dl[i] = avg_decim(mono + i * s_decim, s_decim);
    s_dl_n = m;
}

/* Runs right after it: what the phone's mic sent. Pairs the two and queues
 * whole frames only, so the byte stream never loses its alignment. */
static void ul_tap(int line, const int16_t *mono, size_t n, void *ctx)
{
    (void)line; (void)ctx;
    size_t m = n / s_decim;
    if (m > MAX_OUT) m = MAX_OUT;
    bool st = s_stereo;
    for (size_t i = 0; i < m; i++) {
        int16_t up = avg_decim(mono + i * s_decim, s_decim);
        int16_t dn = i < s_dl_n ? s_dl[i] : 0;
        if (st) {
            s_out[2 * i] = up;
            s_out[2 * i + 1] = dn;
        } else {
            /* Half each: dial tone alone peaks near full scale, and the
             * mic also hears the earpiece back, so a plain sum clipped. */
            s_out[i] = (int16_t)(((int32_t)up + dn) / 2);
        }
    }
    size_t bytes = m * (st ? 4 : 2);
    s_in_frames += m;
    if (xStreamBufferSpacesAvailable(s_sb) < bytes) { s_dropped_frames += m; s_full_frames += m; return; }
    xStreamBufferSend(s_sb, s_out, bytes, 0);
}

/* ---- state -------------------------------------------------------------- */

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_enabled;
static TaskHandle_t s_task;
static char s_key[LIVESTREAM_KEY_LEN + 1];
static volatile bool s_key_changed;
static livestream_status_t s_status;
static int64_t s_t0_us;
static int s_tap_line = -1;

static void load_key(void)
{
    if (s_key[0]) return;
    nvs_handle_t h;
    size_t len = sizeof(s_key);
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_str(h, "key", s_key, &len) != ESP_OK) s_key[0] = '\0';
        nvs_close(h);
    }
    if (strlen(s_key) != LIVESTREAM_KEY_LEN) livestream_new_key();
}

void livestream_new_key(void)
{
    char k[LIVESTREAM_KEY_LEN + 1];
    for (int i = 0; i < LIVESTREAM_KEY_LEN; i += 8)
        snprintf(k + i, 9, "%08lx", (unsigned long)esp_random());
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "key", k);
        nvs_commit(h);
        nvs_close(h);
    }
    portENTER_CRITICAL(&s_mux);
    memcpy(s_key, k, sizeof(s_key));
    portEXIT_CRITICAL(&s_mux);
    s_key_changed = true;
}

void livestream_key(char *out, size_t len)
{
    load_key();
    portENTER_CRITICAL(&s_mux);
    strlcpy(out, s_key, len);
    portEXIT_CRITICAL(&s_mux);
}

bool livestream_enabled(void) { return s_enabled; }

livestream_status_t livestream_status(void)
{
    livestream_status_t st;
    portENTER_CRITICAL(&s_mux);
    st = s_status;
    portEXIT_CRITICAL(&s_mux);
    if (st.streaming) {
        st.secs = (uint32_t)((esp_timer_get_time() - s_t0_us) / 1000000);
        st.dropped_ms = s_rate ? (uint32_t)((uint64_t)s_dropped_frames * 1000 / s_rate) : 0;
    }
    return st;
}

/* ---- taps --------------------------------------------------------------- */

static void taps_off(void)
{
    if (s_tap_line < 0) return;
    audio_remove_diag_tap(s_tap_line);
    audio_remove_downlink_tap(s_tap_line);
    s_tap_line = -1;
    vTaskDelay(pdMS_TO_TICKS(40));      /* let a tap already running finish */
}

static void taps_on(int line)
{
    s_dl_n = 0;
    audio_add_downlink_tap(line, dl_tap, NULL);   /* before the uplink tap, as each pass runs them */
    audio_add_diag_tap(line, ul_tap, NULL);
    s_tap_line = line;
}

/* ---- HTTP --------------------------------------------------------------- */

static void send_text(int fd, const char *status, const char *msg)
{
    char b[384];
    int n = snprintf(b, sizeof(b),
        "HTTP/1.0 %s\r\nContent-Type: text/plain; charset=utf-8\r\nConnection: close\r\n\r\n%s\n",
        status, msg);
    send(fd, b, n, 0);
}

static bool query_val(const char *q, const char *name, char *out, size_t len)
{
    size_t nl = strlen(name);
    for (const char *p = q; p && *p; ) {
        if (strncmp(p, name, nl) == 0 && p[nl] == '=') {
            p += nl + 1;
            size_t i = 0;
            while (*p && *p != '&' && *p != ' ' && i + 1 < len) out[i++] = *p++;
            out[i] = '\0';
            return true;
        }
        p = strchr(p, '&');
        if (p) p++;
    }
    return false;
}

/* Reads the request line and checks it. Returns true with the line and
 * format filled in, or answers the client itself and returns false. */
static bool read_request(int fd, int *line, bool *stereo)
{
    struct timeval tv = { .tv_sec = 2 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    char req[512];
    size_t got = 0;
    while (got + 1 < sizeof(req)) {
        int r = recv(fd, req + got, sizeof(req) - 1 - got, 0);
        if (r <= 0) break;
        got += r;
        req[got] = '\0';
        if (strstr(req, "\r\n\r\n") || strstr(req, "\n\n")) break;
    }
    req[got] = '\0';
    if (strncmp(req, "GET ", 4) != 0) { send_text(fd, "400 Bad Request", "Expected a GET request."); return false; }
    char *path = req + 4;
    char *end = strchr(path, ' ');
    if (end) *end = '\0';
    char *q = strchr(path, '?');
    if (q) *q++ = '\0';
    if (strcmp(path, "/live.wav") != 0) {
        send_text(fd, "404 Not Found", "The live call audio is at /live.wav?key=... (copy the link from Settings -> Diagnostics).");
        return false;
    }
    if (!s_enabled) {
        send_text(fd, "503 Service Unavailable", "Live call audio is turned off: turn it on in Settings -> Diagnostics.");
        return false;
    }
    char key[LIVESTREAM_KEY_LEN + 2] = "", cur[LIVESTREAM_KEY_LEN + 1], v[8];
    livestream_key(cur, sizeof(cur));
    if (!q || !query_val(q, "key", key, sizeof(key)) || strcmp(key, cur) != 0) {
        send_text(fd, "403 Forbidden", "Wrong or missing key: copy the link from Settings -> Diagnostics.");
        return false;
    }
    *line = (query_val(q, "line", v, sizeof(v)) && v[0] == '1') ? 1 : 0;
    *stereo = query_val(q, "stereo", v, sizeof(v)) && v[0] == '1';
    return true;
}

static void wr_u16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void wr_u32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

static bool send_all(int fd, const void *p, size_t n)
{
    const uint8_t *b = p;
    while (n) {
        int w = send(fd, b, n, 0);
        if (w <= 0) return false;
        b += w;
        n -= w;
    }
    return true;
}

static bool send_headers(int fd, bool stereo)
{
    static const char hdr[] =
        "HTTP/1.0 200 OK\r\nContent-Type: audio/wav\r\nCache-Control: no-cache\r\n"
        "Connection: close\r\n\r\n";
    uint16_t ch = stereo ? 2 : 1;
    uint8_t h[44];
    uint32_t data = 0x7FFFFFF0;          /* endless */
    memcpy(h, "RIFF", 4);      wr_u32(h + 4, 36 + data); memcpy(h + 8, "WAVE", 4);
    memcpy(h + 12, "fmt ", 4); wr_u32(h + 16, 16);
    wr_u16(h + 20, 1);
    wr_u16(h + 22, ch);
    wr_u32(h + 24, s_rate);
    wr_u32(h + 28, s_rate * 2 * ch);
    wr_u16(h + 32, 2 * ch);
    wr_u16(h + 34, 16);
    memcpy(h + 36, "data", 4); wr_u32(h + 40, data);
    return send_all(fd, hdr, sizeof(hdr) - 1) && send_all(fd, h, sizeof(h));
}

static void set_status(bool streaming, const char *ip, int line, bool stereo)
{
    portENTER_CRITICAL(&s_mux);
    s_status.streaming = streaming;
    strlcpy(s_status.client, ip ? ip : "", sizeof(s_status.client));
    s_status.line = line;
    s_status.stereo = stereo;
    s_status.secs = 0;
    s_status.dropped_ms = 0;
    portEXIT_CRITICAL(&s_mux);
}

/* ---- the task ----------------------------------------------------------- */

static int open_listener(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(LIVESTREAM_PORT),
                             .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(fd, 2) != 0) {
        close(fd);
        return -1;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    return fd;
}

static void task(void *arg)
{
    (void)arg;
    int lfd = open_listener();
    if (lfd < 0) {
        ESP_LOGE(TAG, "can't listen on port %d", LIVESTREAM_PORT);
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "listening on port %d", LIVESTREAM_PORT);

    int cfd = -1, cline = 0;
    char cip[16] = "";
    static uint8_t buf[CHUNK_MS * OUT_RATE / 1000 * 4];   /* one chunk, stereo */
    int64_t last_note = 0;

    for (;;) {
        /* A new connection, checked every pass (and waited for when idle). */
        if (cfd < 0) {
            fd_set r;
            FD_ZERO(&r);
            FD_SET(lfd, &r);
            struct timeval tv = { .tv_sec = 1 };
            if (select(lfd + 1, &r, NULL, NULL, &tv) <= 0) continue;
        }
        struct sockaddr_in peer;
        socklen_t pl = sizeof(peer);
        int nfd = accept(lfd, (struct sockaddr *)&peer, &pl);
        if (nfd >= 0) {
            fcntl(nfd, F_SETFL, fcntl(nfd, F_GETFL, 0) & ~O_NONBLOCK);
            int line = 0;
            bool stereo = false;
            if (!read_request(nfd, &line, &stereo)) {
                close(nfd);
            } else {
                if (cfd >= 0) { close(cfd); ESP_LOGI(TAG, "%s replaced by a new listener", cip); }
                taps_off();
                cfd = nfd;
                cline = line;
                inet_ntoa_r(peer.sin_addr, cip, sizeof(cip));
                int one = 1;
                setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
                struct timeval tv = { .tv_sec = 3 };
                setsockopt(cfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
                uint32_t sr = audio_sample_rate();
                s_decim = sr >= OUT_RATE ? sr / OUT_RATE : 1;
                s_rate = sr / s_decim;
                s_stereo = stereo;
                s_dropped_frames = s_full_frames = s_in_frames = 0;
                xStreamBufferReset(s_sb);
                xStreamBufferSetTriggerLevel(s_sb, (s_rate * CHUNK_MS / 1000) * (stereo ? 4 : 2));
                if (!send_headers(cfd, stereo)) {
                    close(cfd);
                    cfd = -1;
                    continue;
                }
                taps_on(cline);
                s_t0_us = esp_timer_get_time();
                set_status(true, cip, cline, stereo);
                ESP_LOGI(TAG, "streaming line %d (%s, %lu Hz) to %s", cline, stereo ? "stereo" : "mono",
                         (unsigned long)s_rate, cip);
                activity_log("Live call audio: streaming line %d to %s", cline, cip);
            }
        }
        if (cfd < 0) continue;

        bool drop = !s_enabled || s_key_changed;
        size_t bpf = s_stereo ? 4 : 2;
        if (!drop) {
            size_t chunk = (s_rate * CHUNK_MS / 1000) * bpf;
            size_t got = xStreamBufferReceive(s_sb, buf, chunk, pdMS_TO_TICKS(150));
            if (got == 0) {
                /* No audio for 100 ms: the taps were taken over (a call
                 * recording on this line) and maybe given back. Send 100 ms
                 * of silence so the listener stays in real time, and claim
                 * the taps again once nothing else is recording. */
                size_t n = (s_rate / 10) * bpf;
                memset(buf, 0, sizeof(buf));
                while (n && !drop) {
                    size_t k = n < sizeof(buf) ? n : sizeof(buf);
                    drop = !send_all(cfd, buf, k);
                    n -= k;
                }
                if (callcap_state() != CALLCAP_RECORDING && callcap_state() != CALLCAP_SAVING)
                    taps_on(cline);
            } else {
                drop = !send_all(cfd, buf, got);
                size_t backlog = xStreamBufferBytesAvailable(s_sb);
                size_t limit = (s_rate * MAX_BACKLOG_MS / 1000) * bpf;
                if (limit > SB_BYTES * 3 / 4) limit = SB_BYTES * 3 / 4;
                while (backlog > limit) {      /* the network fell behind: catch up */
                    size_t k = backlog - limit;
                    k -= k % bpf;
                    if (k > sizeof(buf)) k = sizeof(buf) - sizeof(buf) % bpf;
                    if (!k) break;
                    size_t r = xStreamBufferReceive(s_sb, buf, k, 0);
                    s_dropped_frames += r / bpf;
                    backlog = xStreamBufferBytesAvailable(s_sb);
                    if (!r) break;
                }
            }
        }
        int64_t now = esp_timer_get_time();
        if (now - last_note > 1000000) { idle_power_note_activity(); last_note = now; }
        if (drop) {
            taps_off();
            close(cfd);
            cfd = -1;
            s_key_changed = false;
            set_status(false, NULL, 0, false);
            ESP_LOGI(TAG, "stopped streaming to %s after %lu s: %lu frames in, %lu ms lost (%lu ms buffer full)",
                     cip, (unsigned long)((esp_timer_get_time() - s_t0_us) / 1000000),
                     (unsigned long)s_in_frames, (unsigned long)((uint64_t)s_dropped_frames * 1000 / s_rate),
                     (unsigned long)((uint64_t)s_full_frames * 1000 / s_rate));
            activity_log("Live call audio: stopped streaming to %s", cip);
        }
    }
}

esp_err_t livestream_set_enabled(bool on)
{
    load_key();
    if (on && !s_sb) {
        s_sb = xStreamBufferCreate(SB_BYTES, 1);
        if (!s_sb) return ESP_ERR_NO_MEM;
    }
    s_key_changed = false;
    s_enabled = on;
    if (on && !s_task &&
        xTaskCreate(task, "livestream", 4096, NULL, tskIDLE_PRIORITY + 4, &s_task) != pdPASS) {
        s_task = NULL;
        s_enabled = false;
        return ESP_ERR_NO_MEM;
    }
    activity_log("Live call audio turned %s", on ? "on" : "off");
    return ESP_OK;
}

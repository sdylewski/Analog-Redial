#include "rtp.h"
#include "g711.h"
#include "audio.h"
#include "aec.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "rtp";

/* 20 ms packets: 160 samples @ 8 kHz = 160 µ-law bytes; 960 samples @ 48 kHz. */
#define PTIME_MS       20
#define SAMPLES_8K     160
#define SAMPLES_48K    960
#define DECIM          6            /* 48000 / 8000 */
#define RTP_HDR_LEN    12
#define RTP_PT_PCMU    0

typedef struct {
    volatile bool active;
    int          sock;
    struct sockaddr_in remote;
    uint16_t     local_port;
    /* tx RTP state */
    uint16_t     seq;
    uint32_t     ts;
    uint32_t     ssrc;
    /* downsample carry (uplink 48k->8k) */
    int32_t      dn_acc;
    uint32_t     dn_n;
    /* upsample continuity (downlink 8k->48k) */
    int16_t      up_prev;
    bool         up_primed;
    TaskHandle_t tx_task;
    TaskHandle_t rx_task;
} rtp_line_t;

static rtp_line_t s_rtp[AUDIO_NUM_LINES];

uint16_t rtp_local_port(int line)
{
    return (uint16_t)(4000 + line * 2); /* line0 -> 4000, line1 -> 4002 */
}

/* Pull exactly SAMPLES_48K uplink samples (looping short reads); pad with
 * silence on underrun so RTP cadence stays steady. */
static void read_uplink_48k(int line, int16_t *buf)
{
    size_t got = 0;
    int64_t deadline_ticks = xTaskGetTickCount() + pdMS_TO_TICKS(PTIME_MS + 10);
    while (got < SAMPLES_48K) {
        size_t n = audio_rx_read(line, buf + got, SAMPLES_48K - got, PTIME_MS);
        got += n;
        if (n == 0 && xTaskGetTickCount() >= deadline_ticks) break;
    }
    for (size_t i = got; i < SAMPLES_48K; i++) buf[i] = 0; /* pad underrun */
}

static void tx_task(void *arg)
{
    int line = (int)(intptr_t)arg;
    rtp_line_t *r = &s_rtp[line];
    int16_t *in = malloc(SAMPLES_48K * sizeof(int16_t));
    uint8_t *pkt = malloc(RTP_HDR_LEN + SAMPLES_8K);
    int16_t *aec8 = malloc(SAMPLES_8K * sizeof(int16_t));
    if (!in || !pkt || !aec8) { ESP_LOGE(TAG, "line %d tx alloc failed", line); goto out; }

    TickType_t next = xTaskGetTickCount();

    while (r->active) {
        uint8_t *payload = pkt + RTP_HDR_LEN;
        size_t on = 0;

        if (aec_is_enabled(line)) {
            /* Echo canceller active: audio.c has already cancelled AND
             * decimated to 8 kHz (it is the only place where the played frame
             * and the captured frame are aligned -- see audio.h). Draining the
             * 48 kHz path too would steal the recorder's/diag tap's samples,
             * so this branch reads only the cancelled stream. */
            size_t got = 0;
            int64_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(PTIME_MS + 10);
            while (got < SAMPLES_8K) {
                size_t n = audio_aec_read(line, aec8 + got, SAMPLES_8K - got, PTIME_MS);
                got += n;
                if (n == 0 && xTaskGetTickCount() >= deadline) break;
            }
            for (size_t i = got; i < SAMPLES_8K; i++) aec8[i] = 0; /* pad underrun */
            for (; on < SAMPLES_8K; on++) payload[on] = ulaw_encode(aec8[on]);
        } else {
            read_uplink_48k(line, in);
            /* decimate 48k -> 8k by averaging groups of 6 (carried across packets) */
            for (size_t i = 0; i < SAMPLES_48K && on < SAMPLES_8K; i++) {
                r->dn_acc += in[i];
                if (++r->dn_n == DECIM) {
                    payload[on++] = ulaw_encode((int16_t)(r->dn_acc / DECIM));
                    r->dn_acc = 0;
                    r->dn_n = 0;
                }
            }
            while (on < SAMPLES_8K) payload[on++] = ulaw_encode(0);
        }

        /* RTP header (RFC 3550): V=2, PT=0 (PCMU) */
        pkt[0] = 0x80;
        pkt[1] = RTP_PT_PCMU;              /* M=0 */
        pkt[2] = r->seq >> 8; pkt[3] = r->seq & 0xFF;
        pkt[4] = r->ts >> 24; pkt[5] = r->ts >> 16; pkt[6] = r->ts >> 8; pkt[7] = r->ts;
        pkt[8] = r->ssrc >> 24; pkt[9] = r->ssrc >> 16; pkt[10] = r->ssrc >> 8; pkt[11] = r->ssrc;
        r->seq++;
        r->ts += SAMPLES_8K;

        sendto(r->sock, pkt, RTP_HDR_LEN + SAMPLES_8K, 0,
               (struct sockaddr *)&r->remote, sizeof(r->remote));

        vTaskDelayUntil(&next, pdMS_TO_TICKS(PTIME_MS));
    }
out:
    free(in);
    free(pkt);
    free(aec8);
    r->tx_task = NULL;
    vTaskDelete(NULL);
}

static void rx_task(void *arg)
{
    int line = (int)(intptr_t)arg;
    rtp_line_t *r = &s_rtp[line];
    uint8_t *buf = malloc(RTP_HDR_LEN + 512);
    int16_t *out = malloc(SAMPLES_48K * sizeof(int16_t));
    if (!buf || !out) { ESP_LOGE(TAG, "line %d rx alloc failed", line); goto out; }

    while (r->active) {
        int n = recv(r->sock, buf, RTP_HDR_LEN + 512, 0);
        if (n < RTP_HDR_LEN + 1) continue; /* timeout or runt */

        /* Skip the RTP header (+ any CSRCs); ignore extensions for our peers. */
        int cc = buf[0] & 0x0F;
        int hdr = RTP_HDR_LEN + cc * 4;
        if (n <= hdr) continue;
        if ((buf[1] & 0x7F) != RTP_PT_PCMU) continue; /* only PCMU */

        const uint8_t *pl = buf + hdr;
        int npl = n - hdr;

        /* µ-law -> 8k PCM -> linear-interp upsample x6 -> 48k */
        size_t oo = 0;
        for (int i = 0; i < npl && oo + DECIM <= SAMPLES_48K; i++) {
            int16_t cur = ulaw_decode(pl[i]);
            if (!r->up_primed) { r->up_prev = cur; r->up_primed = true; }
            for (int k = 0; k < DECIM; k++) {
                int32_t v = r->up_prev + (int32_t)(cur - r->up_prev) * k / DECIM;
                out[oo++] = (int16_t)v;
            }
            r->up_prev = cur;
        }
        if (oo) audio_ext_write(line, out, oo);
    }
out:
    free(buf);
    free(out);
    r->rx_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t rtp_start(int line, const char *remote_ip, uint16_t remote_port)
{
    if (line < 0 || line >= AUDIO_NUM_LINES) return ESP_ERR_INVALID_ARG;
    rtp_line_t *r = &s_rtp[line];
    if (r->active) rtp_stop(line);

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return ESP_FAIL;

    struct sockaddr_in local = {
        .sin_family = AF_INET,
        .sin_port = htons(rtp_local_port(line)),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&local, sizeof(local)) < 0) {
        ESP_LOGE(TAG, "line %d: bind RTP :%u failed", line, rtp_local_port(line));
        close(sock);
        return ESP_FAIL;
    }
    /* recv timeout so rx_task can notice r->active going false */
    struct timeval tv = { .tv_sec = 0, .tv_usec = 200000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    memset(&r->remote, 0, sizeof(r->remote));
    r->remote.sin_family = AF_INET;
    r->remote.sin_port = htons(remote_port);
    r->remote.sin_addr.s_addr = inet_addr(remote_ip);

    r->sock = sock;
    r->local_port = rtp_local_port(line);
    r->seq = (uint16_t)esp_random();
    r->ts = esp_random();
    r->ssrc = esp_random();
    r->dn_acc = 0; r->dn_n = 0;
    r->up_prev = 0; r->up_primed = false;
    r->active = true;

    /* Priority 6: above the web server, the phone logic and SIP (all 5), so
     * a big page, a download or a firmware upload can't hold up a call's
     * 20 ms packets. Both tasks are short and mostly blocked (recv timeout,
     * tx pacing), so they don't starve those either. */
    r->rx_task = r->tx_task = NULL;
    TaskHandle_t rx = NULL, tx = NULL;
    BaseType_t ok = xTaskCreate(rx_task, "rtp_rx", 4096, (void *)(intptr_t)line,
                                tskIDLE_PRIORITY + 6, &rx);
    if (ok == pdPASS) r->rx_task = rx;
    if (ok == pdPASS) {
        ok = xTaskCreate(tx_task, "rtp_tx", 4096, (void *)(intptr_t)line, tskIDLE_PRIORITY + 6, &tx);
        if (ok == pdPASS) r->tx_task = tx;
    }
    if (ok != pdPASS) {
        /* Without both tasks the call would connect with one-way or no
         * audio. Fail instead: both callers send BYE and close the call. */
        ESP_LOGE(TAG, "line %d: no memory for the RTP tasks -- not connecting the call", line);
        rtp_stop(line);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "line %d: RTP up, local :%u <-> %s:%u",
             line, r->local_port, remote_ip, remote_port);
    return ESP_OK;
}

void rtp_stop(int line)
{
    if (line < 0 || line >= AUDIO_NUM_LINES) return;
    rtp_line_t *r = &s_rtp[line];
    if (!r->active) return;
    r->active = false;
    /* Let tasks observe the flag and exit (rx wakes on its 200 ms timeout). */
    for (int i = 0; i < 20 && (r->tx_task || r->rx_task); i++) vTaskDelay(pdMS_TO_TICKS(20));
    if (r->sock >= 0) { close(r->sock); r->sock = -1; }
    ESP_LOGI(TAG, "line %d: RTP stopped", line);
}

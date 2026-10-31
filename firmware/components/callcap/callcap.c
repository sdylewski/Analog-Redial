#include "callcap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "activity.h"
#include "audio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "storage.h"

static const char *TAG = "callcap";

#define TARGET_RATE   16000               /* to the card */
#define LOCAL_RATE    8000                /* to internal flash: half the write load, and the line carries nothing above 3.4 kHz */
#define CHUNK_BYTES   2048                /* 512 stereo frames, 32 ms at 16 kHz */
#define MAX_CHUNKS    10                  /* ~320 ms of slack for a slow card write */
#define MIN_CHUNKS    6
#define NEED_HEAP     (36 * 1024)         /* chunks + task stack, with room left for the call itself */
#define MAX_SECS      (20 * 60)
#define MAX_FRAME     960                 /* engine samples per audio pass, upper bound */
#define RESERVE_BYTES (2 * 1024 * 1024)   /* free space the card must have to start */
#define LOCAL_MIN_SECS 20                 /* internal flash must have room for this much to start */
#define LOCAL_MARGIN  (32 * 1024)         /* left free on internal flash */

/* The audio buffering is a pool of small chunks passed between the audio task
 * (fills them) and the capture task (writes them to the card) through two
 * queues of pointers, instead of one big buffer: on this board a 24 KB block
 * often doesn't exist, 2 KB ones always do. */
static struct {
    volatile callcap_state_t st;
    int      line;
    uint32_t decim, rate;
    bool     local;                     /* recording to internal flash, not the card */
    uint32_t max_bytes;                 /* internal flash: stop here (0 = no limit) */
    volatile bool force_local;          /* armed with where=flash */
    int64_t  t0_us;
    volatile bool stop;
    volatile uint32_t dropped_frames;   /* frames lost to a card stall, kept in the timeline as silence */
    uint32_t owed;                      /* silent frames still to write to repay them */
    QueueHandle_t free_q, full_q;
    uint8_t *chunks[MAX_CHUNKS];
    int      nchunks;
    uint8_t *cur;                       /* chunk the audio task is filling */
    size_t   cur_len;
    char     name[STORAGE_MAX_NAME];
    char     last_name[STORAGE_MAX_NAME];
    unsigned last_secs, last_dropped_ms;
} s;

static SemaphoreHandle_t s_lock;

/* Taps run on the audio task: no blocking, no allocation, little stack. The
 * downlink tap runs first in every audio pass and the uplink tap right after
 * it, so pairing them here keeps the two channels aligned. */
static int16_t s_dl[MAX_FRAME / 3 + 1];
static size_t  s_dl_n;

static inline int16_t avg_decim(const int16_t *p, uint32_t d)
{
    int32_t acc = 0;
    for (uint32_t k = 0; k < d; k++) acc += p[k];
    return (int16_t)(acc / (int32_t)d);
}

static void dl_tap(int line, const int16_t *mono, size_t n, void *ctx)
{
    (void)line; (void)ctx;
    if (n > MAX_FRAME) n = MAX_FRAME;
    size_t m = n / s.decim;
    for (size_t i = 0; i < m; i++) s_dl[i] = avg_decim(mono + i * s.decim, s.decim);
    s_dl_n = m;
}

/* Adds one stereo frame to the chunk being filled. False if the card is so
 * far behind that no free chunk is left. */
static inline bool put_frame(int16_t left, int16_t right)
{
    if (!s.cur) {
        if (xQueueReceive(s.free_q, &s.cur, 0) != pdTRUE) { s.cur = NULL; return false; }
        s.cur_len = 0;
    }
    int16_t *p = (int16_t *)(s.cur + s.cur_len);
    p[0] = left;                        /* what the phone sends */
    p[1] = right;                       /* what the earpiece hears */
    s.cur_len += 4;
    if (s.cur_len >= CHUNK_BYTES) {
        if (xQueueSend(s.full_q, &s.cur, 0) != pdTRUE)       /* can't happen: the queue holds every chunk */
            xQueueSend(s.free_q, &s.cur, 0);
        s.cur = NULL;
    }
    return true;
}

static void ul_tap(int line, const int16_t *mono, size_t n, void *ctx)
{
    (void)line; (void)ctx;
    if (n > MAX_FRAME) n = MAX_FRAME;
    size_t m = n / s.decim;
    for (size_t i = 0; i < m; i++) {
        /* A stall leaves gaps; fill them with silence once there's room, so
         * the file keeps real time and stays in step with a video. */
        while (s.owed > 0 && put_frame(0, 0)) s.owed--;
        if (!put_frame(avg_decim(mono + i * s.decim, s.decim), i < s_dl_n ? s_dl[i] : 0)) {
            s.owed++;
            s.dropped_frames++;
        }
    }
}

static void wr_u16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void wr_u32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

static void wav_header(uint8_t *h, uint32_t rate, uint32_t data_bytes)
{
    memcpy(h, "RIFF", 4);      wr_u32(h + 4, 36 + data_bytes); memcpy(h + 8, "WAVE", 4);
    memcpy(h + 12, "fmt ", 4); wr_u32(h + 16, 16);
    wr_u16(h + 20, 1);         /* PCM */
    wr_u16(h + 22, 2);         /* stereo: left = phone sends, right = earpiece hears */
    wr_u32(h + 24, rate);
    wr_u32(h + 28, rate * 4);
    wr_u16(h + 32, 4);
    wr_u16(h + 34, 16);
    memcpy(h + 36, "data", 4); wr_u32(h + 40, data_bytes);
}

static void make_name(char *out, size_t len)
{
    /* "call record YYMMDD HHMMSS.wav": 29 characters, inside the 31 the
     * storage layer allows. (The longer "call recording ..." would be 32.) */
    time_t now = time(NULL);
    if (now < 1700000000) {   /* clock not set yet */
        snprintf(out, len, "call record boot%lld.wav", (long long)(esp_timer_get_time() / 1000000));
        return;
    }
    struct tm t;
    localtime_r(&now, &t);
    snprintf(out, len, "call record %02d%02d%02d %02d%02d%02d.wav",
             t.tm_year % 100, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
}

static void remove_taps(int line)
{
    audio_remove_diag_tap(line);
    audio_remove_downlink_tap(line);
}

static void free_pool(void)
{
    if (s.free_q) { vQueueDelete(s.free_q); s.free_q = NULL; }
    if (s.full_q) { vQueueDelete(s.full_q); s.full_q = NULL; }
    for (int i = 0; i < s.nchunks; i++) { free(s.chunks[i]); s.chunks[i] = NULL; }
    s.nchunks = 0;
    s.cur = NULL;
}

static void finish(unsigned secs, unsigned dropped_ms, bool saved)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (saved) {
        strlcpy(s.last_name, s.name, sizeof(s.last_name));
        s.last_secs = secs;
        s.last_dropped_ms = dropped_ms;
    }
    free_pool();
    s.st = CALLCAP_IDLE;
    xSemaphoreGive(s_lock);
}

static bool write_chunk(storage_writer_t *w, const uint8_t *p, size_t n, uint32_t *total)
{
    if (storage_write_chunk(w, p, n) != ESP_OK) return false;
    *total += n;
    return true;
}

static void cap_task(void *arg)
{
    (void)arg;
    storage_writer_t w;
    /* Internal flash: plan on exactly the free space, so nothing (an
     * answering-machine message) is evicted to make room. */
    bool opened = (s.local ? storage_write_begin_local(s.name, s.max_bytes + 44, &w)
                           : storage_write_begin(s.name, RESERVE_BYTES, &w)) == ESP_OK;
    uint32_t data_bytes = 0;
    bool ok = opened;

    if (!opened) {
        ESP_LOGW(TAG, "couldn't open '%s' on %s", s.name, s.local ? "internal flash" : "the card");
        activity_log("Call recording failed: couldn't open the file on %s", s.local ? "internal flash" : "the card");
    } else {
        uint8_t hdr[44];
        wav_header(hdr, s.rate, 0);
        ok = storage_write_chunk(&w, hdr, sizeof(hdr)) == ESP_OK;
    }

    while (opened && ok) {
        uint8_t *c = NULL;
        if (xQueueReceive(s.full_q, &c, pdMS_TO_TICKS(50)) == pdTRUE) {
            ok = write_chunk(&w, c, CHUNK_BYTES, &data_bytes);
            xQueueSend(s.free_q, &c, 0);
            if (!ok) {
                ESP_LOGW(TAG, "write failed (%s): stopping", s.local ? "internal flash full" : "card removed or full");
                activity_log("Call recording stopped: write to %s failed", s.local ? "internal flash" : "the card");
                break;
            }
            if (s.max_bytes && data_bytes + CHUNK_BYTES > s.max_bytes) {
                ESP_LOGW(TAG, "internal flash is full: stopping");
                activity_log("Call recording stopped: internal flash is full");
                s.stop = true;
            }
        }
        unsigned secs = (unsigned)((esp_timer_get_time() - s.t0_us) / 1000000);
        if (s.st == CALLCAP_RECORDING && secs >= MAX_SECS) {
            ESP_LOGW(TAG, "20 minute limit reached");
            s.stop = true;
        }
        if (s.stop) break;
    }

    remove_taps(s.line);                    /* nothing writes the pool after this */
    if (opened && ok) {
        uint8_t *c = NULL;
        while (xQueueReceive(s.full_q, &c, 0) == pdTRUE && ok &&   /* what the taps left behind */
               (!s.max_bytes || data_bytes + CHUNK_BYTES <= s.max_bytes))
            ok = write_chunk(&w, c, CHUNK_BYTES, &data_bytes);
        if (ok && s.cur && s.cur_len && (!s.max_bytes || data_bytes + s.cur_len <= s.max_bytes))
            ok = write_chunk(&w, s.cur, s.cur_len, &data_bytes);
    }

    unsigned secs = s.rate ? data_bytes / (s.rate * 4) : 0;
    unsigned dropped_ms = s.rate ? (unsigned)((uint64_t)s.dropped_frames * 1000 / s.rate) : 0;
    bool saved = false;
    if (opened) {
        storage_write_end(&w);
        if (data_bytes > 0) {               /* a failed write keeps what reached the card */
            uint8_t sz[4];
            wr_u32(sz, 36 + data_bytes);
            storage_patch(s.name, 4, sz, 4);
            wr_u32(sz, data_bytes);
            storage_patch(s.name, 40, sz, 4);
            saved = true;
        }
    }
    if (saved) {
        ESP_LOGI(TAG, "saved '%s': %u s, %u KB, %u ms dropped", s.name, secs,
                 (unsigned)(data_bytes / 1024), dropped_ms);
        if (dropped_ms) activity_log("Call recording saved: %s (%us, %ums lost to slow %s)",
                                     s.name, secs, dropped_ms, s.local ? "flash writes" : "card writes");
        else            activity_log("Call recording saved: %s (%us%s)", s.name, secs,
                                     s.local ? ", internal flash" : "");
    }
    finish(secs, dropped_ms, saved);
    vTaskDelete(NULL);
}

/* ---- public API -------------------------------------------------------- */

static void ensure_lock(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
}

unsigned callcap_local_secs_free(void)
{
    size_t f = storage_free_bytes_internal();
    f = f > LOCAL_MARGIN ? f - LOCAL_MARGIN : 0;
    return (unsigned)(f / (LOCAL_RATE * 4));
}

esp_err_t callcap_arm(bool force_local)
{
    ensure_lock();
    bool local = force_local || !storage_using_sd();
    if (local && callcap_local_secs_free() < LOCAL_MIN_SECS) return ESP_ERR_NO_MEM;
    esp_err_t err = ESP_OK;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s.st != CALLCAP_IDLE) err = ESP_ERR_INVALID_STATE;
    else { s.st = CALLCAP_ARMED; s.force_local = force_local; }
    xSemaphoreGive(s_lock);
    if (err == ESP_OK) activity_log("Call recording armed (%s): lift a handset to start",
                                    local ? "internal flash" : "microSD card");
    return err;
}

void callcap_cancel(void)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s.st == CALLCAP_ARMED) {
        s.st = CALLCAP_IDLE;
        activity_log("Call recording cancelled");
    } else if (s.st == CALLCAP_RECORDING) {
        s.st = CALLCAP_SAVING;
        s.stop = true;
    }
    xSemaphoreGive(s_lock);
}

/* Called with the lock held. Returns why it couldn't start, or NULL. */
static const char *start_locked(int line)
{
    bool local = s.force_local || !storage_using_sd();
    uint32_t max_bytes = 0;
    if (local) {
        unsigned secs = callcap_local_secs_free();
        if (secs < LOCAL_MIN_SECS) return "internal flash is full";
        max_bytes = secs * LOCAL_RATE * 4;
    }
    if (heap_caps_get_free_size(MALLOC_CAP_8BIT) < NEED_HEAP) return "not enough free memory";

    uint32_t rate = audio_sample_rate();
    uint32_t decim = rate / (local ? LOCAL_RATE : TARGET_RATE);
    if (decim < 1) decim = 1;

    s.free_q = xQueueCreate(MAX_CHUNKS, sizeof(uint8_t *));
    s.full_q = xQueueCreate(MAX_CHUNKS, sizeof(uint8_t *));
    s.nchunks = 0;
    if (s.free_q && s.full_q) {
        for (int i = 0; i < MAX_CHUNKS; i++) {
            uint8_t *c = malloc(CHUNK_BYTES);
            if (!c) break;
            s.chunks[s.nchunks++] = c;
            xQueueSend(s.free_q, &c, 0);
        }
    }
    if (s.nchunks < MIN_CHUNKS) { free_pool(); return "not enough free memory"; }

    s.line = line;
    s.local = local;
    s.max_bytes = max_bytes;
    s.decim = decim;
    s.rate = rate / decim;
    s.stop = false;
    s.dropped_frames = 0;
    s.owed = 0;
    s.cur = NULL;
    s.cur_len = 0;
    s_dl_n = 0;
    s.t0_us = esp_timer_get_time();
    make_name(s.name, sizeof(s.name));
    if (xTaskCreate(cap_task, "callcap", 4096, NULL, tskIDLE_PRIORITY + 3, NULL) != pdPASS) {
        free_pool();
        return "couldn't start the capture task";
    }
    audio_add_downlink_tap(line, dl_tap, NULL);   /* before the uplink tap, as each pass runs them */
    audio_add_diag_tap(line, ul_tap, NULL);
    return NULL;
}

void callcap_hook(int line, bool offhook)
{
    if (!s_lock || s.st == CALLCAP_IDLE) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (offhook && s.st == CALLCAP_ARMED) {
        const char *why = start_locked(line);
        if (why) {
            s.st = CALLCAP_IDLE;
            activity_log("Call recording skipped: %s", why);
        } else {
            s.st = CALLCAP_RECORDING;
            activity_log("Call recording started on line %d (%s)", line,
                         s.local ? "internal flash" : "microSD card");
        }
    } else if (!offhook && s.st == CALLCAP_RECORDING && s.line == line) {
        s.st = CALLCAP_SAVING;
        s.stop = true;
    }
    xSemaphoreGive(s_lock);
}

callcap_state_t callcap_state(void) { return s.st; }

unsigned callcap_elapsed_secs(void)
{
    return s.st == CALLCAP_RECORDING ? (unsigned)((esp_timer_get_time() - s.t0_us) / 1000000) : 0;
}

const char *callcap_last_name(void) { return s.last_name; }
unsigned callcap_last_secs(void) { return s.last_secs; }
unsigned callcap_last_dropped_ms(void) { return s.last_dropped_ms; }

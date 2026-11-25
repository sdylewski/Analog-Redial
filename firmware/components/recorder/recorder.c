#include "recorder.h"
#include "audio.h"
#include "g711.h"
#include "storage.h"
#include "activity.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

static const char *TAG = "recorder";

typedef struct {
    bool          active;
    volatile bool stop;
} rec_state_t;

static rec_state_t s_rec[AUDIO_NUM_LINES];

typedef struct {
    int      line;
    char     name[STORAGE_MAX_NAME];
    uint32_t max_ms;
    uint32_t silence_ms;
    bool     downlink;
    recorder_done_cb_t cb;
    void    *ctx;
} rec_args_t;

/* 48 kHz input, read a whole number of decimation groups per pull (256 groups
 * of 6 = 1536 samples). Kept modest so the RX ring buffer never overflows. */
#define IN_CHUNK_SAMPLES 1536
#define SILENCE_RMS      500  /* ~ -36 dBFS; below this counts as "quiet" */

/* Downlink capture (see recorder_start_downlink()): audio_add_extwrite_tap()
 * is a push callback, not the pull-style audio_rx_read() the uplink path
 * uses (that seam is already owned by DTMF detection's permanent rx_tap
 * registration -- see telephony.c -- so recording can't share it the way it
 * shares the always-filled uplink stream buffer). Deliberately NOT
 * audio_add_downlink_tap() -- that observes the line's muxed TX *output*,
 * which goes silent the moment the answering machine silences its own TX
 * during the actual recording phase (so the caller doesn't hear a repeating
 * beep); audio_add_extwrite_tap() observes audio_ext_write()'s input
 * directly, before muxing, so it keeps seeing the caller's decoded RTP
 * audio regardless of what this line is currently transmitting. Bridged
 * into the same pull shape rec_task() already expects via a small local
 * stream buffer, same pattern as audio.c's own echo_probe_tap() -- just
 * bounded to a few chunks of slack instead of the whole capture, since a
 * voicemail can run up to 2 minutes. */
#define DOWNLINK_SB_BYTES 10400   /* 1.3 s of 8 kHz mu-law: holds the caller's first words while the file opens */

typedef struct {
    StreamBufferHandle_t sb;
    uint32_t decim;      /* device rate / 8 kHz */
    int32_t  acc;        /* decimation accumulator: touched only by the tap (audio task) */
    uint32_t acc_n;
} downlink_ctx_t;

/* Runs in the audio task: decimates to 8 kHz and packs mu-law (G.711, which is what the call
 * carried in the first place), so the stream buffer holds a useful second or more in ~10 KB. */
static void downlink_push(int line, const int16_t *mono, size_t n, void *ctx)
{
    (void)line;
    downlink_ctx_t *dc = ctx;
    uint8_t bytes[96];
    size_t nb = 0;
    /* Non-blocking, like echo_probe_tap() -- the audio task can never block.
     * A full buffer (rec_task briefly stalled on a storage write) drops the
     * overrun rather than corrupting timing. */
    for (size_t i = 0; i < n; i++) {
        dc->acc += mono[i];
        if (++dc->acc_n == dc->decim) {
            bytes[nb++] = ulaw_encode((int16_t)(dc->acc / (int32_t)dc->decim));
            dc->acc = 0;
            dc->acc_n = 0;
            if (nb == sizeof(bytes)) { xStreamBufferSend(dc->sb, bytes, nb, 0); nb = 0; }
        }
    }
    if (nb) xStreamBufferSend(dc->sb, bytes, nb, 0);
}

/* AGC for recorded output only (a VoIP recording gets a gentler version, see DL_* below) -- confirmed live (2026-09-03) that 211
 * messages were coming out around -46 dBFS RMS / 20% of full-scale peak
 * even for normal speaking volume, quiet enough several people assumed
 * playback itself was broken. That level is inherent to this hardware's
 * mic gain budget (locked to 0dB PGA -- raising it hits the ES8388's
 * gain-dependent input impedance bug documented in docs/signal-levels.md
 * -- combined with the divider added to keep DTMF from clipping), not
 * something to fix by turning the mic up. Applied here to the decimated
 * 8kHz samples right before they're written, so it only ever touches what
 * lands in the WAV file -- zero effect on live call audio, DTMF detection,
 * or anything else sharing the same RX stream upstream of this.
 *
 * Two-layer design, not just dtmf.c's plain one-block-lag AGC: a smoothed
 * gain estimate (same shape as dtmf.c's, see that file's own tuning
 * comment for the rationale) handles the steady-state level, PLUS a
 * per-block hard ceiling computed from that block's own raw peak, applied
 * on top before writing. The smoothed estimate alone isn't enough for
 * recorded voice the way it is for DTMF: DTMF tones hold a stable level
 * for the whole press, so a one-block lag never really overshoots.
 * Real audio doesn't -- confirmed live: the gain smoothed to near max
 * during a quiet stretch, then a single louder transient (the tail of the
 * pre-record beep bleeding into the first ~0.6s of a recording, see
 * REC_SETTLE_MS below) got hit with that stale high gain and clipped
 * solid at full scale. The per-block ceiling means no single block can
 * ever clip regardless of how far behind the smoothed estimate is. */
#define AGC_TARGET_PEAK  20000 /* ~61% of int16 full scale -- clearly audible, real headroom below clipping */
#define AGC_CEILING_PEAK 30000 /* ~92% -- hard per-block cap, independent of the smoothed estimate */
#define AGC_GATE_PEAK     1000 /* below this, hold the smoothed estimate rather than chase noise floor */
#define AGC_MIN_GAIN_Q8     64 /* 0.25x -- still allows taming an already-loud line */
#define AGC_MAX_GAIN_Q8   2048 /* 8x -- the per-block ceiling is what actually bounds worst case, not this */
#define AGC_SMOOTH_DIV       4
/* Local recordings (211 messages) start at 8x, not 1x, and treat a block as speech once it is a few
 * times the noise floor, with a low minimum. Measured 2026-11-25: quiet counting ("1 2 3 4") peaked
 * at ~700-990 raw, just under the old fixed gate of 1000, so the gain never left 1x and the first
 * digits were inaudible; one louder block then sent the gain up all at once. Handset-to-mouth
 * distance makes the level vary this much on a real call. */
#define AGC_START_GAIN_Q8  2048 /* 8x: where a real voice settled on 2026-11-25 (gain trace) -- the cap */
#define AGC_LOCAL_GATE_MIN  250 /* raw peak; the noise floor measured about 20-70 */
/* Recordings of a caller's voice over the network (downlink): a gentler gain. The voice
 * arrives clean but quiet (measured 2026-11-10: about -35 dBFS RMS against a -69 dBFS
 * noise floor), and the full AGC above lifted line noise to speech level. So: at most
 * 4x (12 dB), and the gain only moves on blocks clearly above the noise floor, which
 * is tracked as the quietest recent block (rising slowly). */
#define DL_MAX_GAIN_Q8     1024 /* 4x */
#define DL_START_GAIN_Q8   1024 /* start at the cap, where a quiet caller settles (about -35 dBFS RMS needs more than 4x) */
#define DL_GATE_MIN_PEAK   1500
#define DL_GATE_FLOOR_MULT    4 /* a block counts as voice above this many times the noise floor */
#define DL_FLOOR_RISE_DIV   128

/* Recording starts the instant the pre-record beep's timer expires
 * (telephony.c), but the beep's own acoustic decay -- audible through the
 * earpiece, picked up by the mic -- doesn't stop that fast. Confirmed
 * live: the first ~0.6s of every recording captured that tail, loud
 * enough to clip. Discarding (not writing) this short window fixes it at
 * the source instead of just papering over it with the AGC ceiling above.
 * Doesn't change the caller-facing "starts right after the beep" behavior
 * -- recording still begins on the same event, this is well under a
 * quarter second of silence they'd talk through anyway. */
#define REC_SETTLE_MS 250

static void wr_u16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void wr_u32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

static void build_wav_header(uint8_t *h, uint32_t rate, uint32_t data_bytes)
{
    memcpy(h, "RIFF", 4);      wr_u32(h + 4, 36 + data_bytes); memcpy(h + 8, "WAVE", 4);
    memcpy(h + 12, "fmt ", 4); wr_u32(h + 16, 16);
    wr_u16(h + 20, 1);         /* PCM */
    wr_u16(h + 22, 1);         /* mono */
    wr_u32(h + 24, rate);
    wr_u32(h + 28, rate * 2);  /* byte rate = rate * channels * bytes/sample */
    wr_u16(h + 32, 2);         /* block align */
    wr_u16(h + 34, 16);        /* bits/sample */
    memcpy(h + 36, "data", 4); wr_u32(h + 40, data_bytes);
}

/* Opening the recording file takes about a second on this board's flash filesystem (measured
 * 2026-11-25: 264 ms to look the name up, 242 ms to total the used space, 421 ms to create the
 * file). Done first, that put the start of every 211 message about 1.3 s after the beep. So for
 * local (uplink) recordings the file is opened by a helper task while this one is already
 * listening, and what is heard meanwhile is held in RAM and written out once the file is ready.
 * If the buffer or the helper can't be had, it falls back to opening first. */
#define PRE_CHUNK_SAMPLES 4096   /* held as 8-bit mu-law (G.711, 1 byte a sample): half the RAM of 16-bit */
#define PRE_MAX_CHUNKS    3      /* 4 KB blocks (a big one won't fit a fragmented heap): up to 1.5 s at 8 kHz */

typedef struct {
    uint8_t *c[PRE_MAX_CHUNKS];
    uint16_t fill[PRE_MAX_CHUNKS];
    int      n_alloc, head, count;   /* a ring of blocks: when full the oldest is dropped, not the newest */
} prebuf_t;

static void pre_free(prebuf_t *p)
{
    for (int i = 0; i < p->n_alloc; i++) free(p->c[i]);
    p->n_alloc = p->head = p->count = 0;
}

static bool pre_alloc(prebuf_t *p)   /* takes what the heap offers; needs at least 2 blocks (1 s) */
{
    memset(p, 0, sizeof(*p));
    while (p->n_alloc < PRE_MAX_CHUNKS) {
        uint8_t *m = malloc(PRE_CHUNK_SAMPLES);
        if (!m) break;
        p->c[p->n_alloc++] = m;
    }
    if (p->n_alloc < 2) { pre_free(p); return false; }
    return true;
}

static void pre_add(prebuf_t *p, const int16_t *src, size_t n)
{
    while (n) {
        int last = (p->head + p->count - 1) % p->n_alloc;
        if (p->count == 0 || p->fill[last] >= PRE_CHUNK_SAMPLES) {
            if (p->count < p->n_alloc) { last = (p->head + p->count) % p->n_alloc; p->count++; }
            else { last = p->head; p->head = (p->head + 1) % p->n_alloc; }   /* full: reuse the oldest */
            p->fill[last] = 0;
        }
        size_t room = PRE_CHUNK_SAMPLES - p->fill[last];
        size_t k = n < room ? n : room;
        for (size_t i = 0; i < k; i++) p->c[last][p->fill[last] + i] = ulaw_encode(src[i]);
        p->fill[last] += (uint16_t)k;
        src += k;
        n -= k;
    }
}

static size_t pre_samples(const prebuf_t *p)
{
    size_t t = 0;
    for (int i = 0; i < p->count; i++) t += p->fill[(p->head + i) % p->n_alloc];
    return t;
}

static esp_err_t pre_flush(prebuf_t *p, storage_writer_t *w, uint32_t *bytes)
{
    int16_t tmp[128];
    for (int i = 0; i < p->count; i++) {
        int idx = (p->head + i) % p->n_alloc;
        for (size_t o = 0; o < p->fill[idx]; ) {
            size_t k = p->fill[idx] - o;
            if (k > 128) k = 128;
            for (size_t j = 0; j < k; j++) tmp[j] = ulaw_decode(p->c[idx][o + j]);
            if (storage_write_chunk(w, tmp, k * sizeof(int16_t)) != ESP_OK) return ESP_FAIL;
            *bytes += k * sizeof(int16_t);
            o += k;
        }
    }
    p->head = p->count = 0;
    return ESP_OK;
}

typedef struct {
    const char       *name;
    size_t            reserve;
    storage_writer_t *w;
    uint32_t          rate;
    atomic_int        state;    /* 0 opening, 1 ready, -1 couldn't open, -2 opened but header write failed */
} opener_t;

static void opener_task(void *arg)
{
    opener_t *o = (opener_t *)arg;
    int st;
    if (storage_write_begin(o->name, o->reserve, o->w) != ESP_OK) {
        st = -1;
    } else {
        uint8_t hdr[44];
        build_wav_header(hdr, o->rate, 0); /* sizes patched at the end */
        st = (storage_write_chunk(o->w, hdr, sizeof(hdr)) == ESP_OK) ? 1 : -2;
    }
    atomic_store(&o->state, st);   /* last touch of *o: the recorder task may free it right after */
    vTaskDelete(NULL);
}

static void rec_task(void *arg)
{
    rec_args_t *a = (rec_args_t *)arg;
    int line = a->line;
    bool ok = false;
    int64_t t_task = esp_timer_get_time(); /* timing log: where the start-up delay goes */

    uint32_t in_rate = audio_sample_rate();
    uint32_t decim = in_rate / 8000;
    if (decim < 1) decim = 1;
    uint32_t out_rate = in_rate / decim; /* ~8000 */

    int16_t *in = a->downlink ? NULL : malloc(IN_CHUNK_SAMPLES * sizeof(int16_t));
    int16_t *out = malloc((IN_CHUNK_SAMPLES / decim + 1) * sizeof(int16_t));
    storage_writer_t w = { 0 };
    bool writer_open = false;
    downlink_ctx_t dlctx = { 0 };
    bool downlink_tap_added = false;
    opener_t op = { .name = a->name, .reserve = 0, .w = &w, .rate = out_rate };
    atomic_init(&op.state, 0);
    prebuf_t pre = { 0 };    /* audio held while the helper opens the file */
    bool     async_open = false, file_ready = false;
    if ((!in && !a->downlink) || !out) { ESP_LOGE(TAG, "line %d: alloc failed", line); goto out; }

    /* Downlink: start listening before the file is opened (the open takes about a second); the
     * stream buffer holds what the caller says meanwhile. */
    if (a->downlink) {
        dlctx.decim = decim;
        dlctx.sb = xStreamBufferCreate(DOWNLINK_SB_BYTES, 1);
        if (!dlctx.sb) { ESP_LOGE(TAG, "line %d: downlink stream buffer alloc failed", line); goto out; }
        audio_add_extwrite_tap(line, downlink_push, &dlctx);
        downlink_tap_added = true;
    }

    /* Reserve room for a full max_ms clip so mid-record eviction can't clip us. */
    size_t reserve = 44 + (size_t)((uint64_t)out_rate * 2 * a->max_ms / 1000);
    op.reserve = reserve;
    if (!a->downlink) {
        if (pre_alloc(&pre)) {
            if (xTaskCreate(opener_task, "rec_open", 4096, &op, tskIDLE_PRIORITY + 2, NULL) == pdPASS) {
                async_open = true;
            } else {
                pre_free(&pre);
            }
        }
        if (!async_open)
            ESP_LOGW(TAG, "line %d: not enough free memory to listen while the file opens "
                          "(largest free block %u B): opening first", line,
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    }
    if (!async_open) {
        if (storage_write_begin(a->name, reserve, &w) != ESP_OK) {
            ESP_LOGW(TAG, "line %d: storage_write_begin('%s') failed", line, a->name);
            goto out;
        }
        writer_open = true;

        uint8_t hdr[44];
        build_wav_header(hdr, out_rate, 0); /* sizes patched at the end */
        if (storage_write_chunk(&w, hdr, sizeof(hdr)) != ESP_OK) goto out;
        file_ready = true;
    }

    ESP_LOGI(TAG, "line %d: recording '%s' (<=%u ms, %u Hz mono, %s)",
             line, a->name, (unsigned)a->max_ms, (unsigned)out_rate,
             a->downlink ? "downlink" : "uplink");
    if (file_ready)
        ESP_LOGI(TAG, "line %d: file ready %d ms after the recorder task started", line,
                 (int)((esp_timer_get_time() - t_task) / 1000));
    activity_log("Line %d: recording started", line);

    uint32_t data_bytes = 0;
    int32_t  acc = 0;      /* decimation accumulator, carried across reads */
    uint32_t acc_n = 0;
    int64_t  t0 = esp_timer_get_time();
    int64_t  quiet_since = 0; /* esp_timer_get_time() when quiet began; 0 = not quiet */
    int32_t  agc_gain_q8 = a->downlink ? DL_START_GAIN_Q8 : AGC_START_GAIN_Q8; /* see AGC_* above */
    int32_t  noise_floor = 0;   /* downlink: quietest recent block peak */
    bool     first_block = true;
    uint32_t rec_samples = 0;   /* 8 kHz samples seen so far, for the settle gap */
    int16_t  gtrace[40];        /* applied gain x10, about every 0.5 s: shows where the AGC settles */
    int      gtrace_n = 0, gtrace_blk = 0;

    while (!s_rec[line].stop) {
        int64_t elapsed_ms = (esp_timer_get_time() - t0) / 1000;
        if (elapsed_ms >= (int64_t)a->max_ms) break;

        size_t n, on = 0;
        if (a->downlink) {
            uint8_t ub[256];   /* already 8 kHz mu-law, see downlink_push() */
            n = xStreamBufferReceive(dlctx.sb, ub, sizeof(ub), pdMS_TO_TICKS(500));
            for (size_t i = 0; i < n; i++) out[on++] = ulaw_decode(ub[i]);
        } else {
            n = audio_rx_read(line, in, IN_CHUNK_SAMPLES, 500);
        }
        if (n == 0) continue; /* RX starved momentarily; keep waiting up to max_ms */
        if (first_block) {
            first_block = false;
            ESP_LOGI(TAG, "line %d: first audio block %d ms after the recorder task started", line,
                     (int)((esp_timer_get_time() - t_task) / 1000));
        }

        if (!a->downlink) {
            for (size_t i = 0; i < n; i++) {
                acc += in[i];
                if (++acc_n == decim) {
                    out[on++] = (int16_t)(acc / (int32_t)decim);
                    acc = 0;
                    acc_n = 0;
                }
            }
        }
        if (on == 0) continue;

        /* Settle gap: decimate (keeps the accumulator in sync) but discard
         * -- don't write, don't count elapsed time against silence
         * detection below -- see REC_SETTLE_MS above. */
        uint32_t blk_start = rec_samples;
        rec_samples += (uint32_t)on;
        if (blk_start < REC_SETTLE_MS * (out_rate / 1000)) continue;   /* by audio time: held audio comes in a burst */

        /* This block's own raw peak, before any gain -- drives both the
         * per-block ceiling and the smoothed estimate's next update (see
         * AGC_* comment above). */
        int32_t raw_peak = 0;
        for (size_t i = 0; i < on; i++) {
            int32_t a_ = out[i] < 0 ? -out[i] : out[i];
            if (a_ > raw_peak) raw_peak = a_;
        }

        /* Gain actually applied to THIS block: the smoothed estimate,
         * capped so this specific block's own peak can't clear
         * AGC_CEILING_PEAK no matter how stale that estimate is. */
        int32_t block_gain_q8 = agc_gain_q8;
        if (raw_peak > 0) {
            int32_t ceiling_gain = (int32_t)(((int64_t)AGC_CEILING_PEAK << 8) / raw_peak);
            if (block_gain_q8 > ceiling_gain) block_gain_q8 = ceiling_gain;
        }
        for (size_t i = 0; i < on; i++) {
            int32_t v = ((int32_t)out[i] * block_gain_q8) >> 8;
            if (v > INT16_MAX) v = INT16_MAX;
            else if (v < INT16_MIN) v = INT16_MIN;
            out[i] = (int16_t)v;
        }
        if (gtrace_n < 40 && (gtrace_blk++ % 5) == 0)
            gtrace[gtrace_n++] = (int16_t)(block_gain_q8 * 10 / 256);
        /* Re-estimate the smoothed gain for the *next* block from this
         * block's raw (pre-gain) peak -- one-block lag, same shape as
         * dtmf.c's AGC, but no longer the only thing standing between a
         * transient and clipping (the ceiling above handles that now). */
        int32_t chunk_peak = raw_peak;
        int32_t gate = AGC_GATE_PEAK, max_gain = AGC_MAX_GAIN_Q8;
        if (noise_floor == 0 || chunk_peak < noise_floor) noise_floor = chunk_peak;
        else noise_floor += (chunk_peak - noise_floor) / DL_FLOOR_RISE_DIV;
        gate = noise_floor * DL_GATE_FLOOR_MULT;
        if (a->downlink) {
            if (gate < DL_GATE_MIN_PEAK) gate = DL_GATE_MIN_PEAK;
            max_gain = DL_MAX_GAIN_Q8;
        } else if (gate < AGC_LOCAL_GATE_MIN) {
            gate = AGC_LOCAL_GATE_MIN;
        }
        if (chunk_peak >= gate) {
            int32_t target = (int32_t)(((int64_t)AGC_TARGET_PEAK << 8) / chunk_peak);
            if (target < AGC_MIN_GAIN_Q8) target = AGC_MIN_GAIN_Q8;
            if (target > max_gain) target = max_gain;
            agc_gain_q8 += (target - agc_gain_q8) / AGC_SMOOTH_DIV;
        }

        if (!file_ready) {
            int st = atomic_load(&op.state);
            if (st == -1) {
                ESP_LOGW(TAG, "line %d: storage_write_begin('%s') failed", line, a->name);
                goto out;
            }
            if (st != 0) {                       /* the helper finished: the file is open */
                writer_open = true;
                if (st == -2) goto out;
                file_ready = true;
                ESP_LOGI(TAG, "line %d: file ready %d ms after the recorder task started "
                              "(%u ms of audio was held meanwhile)", line,
                         (int)((esp_timer_get_time() - t_task) / 1000),
                         (unsigned)(pre_samples(&pre) / (out_rate / 1000)));
                if (pre_flush(&pre, &w, &data_bytes) != ESP_OK) goto out;
            }
        }
        if (file_ready) {
            if (storage_write_chunk(&w, out, on * sizeof(int16_t)) != ESP_OK) goto out;
            data_bytes += on * sizeof(int16_t);
        } else {
            pre_add(&pre, out, on);               /* file still opening: hold it */
        }

        /* Trailing-silence auto-stop, after a min duration so a slow start to
         * speaking doesn't cut it off. */
        if (a->silence_ms && elapsed_ms > 1000) {
            double sum = 0;
            for (size_t i = 0; i < on; i++) { double s = out[i]; sum += s * s; }
            double rms = sqrt(sum / on);
            int64_t now = esp_timer_get_time();
            if (rms < SILENCE_RMS) {
                if (!quiet_since) quiet_since = now;
                else if ((now - quiet_since) / 1000 >= (int64_t)a->silence_ms) break;
            } else {
                quiet_since = 0;
            }
        }
    }

    if (async_open && !file_ready) {             /* ended before the file was open: finish it properly */
        while (atomic_load(&op.state) == 0) vTaskDelay(pdMS_TO_TICKS(20));
        int st = atomic_load(&op.state);
        if (st == -1) goto out;
        writer_open = true;
        if (st == -2) goto out;
        file_ready = true;
        if (pre_flush(&pre, &w, &data_bytes) != ESP_OK) goto out;
    }
    storage_write_end(&w);
    writer_open = false;

    /* Patch the header's RIFF/data sizes now that the length is known. */
    uint8_t sz[4];
    wr_u32(sz, 36 + data_bytes); storage_patch(a->name, 4, sz, 4);
    wr_u32(sz, data_bytes);      storage_patch(a->name, 40, sz, 4);

    ok = (data_bytes > 0);
    ESP_LOGI(TAG, "line %d: recorded '%s' (%u bytes PCM, %.1f s)",
             line, a->name, (unsigned)data_bytes,
             (double)data_bytes / 2.0 / (double)out_rate);
    {
        char tr[220]; int o = 0;
        for (int i = 0; i < gtrace_n && o < (int)sizeof(tr) - 8; i++)
            o += snprintf(tr + o, sizeof(tr) - o, "%d.%d ", gtrace[i] / 10, gtrace[i] % 10);
        ESP_LOGI(TAG, "line %d: gain trace (x, every ~0.5 s): %s", line, tr);
    }
    activity_log("Line %d: recording saved (%.1fs)",
                  line, (double)data_bytes / 2.0 / (double)out_rate);

out:
    if (downlink_tap_added) {
        audio_remove_extwrite_tap(line);
        if (dlctx.sb) vStreamBufferDelete(dlctx.sb);
    }
    if (async_open) {
        while (atomic_load(&op.state) == 0) vTaskDelay(pdMS_TO_TICKS(20));
        /* The helper opened it but we never got as far as noticing: still ours to clean up. Not after a
         * normal finish (file_ready: written, closed, kept) -- aborting that would delete the recording. */
        if (!writer_open && !file_ready && atomic_load(&op.state) != -1) writer_open = true;
    }
    if (writer_open) storage_write_abort(&w);
    pre_free(&pre);
    free(in);
    free(out);
    s_rec[line].active = false;
    if (a->cb) a->cb(line, ok, a->ctx);
    free(a);
    vTaskDelete(NULL);
}

static esp_err_t recorder_start_ex(int line, const char *storage_name, uint32_t max_ms,
                                    uint32_t silence_ms, bool downlink,
                                    recorder_done_cb_t done_cb, void *ctx)
{
    if (line < 0 || line >= AUDIO_NUM_LINES) return ESP_ERR_INVALID_ARG;
    if (s_rec[line].active) return ESP_ERR_INVALID_STATE;

    rec_args_t *a = malloc(sizeof(*a));
    if (!a) return ESP_ERR_NO_MEM;
    a->line = line;
    strncpy(a->name, storage_name, sizeof(a->name) - 1);
    a->name[sizeof(a->name) - 1] = 0;
    a->max_ms = max_ms;
    a->silence_ms = silence_ms;
    a->downlink = downlink;
    a->cb = done_cb;
    a->ctx = ctx;

    s_rec[line].active = true;
    s_rec[line].stop = false;

    if (xTaskCreate(rec_task, "recorder", 5120, a, tskIDLE_PRIORITY + 3, NULL) != pdPASS) {
        s_rec[line].active = false;
        free(a);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t recorder_start(int line, const char *storage_name, uint32_t max_ms,
                         uint32_t silence_ms, recorder_done_cb_t done_cb, void *ctx)
{
    return recorder_start_ex(line, storage_name, max_ms, silence_ms, false, done_cb, ctx);
}

esp_err_t recorder_start_downlink(int line, const char *storage_name, uint32_t max_ms,
                                  uint32_t silence_ms, recorder_done_cb_t done_cb, void *ctx)
{
    return recorder_start_ex(line, storage_name, max_ms, silence_ms, true, done_cb, ctx);
}

void recorder_stop(int line)
{
    if (line < 0 || line >= AUDIO_NUM_LINES) return;
    s_rec[line].stop = true;
}

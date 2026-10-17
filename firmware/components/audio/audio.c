#include "audio.h"
#include "board_pins.h"
#include "dtmf.h"
#include "aec.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "driver/i2s_std.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>

static const char *TAG = "audio";

typedef enum {
    SRC_SILENCE = 0,
    SRC_TONE,
    SRC_DTMF,
    SRC_BRIDGE,
    SRC_EXTERNAL,
} tx_src_t;

typedef struct {
    tx_src_t        src;
    progress_tone_t prog;     /* SRC_TONE */
    tone_osc_t      dtmf;     /* SRC_DTMF */
    uint32_t        dtmf_left;/* samples remaining in DTMF burst */
    int             bridge_from;
    StreamBufferHandle_t ext; /* SRC_EXTERNAL downlink */

    audio_rx_tap_fn rx_tap;
    void           *rx_ctx;
    audio_rx_tap_fn diag_tap; /* second, independent slot -- see audio_add_diag_tap() */
    void           *diag_ctx;
    audio_rx_tap_fn downlink_tap; /* see audio_add_downlink_tap() */
    void           *downlink_ctx;
    audio_rx_tap_fn extwrite_tap; /* see audio_add_extwrite_tap() */
    void           *extwrite_ctx;
    StreamBufferHandle_t rx;  /* uplink capture for voip/record */
    uint16_t        rx_level;
    /* AEC path: 8 kHz echo-cancelled uplink, fed only while the canceller is
     * enabled. Kept separate from `rx` above so the 48 kHz raw capture stays
     * exactly that -- recorder/diag taps/echoprobe must keep seeing the true
     * uncancelled mic signal, or they'd be measuring the canceller instead of
     * the hardware. */
    StreamBufferHandle_t aec_out;
    int32_t         dec_ref_acc, dec_near_acc; /* 48k->8k decimation accumulators */
    uint32_t        dec_n;
    volatile bool   inject_uplink; /* see audio_set_uplink_inject() */
    /* Voice effect (audio_set_voice_fx()): fx_down shapes what this line's
     * earpiece hears from a bridge/VoIP peer; fx_up/fx_up8 shape the uplink
     * VoIP reads (48 kHz raw or 8 kHz echo-cancelled). Allocated on first use. */
    voice_fx_t        fx;
    voice_fx_state_t *fx_down, *fx_up, *fx_up8;
    /* When something (the VoIP RTP task, the recorder) last asked for this
     * line's uplink. The uplink effect costs as much as the downlink one
     * (~2 ms of every 10 ms frame for pitch effects), so on a call between two
     * local phones, where nobody reads the uplink, it's skipped. */
    volatile int64_t uplink_read_us;
    /* Frames of external audio (VoIP, player) the engine had to fill with
     * silence because the source hadn't delivered in time; see audio_ext_underruns(). */
    volatile uint32_t ext_underruns;
} line_t;

static line_t s_line[AUDIO_NUM_LINES];
static int16_t *s_rx_mono[AUDIO_NUM_LINES]; /* last RX frame, for bridge */
static int16_t *s_fx_tmp[AUDIO_NUM_LINES];  /* uplink copy for the voice effect */

static i2s_chan_handle_t s_tx, s_rx;
static uint32_t s_sr = 48000;
static size_t   s_frame = 480;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_task;

/* Test-only digital loopback: when on, the RX processing chain (level meter,
 * DTMF detector tap, rx stream buffer) is fed the TX we just synthesized
 * instead of the codec's real ADC input -- so an automated test can play a
 * tone/DTMF/clip and confirm it's actually produced and detectable, without
 * a physical handset. This is a DSP-domain loopback (TX buffer -> RX
 * processing); it does NOT exercise the analog codec I/O, which is what the
 * mic->speaker `loopback` command + a listen are for. */
static volatile bool s_test_loopback;

/* Idle pause (see audio_allow_pause()). The task normally runs a full
 * read-process-write cycle every 10ms forever, even with every phone hung up
 * -- the biggest steady CPU load on the board. When the layer above says all
 * lines are idle AND nothing has touched audio for AUDIO_PAUSE_AFTER_MS, the
 * task disables both I2S channels and sleeps. Anything that wants audio
 * wakes it through audio_wake(), called from every setter that starts a
 * source/tap and from audio_rx_read(), so tools that use audio with the
 * phones on-hook (bring-up `tone`, `capture`, test loopback, the web UI's
 * test recording) keep working without knowing any of this exists. */
#define AUDIO_PAUSE_AFTER_MS 2000
static volatile bool    s_allow_pause;
static volatile bool    s_paused;
static volatile int64_t s_last_use_us;   /* last time something asked for audio */

static void audio_wake(void)
{
    s_last_use_us = esp_timer_get_time();
    if (s_paused && s_task) xTaskNotifyGive(s_task);
}

uint32_t audio_sample_rate(void) { return s_sr; }

/* ---- source setters (short critical sections) ---- */
void audio_tx_silence(int line)
{
    if (line < 0 || line >= AUDIO_NUM_LINES) return;
    portENTER_CRITICAL(&s_mux);
    s_line[line].src = SRC_SILENCE;
    portEXIT_CRITICAL(&s_mux);
}

void audio_tx_tone(int line, call_progress_t tone)
{
    audio_wake();
    if (line < 0 || line >= AUDIO_NUM_LINES) return;
    progress_tone_t p;
    progress_tone_start(&p, s_sr, tone);
    portENTER_CRITICAL(&s_mux);
    s_line[line].prog = p;
    s_line[line].src = SRC_TONE;
    portEXIT_CRITICAL(&s_mux);
}

void audio_tx_dtmf(int line, char key, uint32_t ms)
{
    audio_wake();
    if (line < 0 || line >= AUDIO_NUM_LINES) return;
    tone_osc_t o;
    if (!dtmf_make_osc(&o, s_sr, key, 0.5f)) return;
    uint32_t n = (uint32_t)((uint64_t)ms * s_sr / 1000);
    portENTER_CRITICAL(&s_mux);
    s_line[line].dtmf = o;
    s_line[line].dtmf_left = n;
    s_line[line].src = SRC_DTMF;
    portEXIT_CRITICAL(&s_mux);
}

void audio_tx_burst(int line, float f1, float f2, float amp, uint32_t ms)
{
    audio_wake();
    if (line < 0 || line >= AUDIO_NUM_LINES) return;
    tone_osc_t o;
    tone_osc_init(&o, s_sr, f1, f2, amp);
    uint32_t n = (uint32_t)((uint64_t)ms * s_sr / 1000);
    portENTER_CRITICAL(&s_mux);
    s_line[line].dtmf = o;
    s_line[line].dtmf_left = n;
    s_line[line].src = SRC_DTMF;
    portEXIT_CRITICAL(&s_mux);
}

void audio_tx_bridge(int line, int from_line, bool enable)
{
    if (enable) audio_wake();
    if (line < 0 || line >= AUDIO_NUM_LINES) return;
    portENTER_CRITICAL(&s_mux);
    if (enable) {
        s_line[line].bridge_from = from_line;
        s_line[line].src = SRC_BRIDGE;
    } else if (s_line[line].src == SRC_BRIDGE) {
        s_line[line].src = SRC_SILENCE;
    }
    portEXIT_CRITICAL(&s_mux);
}

esp_err_t audio_set_voice_fx(int line, voice_fx_t fx)
{
    if (line < 0 || line >= AUDIO_NUM_LINES) return ESP_ERR_INVALID_ARG;
    line_t *l = &s_line[line];
    if (fx != VOICE_FX_NONE && !l->fx_down) {
        voice_fx_state_t *down = voice_fx_create(s_sr);
        voice_fx_state_t *up   = voice_fx_create(s_sr);
        voice_fx_state_t *up8  = voice_fx_create(8000);
        if (!down || !up || !up8) {
            /* voice_fx states are never freed once in use, so a partial
             * allocation can't be handed out; leak them (tiny, OOM path). */
            return ESP_ERR_NO_MEM;
        }
        portENTER_CRITICAL(&s_mux);
        l->fx_down = down;
        l->fx_up = up;
        l->fx_up8 = up8;
        portEXIT_CRITICAL(&s_mux);
    }
    portENTER_CRITICAL(&s_mux);
    l->fx = fx;
    portEXIT_CRITICAL(&s_mux);
    return ESP_OK;
}

void audio_voice_fx_stats(int line, bool downlink, voice_fx_stats_t *out)
{
    memset(out, 0, sizeof(*out));
    if (line < 0 || line >= AUDIO_NUM_LINES) return;
    voice_fx_state_t *s = downlink ? s_line[line].fx_down : s_line[line].fx_up;
    voice_fx_take_stats(s, out);
    out->fx = s_line[line].fx;
}

voice_fx_t audio_voice_fx(int line)
{
    return (line >= 0 && line < AUDIO_NUM_LINES) ? s_line[line].fx : VOICE_FX_NONE;
}

void audio_tx_external(int line, bool enable)
{
    if (enable) audio_wake();
    if (line < 0 || line >= AUDIO_NUM_LINES) return;
    portENTER_CRITICAL(&s_mux);
    s_line[line].src = enable ? SRC_EXTERNAL : SRC_SILENCE;
    portEXIT_CRITICAL(&s_mux);
}

void audio_set_uplink_inject(int line, bool on)
{
    if (on) audio_wake();
    if (line < 0 || line >= AUDIO_NUM_LINES) return;
    s_line[line].inject_uplink = on; /* read in audio_task's RX-processing loop; no lock needed for a single bool */
}

uint32_t audio_ext_underruns(int line)
{
    if (line < 0 || line >= AUDIO_NUM_LINES) return 0;
    return s_line[line].ext_underruns;
}

size_t audio_ext_write(int line, const int16_t *mono, size_t n)
{
    if (line < 0 || line >= AUDIO_NUM_LINES || !s_line[line].ext) return 0;
    if (s_line[line].extwrite_tap) {
        s_line[line].extwrite_tap(line, mono, n, s_line[line].extwrite_ctx);
    }
    return xStreamBufferSend(s_line[line].ext, mono, n * sizeof(int16_t), 0)
           / sizeof(int16_t);
}

size_t audio_ext_write_downlink(int line, const int16_t *mono, size_t n)
{
    if (line < 0 || line >= AUDIO_NUM_LINES || !s_line[line].ext) return 0;
    if (s_line[line].inject_uplink) {
        /* Answering machine on a VoIP call: the greeting/beep are being
         * queued by the player, and this is the CALLER'S voice. Queuing it
         * too would interleave the two in one buffer (the caller hears the
         * greeting chopped up and their own voice echoed back). The
         * recorder still gets it, through the tap. */
        if (s_line[line].extwrite_tap) s_line[line].extwrite_tap(line, mono, n, s_line[line].extwrite_ctx);
        return n;
    }
    return audio_ext_write(line, mono, n);
}

void audio_set_rx_tap(int line, audio_rx_tap_fn fn, void *ctx)
{
    if (line < 0 || line >= AUDIO_NUM_LINES) return;
    portENTER_CRITICAL(&s_mux);
    s_line[line].rx_tap = fn;
    s_line[line].rx_ctx = ctx;
    portEXIT_CRITICAL(&s_mux);
}

void audio_add_diag_tap(int line, audio_rx_tap_fn fn, void *ctx)
{
    audio_wake();
    if (line < 0 || line >= AUDIO_NUM_LINES) return;
    portENTER_CRITICAL(&s_mux);
    s_line[line].diag_tap = fn;
    s_line[line].diag_ctx = ctx;
    portEXIT_CRITICAL(&s_mux);
}

void audio_remove_diag_tap(int line)
{
    if (line < 0 || line >= AUDIO_NUM_LINES) return;
    portENTER_CRITICAL(&s_mux);
    s_line[line].diag_tap = NULL;
    s_line[line].diag_ctx = NULL;
    portEXIT_CRITICAL(&s_mux);
}

void audio_add_downlink_tap(int line, audio_rx_tap_fn fn, void *ctx)
{
    audio_wake();
    if (line < 0 || line >= AUDIO_NUM_LINES) return;
    portENTER_CRITICAL(&s_mux);
    s_line[line].downlink_tap = fn;
    s_line[line].downlink_ctx = ctx;
    portEXIT_CRITICAL(&s_mux);
}

void audio_remove_downlink_tap(int line)
{
    if (line < 0 || line >= AUDIO_NUM_LINES) return;
    portENTER_CRITICAL(&s_mux);
    s_line[line].downlink_tap = NULL;
    s_line[line].downlink_ctx = NULL;
    portEXIT_CRITICAL(&s_mux);
}

void audio_add_extwrite_tap(int line, audio_rx_tap_fn fn, void *ctx)
{
    audio_wake();
    if (line < 0 || line >= AUDIO_NUM_LINES) return;
    portENTER_CRITICAL(&s_mux);
    s_line[line].extwrite_tap = fn;
    s_line[line].extwrite_ctx = ctx;
    portEXIT_CRITICAL(&s_mux);
}

void audio_remove_extwrite_tap(int line)
{
    if (line < 0 || line >= AUDIO_NUM_LINES) return;
    portENTER_CRITICAL(&s_mux);
    s_line[line].extwrite_tap = NULL;
    s_line[line].extwrite_ctx = NULL;
    portEXIT_CRITICAL(&s_mux);
}

/* record_tap-style plumbing, private to audio_echo_probe() -- deliberately
 * not reusing audio_rx_read()'s stream buffer (see its own doc comment: one
 * reader max, and the RTP task may already own it) or bringup.c's identical
 * helper for `record` (that one streams to storage in chunks; this one just
 * needs a single bounded in-RAM capture). */
typedef struct {
    StreamBufferHandle_t sb;
} echo_probe_tap_ctx_t;

static void echo_probe_tap(int line, const int16_t *mono, size_t n, void *ctx)
{
    (void)line;
    echo_probe_tap_ctx_t *c = ctx;
    xStreamBufferSend(c->sb, mono, n * sizeof(int16_t), 0);
}

bool audio_echo_probe(int line, uint32_t burst_hz, uint32_t burst_ms,
                       uint32_t listen_ms, int16_t *cap_out, size_t cap_out_n,
                       audio_echo_probe_t *out)
{
    if (line < 0 || line >= AUDIO_NUM_LINES || !out) return false;
    memset(out, 0, sizeof(*out));

    uint32_t sr = s_sr;
    size_t n_samples = (size_t)((uint64_t)sr * listen_ms / 1000);
    int16_t *cap = malloc(n_samples * sizeof(int16_t));
    if (!cap) return false;

    /* Sized to the whole capture, not just enough for typical producer/
     * consumer pacing: echo_probe_tap()'s xStreamBufferSend() is
     * non-blocking (the audio task can never block), so an undersized
     * buffer here would silently drop samples and corrupt the capture
     * rather than raise an error. */
    StreamBufferHandle_t sb = xStreamBufferCreate(n_samples * sizeof(int16_t), 1);
    if (!sb) { free(cap); return false; }

    echo_probe_tap_ctx_t ctx = { .sb = sb };
    audio_add_diag_tap(line, echo_probe_tap, &ctx);
    /* Tap first, THEN emit: starting the burst before the tap is armed would
     * lose the very leading edge this measurement is all about. */
    audio_tx_burst(line, (float)burst_hz, 0.0f, 0.8f, burst_ms);

    size_t got = 0;
    while (got < n_samples) {
        size_t want = n_samples - got;
        size_t n = xStreamBufferReceive(sb, cap + got, want * sizeof(int16_t),
                                        pdMS_TO_TICKS(500));
        if (n == 0) break;
        got += n / sizeof(int16_t);
    }
    audio_remove_diag_tap(line);
    vStreamBufferDelete(sb);
    audio_tx_silence(line);

    if (got == 0) { free(cap); return false; }

    /* Peak + noise floor. The floor is taken from the first 2 ms, which is
     * always before any plausible echo return (bulk delay is dominated by
     * DMA buffering, tens of ms) -- so it measures ambient/self-noise, not
     * the thing we're looking for. */
    size_t floor_n = (size_t)(sr * 2 / 1000);
    if (floor_n > got) floor_n = got;
    uint64_t acc = 0;
    for (size_t i = 0; i < floor_n; i++) acc += (uint64_t)((int32_t)cap[i] * cap[i]);
    uint32_t noise_rms = floor_n ? (uint32_t)sqrt((double)acc / (double)floor_n) : 0;

    int32_t peak = 0; size_t peak_i = 0;
    for (size_t i = 0; i < got; i++) {
        int32_t a = cap[i] < 0 ? -cap[i] : cap[i];
        if (a > peak) { peak = a; peak_i = i; }
    }

    /* Tail: last sample still above max(4x noise floor, 5% of peak), scanned
     * from the end so a mid-burst dip doesn't cut it short. */
    int32_t thresh = (int32_t)noise_rms * 4;
    if (thresh < peak / 20) thresh = peak / 20;
    size_t tail_i = peak_i;
    for (size_t i = got; i > peak_i; i--) {
        int32_t a = cap[i - 1] < 0 ? -cap[i - 1] : cap[i - 1];
        if (a > thresh) { tail_i = i - 1; break; }
    }

    out->leak_detected = peak >= (int32_t)noise_rms * 4;
    out->noise_rms = noise_rms;
    out->peak = peak;
    out->delay_ms = (double)peak_i * 1000.0 / (double)sr;
    out->tail_ms = (double)(tail_i - peak_i) * 1000.0 / (double)sr;

    if (cap_out && cap_out_n) {
        size_t n_copy = (got < cap_out_n) ? got : cap_out_n;
        memcpy(cap_out, cap, n_copy * sizeof(int16_t));
        /* A short capture (stream stalled before n_samples) only fills
         * n_copy of the caller's buffer -- zero the rest so a consumer
         * plotting/scanning it (e.g. bringup's envelope chart) sees clean
         * silence instead of whatever was previously in that memory. */
        if (n_copy < cap_out_n) {
            memset(cap_out + n_copy, 0, (cap_out_n - n_copy) * sizeof(int16_t));
        }
    }
    free(cap);
    return true;
}

size_t audio_rx_read(int line, int16_t *mono, size_t n, uint32_t timeout_ms)
{
    audio_wake();
    if (line < 0 || line >= AUDIO_NUM_LINES || !s_line[line].rx) return 0;
    s_line[line].uplink_read_us = esp_timer_get_time();
    size_t got = xStreamBufferReceive(s_line[line].rx, mono, n * sizeof(int16_t),
                                      pdMS_TO_TICKS(timeout_ms)) / sizeof(int16_t);
    s_line[line].uplink_read_us = esp_timer_get_time();
    return got;
}

size_t audio_aec_read(int line, int16_t *mono, size_t n, uint32_t timeout_ms)
{
    if (line < 0 || line >= AUDIO_NUM_LINES || !s_line[line].aec_out) return 0;
    s_line[line].uplink_read_us = esp_timer_get_time();
    size_t got = xStreamBufferReceive(s_line[line].aec_out, mono, n * sizeof(int16_t),
                                      pdMS_TO_TICKS(timeout_ms)) / sizeof(int16_t);
    s_line[line].uplink_read_us = esp_timer_get_time();
    return got;
}

uint16_t audio_rx_level(int line)
{
    if (line < 0 || line >= AUDIO_NUM_LINES) return 0;
    return s_line[line].rx_level;
}

void audio_set_test_loopback(bool on)
{
    if (on) audio_wake();
    s_test_loopback = on;
}

bool audio_test_loopback(void)
{
    return s_test_loopback;
}

/* Fill one line's mono TX frame according to its current source. */
static void fill_tx_line(int line, int16_t *out, size_t n)
{
    /* Copy out the volatile bits under lock, then synthesize outside it. */
    line_t snap;
    portENTER_CRITICAL(&s_mux);
    snap = s_line[line];
    portEXIT_CRITICAL(&s_mux);

    switch (snap.src) {
        case SRC_TONE: {
            progress_tone_fill(&snap.prog, out, n);
            portENTER_CRITICAL(&s_mux);
            if (s_line[line].src == SRC_TONE) s_line[line].prog = snap.prog;
            portEXIT_CRITICAL(&s_mux);
            break;
        }
        case SRC_DTMF: {
            size_t emit = (snap.dtmf_left < n) ? snap.dtmf_left : n;
            if (emit) tone_osc_fill(&snap.dtmf, out, emit);
            for (size_t i = emit; i < n; i++) out[i] = 0;
            portENTER_CRITICAL(&s_mux);
            if (s_line[line].src == SRC_DTMF) {
                s_line[line].dtmf = snap.dtmf;
                s_line[line].dtmf_left = (snap.dtmf_left > emit) ? snap.dtmf_left - emit : 0;
                if (s_line[line].dtmf_left == 0) s_line[line].src = SRC_SILENCE;
            }
            portEXIT_CRITICAL(&s_mux);
            break;
        }
        case SRC_BRIDGE: {
            int from = snap.bridge_from;
            if (from >= 0 && from < AUDIO_NUM_LINES && s_rx_mono[from]) {
                memcpy(out, s_rx_mono[from], n * sizeof(int16_t));
                voice_fx_process(snap.fx_down, snap.fx, out, n);
            } else {
                memset(out, 0, n * sizeof(int16_t));
            }
            break;
        }
        case SRC_EXTERNAL: {
            size_t got = 0;
            if (snap.ext) {
                got = xStreamBufferReceive(snap.ext, out, n * sizeof(int16_t), 0)
                      / sizeof(int16_t);
            }
            if (got < n) s_line[line].ext_underruns++;
            for (size_t i = got; i < n; i++) out[i] = 0; /* underrun -> silence */
            voice_fx_process(snap.fx_down, snap.fx, out, n);
            break;
        }
        case SRC_SILENCE:
        default:
            memset(out, 0, n * sizeof(int16_t));
            break;
    }
}

static uint16_t rms_of(const int16_t *x, size_t n)
{
    if (!n) return 0;
    uint64_t acc = 0;
    for (size_t i = 0; i < n; i++) acc += (int32_t)x[i] * x[i];
    return (uint16_t)sqrt((double)acc / (double)n);
}

/* Something still needs the pipeline running, independent of what the layer
 * above says. The telephony DTMF rx_tap is deliberately NOT counted -- it's
 * registered permanently on every line, and DTMF only matters off-hook,
 * which the layer above already reports as not-idle. */
static bool audio_busy(void)
{
    if (s_test_loopback) return true;
    for (int i = 0; i < AUDIO_NUM_LINES; i++) {
        const line_t *l = &s_line[i];
        if (l->src != SRC_SILENCE || l->inject_uplink || l->diag_tap ||
            l->downlink_tap || l->extwrite_tap) {
            return true;
        }
    }
    return (esp_timer_get_time() - s_last_use_us) < (int64_t)AUDIO_PAUSE_AFTER_MS * 1000;
}

void audio_allow_pause(bool allow)
{
    if (!allow) s_last_use_us = esp_timer_get_time();
    s_allow_pause = allow;
    if (!allow && s_paused && s_task) xTaskNotifyGive(s_task);
}

bool audio_is_paused(void) { return s_paused; }

static void audio_task(void *arg)
{
    const size_t n = s_frame;
    int16_t *txL = malloc(n * sizeof(int16_t));
    int16_t *txR = malloc(n * sizeof(int16_t));
    int16_t *interleaved = malloc(n * 2 * sizeof(int16_t));
    if (!txL || !txR || !interleaved) {
        ESP_LOGE(TAG, "frame buffer alloc failed");
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        bool want_pause = s_allow_pause && !audio_busy();
        if (!s_paused && want_pause) {
            i2s_channel_disable(s_tx);
            i2s_channel_disable(s_rx);
            s_paused = true;
            ESP_LOGI(TAG, "all lines idle -- audio pipeline paused");
        }
        if (s_paused) {
            if (want_pause) {
                /* Woken early by audio_wake()/audio_allow_pause(false); the
                 * timeout just re-checks in case a flag changed silently. */
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500));
                continue;
            }
            int64_t t0 = esp_timer_get_time();
            i2s_channel_enable(s_tx);
            i2s_channel_enable(s_rx);
            s_paused = false;
            ESP_LOGI(TAG, "audio pipeline resumed (%lld us)",
                     (long long)(esp_timer_get_time() - t0));
        }

        /* Always drain the codec RX so the I2S channel can't back up, even
         * when we're going to substitute the loopback source below. */
        size_t bytes_read = 0, got = 0;
        if (i2s_channel_read(s_rx, interleaved, n * 2 * sizeof(int16_t),
                             &bytes_read, pdMS_TO_TICKS(100)) == ESP_OK) {
            got = bytes_read / (2 * sizeof(int16_t));
            for (int line = 0; line < AUDIO_NUM_LINES; line++) {
                for (size_t i = 0; i < got; i++) {
                    s_rx_mono[line][i] = interleaved[2 * i + line];
                }
            }
        }

        /* Build TX. */
        fill_tx_line(0, txL, n);
        fill_tx_line(1, txR, n);

        /* Downlink tap: observes exactly what's about to be played on this
         * line -- for a call, the decoded network audio, before it ever
         * reaches the DAC/earpiece. Independent of test_loopback (which
         * only affects the uplink/RX-processing side below) and of the
         * uplink-side rx_tap/diag_tap (which observe the mic/uplink signal,
         * the opposite direction) -- see audio_add_downlink_tap()'s comment
         * in audio.h for what this is for. */
        if (s_line[0].downlink_tap) s_line[0].downlink_tap(0, txL, n, s_line[0].downlink_ctx);
        if (s_line[1].downlink_tap) s_line[1].downlink_tap(1, txR, n, s_line[1].downlink_ctx);

        /* RX processing: real ADC input, or (test loopback) the TX we just
         * built, so a played tone/DTMF/clip is level-metered, DTMF-detected
         * and readable back without a handset. */
        for (int line = 0; line < AUDIO_NUM_LINES; line++) {
            /* Per-line uplink injection (audio_set_uplink_inject(), for the
             * answering machine's auto-answer path) does the exact same
             * TX-becomes-RX swap as the global test_loopback below, just
             * scoped to one line -- either one substitutes this line's own
             * current TX output for its mic input on the uplink side. */
            bool loop = s_test_loopback || s_line[line].inject_uplink;
            size_t rxn = loop ? n : got;
            const int16_t *src = loop ? (line == 0 ? txL : txR) : s_rx_mono[line];
            s_line[line].rx_level = rms_of(src, rxn);
            if (rxn && s_line[line].rx_tap) {
                s_line[line].rx_tap(line, src, rxn, s_line[line].rx_ctx);
            }
            if (rxn && s_line[line].diag_tap) {
                s_line[line].diag_tap(line, src, rxn, s_line[line].diag_ctx);
            }
            if (rxn && s_line[line].rx) {
                /* Taps above keep the raw mic (DTMF detection must still
                 * hear the '0' that turns an effect off); only the uplink
                 * VoIP reads gets the effect. */
                const int16_t *up = src;
                bool wanted = esp_timer_get_time() - s_line[line].uplink_read_us < 500000;
                if (wanted && s_line[line].fx != VOICE_FX_NONE && s_line[line].fx_up && s_fx_tmp[line]) {
                    memcpy(s_fx_tmp[line], src, rxn * sizeof(int16_t));
                    voice_fx_process(s_line[line].fx_up, s_line[line].fx, s_fx_tmp[line], rxn);
                    up = s_fx_tmp[line];
                }
                xStreamBufferSend(s_line[line].rx, up, rxn * sizeof(int16_t), 0);
            }
            /* Echo cancellation. Both signals are taken from HERE on purpose:
             * `tx` is the frame about to be handed to I2S (what will actually
             * be played), and `src` is the frame the mic just captured -- same
             * task, same iteration, so the offset between them is the fixed
             * hardware pipeline delay that aec_set_delay_ms() compensates.
             * Taking the reference from where RTP arrives instead would add
             * the variable playout-buffer depth and no fixed delay could
             * align it. Decimated 6:1 to the canceller's 8 kHz (see aec.h for
             * why it doesn't run at 48 kHz). */
            if (rxn && s_line[line].aec_out && aec_is_enabled(line)) {
                const int16_t *tx = (line == 0) ? txL : txR;
                int16_t ref8[AUDIO_AEC_BLOCK], near8[AUDIO_AEC_BLOCK];
                size_t m = 0;
                for (size_t i = 0; i < rxn && m < AUDIO_AEC_BLOCK; i++) {
                    s_line[line].dec_ref_acc  += tx[i];
                    s_line[line].dec_near_acc += src[i];
                    if (++s_line[line].dec_n == 6) {
                        ref8[m]  = (int16_t)(s_line[line].dec_ref_acc / 6);
                        near8[m] = (int16_t)(s_line[line].dec_near_acc / 6);
                        m++;
                        s_line[line].dec_ref_acc = s_line[line].dec_near_acc = 0;
                        s_line[line].dec_n = 0;
                    }
                }
                if (m) {
                    /* While the answering machine injects its own TX as the
                     * uplink, "mic" and "speaker" are the same signal: the
                     * canceller would subtract the greeting from itself and
                     * the caller would hear it chopped and ~25 dB down. */
                    if (!s_line[line].inject_uplink)
                        aec_process(line, ref8, near8, near8, m); /* in-place */
                    if (esp_timer_get_time() - s_line[line].uplink_read_us < 500000)
                        voice_fx_process(s_line[line].fx_up8, s_line[line].fx, near8, m);
                    xStreamBufferSend(s_line[line].aec_out, near8, m * sizeof(int16_t), 0);
                }
            }
        }

        /* Write TX. */
        for (size_t i = 0; i < n; i++) {
            interleaved[2 * i + 0] = txL[i];
            interleaved[2 * i + 1] = txR[i];
        }
        size_t written = 0;
        i2s_channel_write(s_tx, interleaved, n * 2 * sizeof(int16_t),
                          &written, pdMS_TO_TICKS(100));
    }
}

esp_err_t audio_init(const audio_config_t *cfg)
{
    audio_config_t c = cfg ? *cfg : AUDIO_DEFAULT_CONFIG();
    if (c.sample_rate == 0) c.sample_rate = 48000;
    s_sr = c.sample_rate;
    s_frame = c.frame_samples ? c.frame_samples : (s_sr / 100);

    for (int i = 0; i < AUDIO_NUM_LINES; i++) {
        memset(&s_line[i], 0, sizeof(s_line[i]));
        s_line[i].src = SRC_SILENCE;
        s_line[i].bridge_from = -1;
        s_line[i].ext = xStreamBufferCreate(s_frame * 8 * sizeof(int16_t), 1);
        s_line[i].rx  = xStreamBufferCreate(s_frame * 8 * sizeof(int16_t), 1);
        /* 8 kHz, so an eighth the byte count of `rx` for the same time span. */
        s_line[i].aec_out = xStreamBufferCreate(AUDIO_AEC_BLOCK * 8 * sizeof(int16_t), 1);
        s_rx_mono[i]  = calloc(s_frame, sizeof(int16_t));
        s_fx_tmp[i]   = calloc(s_frame, sizeof(int16_t));
        if (!s_line[i].ext || !s_line[i].rx || !s_line[i].aec_out || !s_rx_mono[i]) {
            ESP_LOGE(TAG, "line %d buffer alloc failed", i);
            return ESP_ERR_NO_MEM;
        }
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    /* Default (dma_desc_num=6, dma_frame_num=240) is 1440 frames = 30ms of
     * DMA ring per direction at 48kHz -- up to ~60ms round-trip just from
     * this, which is most of the 12-52ms local echo-path delay the AEC has
     * to cover (the ES8388 codec itself adds negligible delay; this ring is
     * the real "buffering" for a digital I2S link). Halved here: 4x180 =
     * 720 frames = 15ms/direction. Still >= 2 descriptors' worth of slack
     * ahead of audio_task's own 10ms (s_frame) processing block, so a
     * single late wakeup doesn't starve the ring -- but less slack than
     * before, so a *sustained* stall (WiFi/SD/etc. blocking audio_task for
     * multiple frames) is more likely to produce an audible glitch than it
     * was. Tune via aec sweep; revert to the default pair above if this
     * causes audible clicks in normal use. */
    chan_cfg.dma_desc_num = 4;
    chan_cfg.dma_frame_num = 180;
    esp_err_t err = i2s_new_channel(&chan_cfg, &s_tx, &s_rx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel: %s", esp_err_to_name(err));
        return err;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(s_sr),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BOARD_I2S_MCLK,
            .bclk = BOARD_I2S_BCLK,
            .ws   = BOARD_I2S_LRCK,
            .dout = BOARD_I2S_DOUT,
            .din  = BOARD_I2S_DIN,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    /* MCLK = 256 * fs (= 12.288 MHz at 48 kHz). */
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;

    if ((err = i2s_channel_init_std_mode(s_tx, &std_cfg)) != ESP_OK ||
        (err = i2s_channel_init_std_mode(s_rx, &std_cfg)) != ESP_OK) {
        ESP_LOGE(TAG, "i2s init std: %s", esp_err_to_name(err));
        return err;
    }
    i2s_channel_enable(s_tx);
    i2s_channel_enable(s_rx);

    BaseType_t ok = xTaskCreate(audio_task, "audio_io", 4096, NULL,
                                configMAX_PRIORITIES - 3, &s_task);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "audio task create failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "audio up: %u Hz, frame=%u samples (%.1f ms), MCLK=256fs",
             (unsigned)s_sr, (unsigned)s_frame, 1000.0f * s_frame / s_sr);
    return ESP_OK;
}

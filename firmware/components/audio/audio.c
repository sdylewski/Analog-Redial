#include "audio.h"
#include "board_pins.h"
#include "dtmf.h"
#include "aec.h"
#include "esp_log.h"
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
} line_t;

static line_t s_line[AUDIO_NUM_LINES];
static int16_t *s_rx_mono[AUDIO_NUM_LINES]; /* last RX frame, for bridge */

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

uint32_t audio_sample_rate(void) { return s_sr; }
size_t   audio_frame_samples(void) { return s_frame; }

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

void audio_tx_external(int line, bool enable)
{
    if (line < 0 || line >= AUDIO_NUM_LINES) return;
    portENTER_CRITICAL(&s_mux);
    s_line[line].src = enable ? SRC_EXTERNAL : SRC_SILENCE;
    portEXIT_CRITICAL(&s_mux);
}

size_t audio_ext_write(int line, const int16_t *mono, size_t n)
{
    if (line < 0 || line >= AUDIO_NUM_LINES || !s_line[line].ext) return 0;
    return xStreamBufferSend(s_line[line].ext, mono, n * sizeof(int16_t), 0)
           / sizeof(int16_t);
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

size_t audio_rx_read(int line, int16_t *mono, size_t n, uint32_t timeout_ms)
{
    if (line < 0 || line >= AUDIO_NUM_LINES || !s_line[line].rx) return 0;
    return xStreamBufferReceive(s_line[line].rx, mono, n * sizeof(int16_t),
                                pdMS_TO_TICKS(timeout_ms)) / sizeof(int16_t);
}

size_t audio_aec_read(int line, int16_t *mono, size_t n, uint32_t timeout_ms)
{
    if (line < 0 || line >= AUDIO_NUM_LINES || !s_line[line].aec_out) return 0;
    return xStreamBufferReceive(s_line[line].aec_out, mono, n * sizeof(int16_t),
                                pdMS_TO_TICKS(timeout_ms)) / sizeof(int16_t);
}

uint16_t audio_rx_level(int line)
{
    if (line < 0 || line >= AUDIO_NUM_LINES) return 0;
    return s_line[line].rx_level;
}

void audio_set_test_loopback(bool on)
{
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
            for (size_t i = got; i < n; i++) out[i] = 0; /* underrun -> silence */
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

        /* RX processing: real ADC input, or (test loopback) the TX we just
         * built, so a played tone/DTMF/clip is level-metered, DTMF-detected
         * and readable back without a handset. */
        bool loop = s_test_loopback;
        size_t rxn = loop ? n : got;
        for (int line = 0; line < AUDIO_NUM_LINES; line++) {
            const int16_t *src = loop ? (line == 0 ? txL : txR) : s_rx_mono[line];
            s_line[line].rx_level = rms_of(src, rxn);
            if (rxn && s_line[line].rx_tap) {
                s_line[line].rx_tap(line, src, rxn, s_line[line].rx_ctx);
            }
            if (rxn && s_line[line].diag_tap) {
                s_line[line].diag_tap(line, src, rxn, s_line[line].diag_ctx);
            }
            if (rxn && s_line[line].rx) {
                xStreamBufferSend(s_line[line].rx, src, rxn * sizeof(int16_t), 0);
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
                    aec_process(line, ref8, near8, near8, m); /* in-place */
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
    /* MCLK = 256 * fs (= 12.288 MHz at 48 kHz), which the WM8731 R8 table expects. */
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

#include "bt_audio_io.h"
#include "driver/dac_continuous.h"
#include "esp_adc/adc_continuous.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "esp_log.h"
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "bt_audio_io";

#define SAMPLE_RATE_HZ   8000   /* CVSD's native rate -- matches HFP's own PCM */
#define ADC_CHANNEL      ADC_CHANNEL_6   /* GPIO34 on the original ESP32 */
#define UPLINK_RB_SIZE   4096   /* ~256ms of 16-bit mono @ 8kHz -- plenty of slack
                                  * against HFP's own small per-callback chunk size */

static dac_continuous_handle_t s_dac;
static adc_continuous_handle_t s_adc;
static RingbufHandle_t         s_uplink_rb;
static TaskHandle_t             s_adc_task;

/* ---- downlink: HFP samples -> DAC (GPIO25) ---- */

/* Diagnostic finding, 2026-09-14: a synthetic tone written through this
 * function -- bypassing Bluetooth entirely -- came out with the same
 * distortion as live call audio, and consistently only the first ~2-2.5s
 * of any capture was ever clean/present before the signal collapsed to
 * near-silence. This function used to malloc()/free() a fresh conversion
 * buffer on every single call -- for real HFP audio, Bluedroid's own
 * incoming-data callback can fire well over 100x/sec, so that's a lot of
 * small, repeated heap churn on a chip with a much smaller heap than the
 * C6. A static scratch buffer removes that as a variable while the actual
 * ~2.5s-cliff behavior is investigated further -- not confirmed as the
 * fix, but a real improvement either way (this callback runs in Bluedroid's
 * own task context, where blocking on a slow/fragmented allocator is
 * exactly the kind of thing that can start failing intermittently once the
 * heap's been running a while). */
#define DOWNLINK_SCRATCH_SAMPLES 512
static uint8_t s_downlink_scratch[DOWNLINK_SCRATCH_SAMPLES];

void bt_audio_io_write_downlink(const uint8_t *pcm16, size_t len)
{
    if (!s_dac || len < 2) return;
    /* 16-bit signed -> 8-bit unsigned: keep the high byte (same conversion
     * used by every 16-to-8-bit PCM downsample), offset by 0x80 since the
     * DAC's 8-bit range is unsigned (0-255, 128 = "silence"/mid-rail) while
     * PCM16 is signed (0 = silence). */
    size_t total = len / 2;
    const int16_t *in = (const int16_t *)pcm16;
    for (size_t done = 0; done < total; ) {
        size_t n = total - done;
        if (n > DOWNLINK_SCRATCH_SAMPLES) n = DOWNLINK_SCRATCH_SAMPLES;
        for (size_t i = 0; i < n; i++) {
            s_downlink_scratch[i] = (uint8_t)(((int32_t)in[done + i] >> 8) + 128);
        }
        size_t written = 0;
        dac_continuous_write(s_dac, s_downlink_scratch, n, &written, pdMS_TO_TICKS(50));
        done += n;
    }
}

/* ---- uplink: ADC (GPIO34) -> HFP samples ---- */

uint32_t bt_audio_io_read_uplink(uint8_t *out, uint32_t max_len)
{
    if (!s_uplink_rb) return 0;
    size_t item_size = 0;
    uint8_t *data = xRingbufferReceiveUpTo(s_uplink_rb, &item_size, 0, max_len);
    if (!data) return 0;
    memcpy(out, data, item_size);
    vRingbufferReturnItem(s_uplink_rb, data);
    return (uint32_t)item_size;
}

/* Continuously samples GPIO34 in the background and pushes converted
 * 16-bit PCM into s_uplink_rb, so bt_audio_io_read_uplink() (called from
 * HFP's own outgoing-data callback, which must not block) always has
 * recent audio ready rather than sampling the ADC synchronously on demand. */
static void adc_sample_task(void *arg)
{
    /* One ADC conversion frame per SAMPLE_RATE_HZ/50 -- 160 samples
     * (~20ms) matches typical SCO packet cadence without over-buffering
     * latency. adc_digi_output_data_t is 2 bytes/sample (TYPE1, original
     * ESP32). */
    enum { FRAME_SAMPLES = 160 };
    uint8_t raw[FRAME_SAMPLES * SOC_ADC_DIGI_RESULT_BYTES];
    int16_t pcm[FRAME_SAMPLES];

    while (1) {
        uint32_t got = 0;
        esp_err_t err = adc_continuous_read(s_adc, raw, sizeof(raw), &got, pdMS_TO_TICKS(1000));
        if (err != ESP_OK) continue;

        size_t n = 0;
        for (uint32_t i = 0; i + SOC_ADC_DIGI_RESULT_BYTES <= got && n < FRAME_SAMPLES; i += SOC_ADC_DIGI_RESULT_BYTES) {
            adc_digi_output_data_t *p = (adc_digi_output_data_t *)&raw[i];
            /* 12-bit unsigned (0-4095, ~1.65V bias sits near mid-scale per
             * the bias network in archive/BLUETOOTH_AUDIO_UPGRADE.md) ->
             * 16-bit signed PCM: center on mid-scale, then scale up. */
            int32_t centered = (int32_t)p->type1.data - 2048;
            pcm[n++] = (int16_t)(centered * 16); /* 12-bit -> 16-bit headroom */
        }
        if (n > 0 && s_uplink_rb) {
            xRingbufferSend(s_uplink_rb, pcm, n * sizeof(int16_t), pdMS_TO_TICKS(20));
        }
    }
}

/* ---- diagnostic: synthetic tone straight into the downlink path ---- */

typedef struct { int hz; int ms; } test_tone_args_t;

static void test_tone_task(void *arg)
{
    test_tone_args_t a = *(test_tone_args_t *)arg;
    free(arg);

    enum { CHUNK_SAMPLES = 160 }; /* ~20ms/chunk, matches adc_sample_task's own cadence */
    int16_t chunk[CHUNK_SAMPLES];
    int total_samples = SAMPLE_RATE_HZ * a.ms / 1000;
    int phase_i = 0;

    for (int done = 0; done < total_samples; done += CHUNK_SAMPLES) {
        int n = total_samples - done;
        if (n > CHUNK_SAMPLES) n = CHUNK_SAMPLES;
        for (int i = 0; i < n; i++) {
            float t = (float)(phase_i + i) / SAMPLE_RATE_HZ;
            chunk[i] = (int16_t)(28000.0f * sinf(2.0f * (float)M_PI * a.hz * t));
        }
        phase_i += n;
        bt_audio_io_write_downlink((const uint8_t *)chunk, (size_t)n * sizeof(int16_t));
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    vTaskDelete(NULL);
}

bool bt_audio_io_test_tone(int hz, int ms)
{
    if (!s_dac) { ESP_LOGE(TAG, "test_tone: DAC not initialized"); return false; }
    if (hz <= 0 || ms <= 0) { ESP_LOGE(TAG, "test_tone: bad hz/ms"); return false; }
    test_tone_args_t *a = malloc(sizeof(*a));
    if (!a) { ESP_LOGE(TAG, "test_tone: malloc failed"); return false; }
    a->hz = hz;
    a->ms = ms;
    /* High priority, matching adc_sample_task -- diagnostic to check
     * whether the ~40Hz artifact seen 2026-09-14 (identical whether the
     * source is live Bluetooth audio or this synthetic tone) is caused by
     * this task getting starved/preempted by the BT stack's own background
     * housekeeping while running at a low priority. */
    if (xTaskCreate(test_tone_task, "bt_test_tone", 3072, a, configMAX_PRIORITIES - 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "test_tone: xTaskCreate failed");
        free(a);
        return false;
    }
    return true;
}

/* ---- diagnostic: static DC level, for a multimeter/scope sanity check ---- */

typedef struct { bool high; int ms; } test_level_args_t;

static void test_level_task(void *arg)
{
    test_level_args_t a = *(test_level_args_t *)arg;
    free(arg);

    /* +/-32000 rather than full-scale +/-32768 -- same conversion
     * bt_audio_io_write_downlink() already does, just a constant instead of
     * a sine, so this exercises the exact same code path as the real tone
     * test. Maps to ~0.05V (low) / ~3.25V (high) at GPIO25, not quite rail-
     * to-rail. */
    int16_t level = a.high ? 32000 : -32000;
    enum { CHUNK_SAMPLES = 160 };
    int16_t chunk[CHUNK_SAMPLES];
    for (int i = 0; i < CHUNK_SAMPLES; i++) chunk[i] = level;

    int total_samples = SAMPLE_RATE_HZ * a.ms / 1000;
    for (int done = 0; done < total_samples; done += CHUNK_SAMPLES) {
        bt_audio_io_write_downlink((const uint8_t *)chunk, sizeof(chunk));
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    vTaskDelete(NULL);
}

void bt_audio_io_test_level(bool high, int ms)
{
    if (!s_dac || ms <= 0) return;
    test_level_args_t *a = malloc(sizeof(*a));
    if (!a) return;
    a->high = high;
    a->ms = ms;
    if (xTaskCreate(test_level_task, "bt_test_level", 3072, a, configMAX_PRIORITIES - 4, NULL) != pdPASS) {
        free(a);
    }
}

esp_err_t bt_audio_io_init(void)
{
    /* Idempotent -- called unconditionally at boot now (main.c) so
     * bt_audio_io_test_tone() works without ever needing a real HFP audio
     * connection first, AND still called from bt_app_hf_client_audio_open()
     * on every real connection (harmless no-op the second+ time). */
    if (s_dac) return ESP_OK;

    /* DAC: GPIO25 only (DAC_CHAN_0), mono -- HFP audio is mono, no reason
     * to also drive GPIO26 with a duplicate channel.
     *
     * clk_src MUST be APLL, not DEFAULT (PLL_D2) -- confirmed 2026-09-15 via
     * a clean boot-log capture that this was failing silently at every boot
     * this entire session: "Frequency is too small, the mclk division
     * exceed the maximum value 255". PLL_D2 is too fast a base clock to
     * divide down to an 8kHz DAC rate within the driver's 8-bit divider
     * limit; ESP-IDF's own dac_continuous/dac_audio example uses APLL for
     * exactly this reason ("to get a wider frequency range"). Since
     * dac_continuous_new_channels() was returning ESP_ERR_INVALID_ARG,
     * s_dac was never set, and every downlink test this session (tone,
     * static level, real HFP audio) was writing to a DAC handle that
     * silently didn't exist -- none of the "distortion" findings before
     * this fix should be trusted as characterizing the real DAC output. */
    dac_continuous_config_t dac_cfg = {
        .chan_mask = DAC_CHANNEL_MASK_CH0,
        .desc_num = 8,
        .buf_size = 512,
        .freq_hz = SAMPLE_RATE_HZ,
        .offset = 0,
        .clk_src = DAC_DIGI_CLK_SRC_APLL,
        .chan_mode = DAC_CHANNEL_MODE_SIMUL,
    };
    esp_err_t err = dac_continuous_new_channels(&dac_cfg, &s_dac);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "dac_continuous_new_channels failed: %s", esp_err_to_name(err));
        return err;
    }
    err = dac_continuous_enable(s_dac);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "dac_continuous_enable failed: %s", esp_err_to_name(err));
        return err;
    }

    /* ADC: GPIO34 (ADC1_CHANNEL_6). */
    adc_continuous_handle_cfg_t adc_handle_cfg = {
        .max_store_buf_size = 2048,
        .conv_frame_size = 160 * SOC_ADC_DIGI_RESULT_BYTES,
    };
    err = adc_continuous_new_handle(&adc_handle_cfg, &s_adc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adc_continuous_new_handle failed: %s", esp_err_to_name(err));
        return err;
    }
    adc_digi_pattern_config_t pattern = {
        .atten = ADC_ATTEN_DB_12, /* full 0-3.3V range -- matches the bias network's ~1.65V center */
        .channel = ADC_CHANNEL & 0x7,
        .unit = ADC_UNIT_1,
        .bit_width = SOC_ADC_DIGI_MAX_BITWIDTH,
    };
    adc_continuous_config_t adc_cfg = {
        .sample_freq_hz = SAMPLE_RATE_HZ,
        .conv_mode = ADC_CONV_SINGLE_UNIT_1,
        .format = ADC_DIGI_OUTPUT_FORMAT_TYPE1,
        .pattern_num = 1,
        .adc_pattern = &pattern,
    };
    err = adc_continuous_config(s_adc, &adc_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adc_continuous_config failed: %s", esp_err_to_name(err));
        return err;
    }
    err = adc_continuous_start(s_adc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adc_continuous_start failed: %s", esp_err_to_name(err));
        return err;
    }

    s_uplink_rb = xRingbufferCreate(UPLINK_RB_SIZE, RINGBUF_TYPE_BYTEBUF);
    if (!s_uplink_rb) {
        ESP_LOGE(TAG, "xRingbufferCreate failed");
        return ESP_ERR_NO_MEM;
    }

    xTaskCreate(adc_sample_task, "adc_sample", 3072, NULL, configMAX_PRIORITIES - 4, &s_adc_task);
    ESP_LOGI(TAG, "audio I/O up: DAC GPIO25 + ADC GPIO34, %d Hz mono", SAMPLE_RATE_HZ);
    return ESP_OK;
}

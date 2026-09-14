#include "bt_audio_io.h"
#include "driver/dac_continuous.h"
#include "esp_adc/adc_continuous.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "esp_log.h"
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

void bt_audio_io_write_downlink(const uint8_t *pcm16, size_t len)
{
    if (!s_dac || len < 2) return;
    /* 16-bit signed -> 8-bit unsigned: keep the high byte (same conversion
     * used by every 16-to-8-bit PCM downsample), offset by 0x80 since the
     * DAC's 8-bit range is unsigned (0-255, 128 = "silence"/mid-rail) while
     * PCM16 is signed (0 = silence). */
    size_t n = len / 2;
    uint8_t *out = malloc(n);
    if (!out) return;
    const int16_t *in = (const int16_t *)pcm16;
    for (size_t i = 0; i < n; i++) {
        out[i] = (uint8_t)(((int32_t)in[i] >> 8) + 128);
    }
    size_t written = 0;
    dac_continuous_write(s_dac, out, n, &written, pdMS_TO_TICKS(50));
    free(out);
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

esp_err_t bt_audio_io_init(void)
{
    /* DAC: GPIO25 only (DAC_CHAN_0), mono -- HFP audio is mono, no reason
     * to also drive GPIO26 with a duplicate channel. */
    dac_continuous_config_t dac_cfg = {
        .chan_mask = DAC_CHANNEL_MASK_CH0,
        .desc_num = 8,
        .buf_size = 512,
        .freq_hz = SAMPLE_RATE_HZ,
        .offset = 0,
        .clk_src = DAC_DIGI_CLK_SRC_DEFAULT,
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

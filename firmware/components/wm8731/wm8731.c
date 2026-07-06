#include "wm8731.h"
#include "board.h"
#include "board_pins.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "wm8731";

static i2c_master_dev_handle_t s_dev;
static uint8_t  s_addr = BOARD_WM8731_ADDR;
static uint16_t s_shadow[16];

/* WM8731 control word: 7-bit register address + 9-bit data, sent MSB-first as
 * two bytes:  [reg<<1 | data8] [data7..0]. */
static esp_err_t raw_write(uint8_t reg, uint16_t value)
{
    if (!s_dev) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t buf[2];
    buf[0] = (uint8_t)((reg << 1) | ((value >> 8) & 0x01));
    buf[1] = (uint8_t)(value & 0xFF);
    esp_err_t err = i2c_master_transmit(s_dev, buf, sizeof(buf), 100 /*ms*/);
    if (err == ESP_OK && reg < 16) {
        s_shadow[reg] = value & 0x1FF;
    }
    return err;
}

esp_err_t wm8731_write_reg(wm8731_reg_t reg, uint16_t value)
{
    return raw_write((uint8_t)reg, value);
}

uint16_t wm8731_shadow(wm8731_reg_t reg)
{
    return (reg < 16) ? s_shadow[reg] : 0;
}

static esp_err_t ensure_dev(uint8_t addr)
{
    ESP_ERROR_CHECK(board_i2c_init());
    if (s_dev && s_addr == addr) {
        return ESP_OK;
    }
    if (s_dev) {
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
    }
    i2c_device_config_t dcfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = 100000,
    };
    esp_err_t err = i2c_master_bus_add_device(board_i2c_bus(), &dcfg, &s_dev);
    if (err == ESP_OK) {
        s_addr = addr;
    }
    return err;
}

esp_err_t wm8731_probe(uint8_t i2c_addr)
{
    ESP_ERROR_CHECK(board_i2c_init());
    return i2c_master_probe(board_i2c_bus(), i2c_addr, 100 /*ms*/);
}

esp_err_t wm8731_reset(void)
{
    esp_err_t err = raw_write(WM8731_REG_RESET, 0x000);
    vTaskDelay(pdMS_TO_TICKS(10));
    return err;
}

/* Sampling control (R8) for a fixed 12.288 MHz MCLK (256fs) — datasheet
 * "Normal Mode Sample Rate Look-up Table".
 *   48 kHz / 48 kHz : BOSR=0 SR=0000 -> 0x00
 *    8 kHz /  8 kHz : BOSR=0 SR=0011 -> 0x0C
 * We always feed MCLK = 12.288 MHz from the I2S master (mclk_multiple varies). */
static uint16_t sampling_reg_for(uint32_t fs)
{
    switch (fs) {
        case 8000:  return 0x0C; /* (SR=0011)<<2 */
        case 48000: return 0x00;
        default:    return 0x00;
    }
}

esp_err_t wm8731_set_active(bool active)
{
    return raw_write(WM8731_REG_ACTIVE, active ? 0x001 : 0x000);
}

esp_err_t wm8731_set_dac_mute(bool mute)
{
    /* R5 digital path: bit3 DACMU */
    uint16_t v = s_shadow[WM8731_REG_DIGITAL_PATH];
    if (mute) v |= (1 << 3); else v &= ~(1 << 3);
    return raw_write(WM8731_REG_DIGITAL_PATH, v);
}

esp_err_t wm8731_set_line_in_volume(uint8_t left_step, uint8_t right_step)
{
    /* R0/R1 bits[4:0] LINVOL, bit8 LRINBOTH/load. Clear mute (bit7 LINMUTE). */
    left_step  &= 0x1F;
    right_step &= 0x1F;
    esp_err_t e1 = raw_write(WM8731_REG_LLINE_IN, left_step);
    esp_err_t e2 = raw_write(WM8731_REG_RLINE_IN, right_step);
    return (e1 != ESP_OK) ? e1 : e2;
}

esp_err_t wm8731_set_hp_volume(uint8_t left_step, uint8_t right_step)
{
    /* R2/R3 bits[6:0] HPVOL (0x79 = 0 dB, 0x30..0x7F valid, <0x30 mute).
     * bit8 = simultaneous load + zero-cross. */
    esp_err_t e1 = raw_write(WM8731_REG_LHP_OUT, (left_step & 0x7F) | (1 << 8));
    esp_err_t e2 = raw_write(WM8731_REG_RHP_OUT, (right_step & 0x7F) | (1 << 8));
    return (e1 != ESP_OK) ? e1 : e2;
}

esp_err_t wm8731_set_bypass(bool enable)
{
    /* R4 analog path: bit3 BYPASS */
    uint16_t v = s_shadow[WM8731_REG_ANALOG_PATH];
    if (enable) v |= (1 << 3); else v &= ~(1 << 3);
    return raw_write(WM8731_REG_ANALOG_PATH, v);
}

esp_err_t wm8731_set_dac_select(bool enable)
{
    /* R4 analog path: bit4 DACSEL */
    uint16_t v = s_shadow[WM8731_REG_ANALOG_PATH];
    if (enable) v |= (1 << 4); else v &= ~(1 << 4);
    return raw_write(WM8731_REG_ANALOG_PATH, v);
}

esp_err_t wm8731_init(const wm8731_config_t *cfg)
{
    wm8731_config_t c = cfg ? *cfg : WM8731_DEFAULT_CONFIG();
    if (c.i2c_addr == 0) c.i2c_addr = BOARD_WM8731_ADDR;
    if (c.sample_rate_hz == 0) c.sample_rate_hz = 48000;

    esp_err_t err = ensure_dev(c.i2c_addr);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "add device 0x%02X failed: %s", c.i2c_addr, esp_err_to_name(err));
        return err;
    }

    /* Reset. */
    if ((err = wm8731_reset()) != ESP_OK) return err;

    /* Power down everything except we keep OSC/CLKOUT down; power up later.
     * R6 bits: 1=LINEINPD 2=ADCPD 3=DACPD 4=OUTPD 5=OSCPD 6=CLKOUTPD 7=POWEROFF
     * Power up line-in, ADC, DAC, outputs. Keep OSC powered down (external MCLK),
     * CLKOUT powered down. */
    raw_write(WM8731_REG_POWER_DOWN, (1 << 5) | (1 << 6)); /* OSCPD|CLKOUTPD */

    /* Line input volume ~0 dB (0x17), unmuted. */
    wm8731_set_line_in_volume(0x17, 0x17);

    /* Headphone out volume modest (~ -10 dB) for debug jack; 0x79 = 0 dB. */
    wm8731_set_hp_volume(0x6F, 0x6F);

    /* R4 analog path: DACSEL=1 (DAC->out), BYPASS=0, INSEL=0 (line in to ADC),
     * MUTEMIC=1, sidetone off. bit0 MICBOOST=0, bit1 MUTEMIC=1, bit2 INSEL=0,
     * bit3 BYPASS=0, bit4 DACSEL=1. */
    raw_write(WM8731_REG_ANALOG_PATH, (1 << 1) | (1 << 4));

    /* R5 digital path: de-emphasis off, ADC high-pass on (bit0 ADCHPD=0),
     * DACMU=0. Value 0x00. */
    raw_write(WM8731_REG_DIGITAL_PATH, 0x000);

    /* R7 digital interface: I2S format (bits1:0=10), 16-bit (bits3:2=00),
     * slave (bit6 MS=0), normal LRP. Value = 0x02. */
    raw_write(WM8731_REG_DIGITAL_IF, 0x002);

    /* R8 sampling control for chosen fs at 12.288 MHz MCLK. */
    raw_write(WM8731_REG_SAMPLING, sampling_reg_for(c.sample_rate_hz));

    /* Power up the analog/digital blocks we use (clear PD bits except OSC/CLKOUT). */
    raw_write(WM8731_REG_POWER_DOWN, (1 << 5) | (1 << 6));

    /* Activate digital core. */
    wm8731_set_active(true);

    ESP_LOGI(TAG, "init ok: addr=0x%02X fs=%uHz R8=0x%03X",
             c.i2c_addr, (unsigned)c.sample_rate_hz, sampling_reg_for(c.sample_rate_hz));
    return ESP_OK;
}

/*
 * wm8731.h — Wolfson WM8731 codec driver (I2C control).
 *
 * The codec is an I2S *slave*; the ESP32 generates BCLK/LRCK/MCLK. This driver
 * only handles the I2C control-register side (init, routing, volume, power).
 * I2S data streaming lives in the `audio` component.
 *
 * Board mapping: codec LEFT channel = Line 0, RIGHT channel = Line 1.
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Register indices (datasheet Table). Values are 9-bit. */
typedef enum {
    WM8731_REG_LLINE_IN   = 0x00,
    WM8731_REG_RLINE_IN   = 0x01,
    WM8731_REG_LHP_OUT    = 0x02,
    WM8731_REG_RHP_OUT    = 0x03,
    WM8731_REG_ANALOG_PATH = 0x04,
    WM8731_REG_DIGITAL_PATH = 0x05,
    WM8731_REG_POWER_DOWN = 0x06,
    WM8731_REG_DIGITAL_IF = 0x07,
    WM8731_REG_SAMPLING   = 0x08,
    WM8731_REG_ACTIVE     = 0x09,
    WM8731_REG_RESET      = 0x0F,
} wm8731_reg_t;

typedef struct {
    uint8_t  i2c_addr;       /* default BOARD_WM8731_ADDR (0x1A) */
    uint32_t sample_rate_hz; /* 8000 or 48000 supported here */
} wm8731_config_t;

#define WM8731_DEFAULT_CONFIG() (wm8731_config_t){ .i2c_addr = 0x1A, .sample_rate_hz = 48000 }

/* Initialize the codec for telephony: line-in -> ADC, DAC -> line-out,
 * stereo 16-bit I2S (Philips), given sample rate. Uses the shared board I2C bus. */
esp_err_t wm8731_init(const wm8731_config_t *cfg);

/* Probe: returns ESP_OK if the device ACKs on the I2C bus. */
esp_err_t wm8731_probe(uint8_t i2c_addr);

/* Write one 9-bit register (low-level). */
esp_err_t wm8731_write_reg(wm8731_reg_t reg, uint16_t value);

/* Mirror of the last value written to each register (the WM8731 is
 * write-only over I2C, so we shadow it). reg must be 0..15. */
uint16_t wm8731_shadow(wm8731_reg_t reg);

/* Line-out (DAC) and line-in (ADC) volume helpers.
 * out_db: headphone/line out volume control; in 1 dB steps clamped to range.
 * line_in: 0..31 codec steps (0x17 = 0 dB). */
esp_err_t wm8731_set_dac_mute(bool mute);
esp_err_t wm8731_set_line_in_volume(uint8_t left_step, uint8_t right_step);
esp_err_t wm8731_set_hp_volume(uint8_t left_step, uint8_t right_step);

/* Enable/disable analog bypass (line-in routed straight to outputs).
 * Useful for bring-up (hear the SLIC without the I2S/DSP path). */
esp_err_t wm8731_set_bypass(bool enable);

/* Select DAC output path (normal playback) on/off. */
esp_err_t wm8731_set_dac_select(bool enable);

/* Activate / deactivate the digital core (R9). */
esp_err_t wm8731_set_active(bool active);

/* Full software reset. */
esp_err_t wm8731_reset(void);

#ifdef __cplusplus
}
#endif

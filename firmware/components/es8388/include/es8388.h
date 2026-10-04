/*
 * es8388.h — Everest ES8388 codec driver (I2C control).
 *
 * The codec is an I2S *slave*; the ESP32 generates BCLK/LRCK/MCLK. This driver
 * only handles the I2C control-register side (init, routing, volume, power).
 * I2S data streaming lives in the `audio` component.
 *
 * Board mapping (Analog Redial v2, U4): DAC LOUT1 = Line 0 tx, ROUT1 = Line 1
 * tx — both play back simultaneously. LOUT2/ROUT2 are not a separate signal:
 * per the ES8388 datasheet, the left mixer output always drives both LOUT1
 * and LOUT2, and the right mixer drives both ROUT1 and ROUT2. On this board
 * LOUT2/ROUT2 come out on debug header J5 as a bench-test monitor tap of the
 * same Line0/Line1 tx audio already going to the SLICs, not an independent
 * third/fourth channel.
 *
 * ADC input: LIN1 = Line 0 rx (unchanged from the original layout), RIN1 =
 * Line 1 rx. Originally LIN2 carried Line 1's mic (muxed against LIN1 on the
 * same left ADC, so only one line's rx was ever capturable) while RIN1/RIN2
 * sat dead, capacitively bypassed to GND with no real source. Bodge-fixed by
 * pulling the RIN1 bypass cap and wiring Line 1's AG1171 Vout into that pad
 * (100nF coupling cap, same as the original LIN1/LIN2 paths) instead of
 * sharing LIN2. LIN1 and RIN1 are independent ADC channels (left/right PGA
 * -> ADC, see ES8388 block diagram) that convert simultaneously every I2S
 * frame -- true stereo capture, no runtime muxing needed. LIN2/RIN2 are
 * unused now (LIN2 still has the original Line 1 tap electrically present
 * but nothing ever selects it via LINSEL again).
 *
 * PGA gain is +3dB on both channels (down from the original left channel's
 * ~+24dB). With both lines' mic+speaker live simultaneously for the first
 * time ever, high gain on both channels closed a real positive-feedback
 * loop (acoustic handset coupling / AG1171 sidetone) -- confirmed by the
 * noise ramping up over ~0.5s to full volume rather than being present at
 * fixed amplitude, the signature of loop gain > 1. +3dB/+3dB is clean and
 * clear on a real two-line call. Tuned empirically on real hardware via the
 * bring-up console's `micamp` command, 2026-07-20.
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Register indices (ES8388 datasheet). Values are 8-bit. */
typedef enum {
    ES8388_REG_CONTROL1    = 0x00,
    ES8388_REG_CONTROL2    = 0x01,
    ES8388_REG_CHIPPOWER   = 0x02,
    ES8388_REG_ADCPOWER    = 0x03,
    ES8388_REG_DACPOWER    = 0x04,
    ES8388_REG_CHIPLOPOW1  = 0x05,
    ES8388_REG_CHIPLOPOW2  = 0x06,
    ES8388_REG_ANAVOLMANAG = 0x07,
    ES8388_REG_MASTERMODE  = 0x08,
    ES8388_REG_ADCCONTROL1 = 0x09,
    ES8388_REG_ADCCONTROL2 = 0x0A,
    ES8388_REG_ADCCONTROL3 = 0x0B,
    ES8388_REG_ADCCONTROL4 = 0x0C,
    ES8388_REG_ADCCONTROL5 = 0x0D,
    ES8388_REG_LADCVOL     = 0x10,
    ES8388_REG_RADCVOL     = 0x11,
    ES8388_REG_DACCONTROL1 = 0x17,
    ES8388_REG_DACCONTROL2 = 0x18,
    ES8388_REG_DACCONTROL3 = 0x19,
    ES8388_REG_LDACVOL     = 0x1A,
    ES8388_REG_RDACVOL     = 0x1B,
    ES8388_REG_DACCONTROL16 = 0x26,
    ES8388_REG_DACCONTROL17 = 0x27,
    ES8388_REG_DACCONTROL20 = 0x2A,
    ES8388_REG_DACCONTROL21 = 0x2B,
    ES8388_REG_DACCONTROL23 = 0x2D,
    ES8388_REG_LOUT1VOL     = 0x2E,
    ES8388_REG_ROUT1VOL     = 0x2F,
    ES8388_REG_LOUT2VOL     = 0x30,
    ES8388_REG_ROUT2VOL     = 0x31,
} es8388_reg_t;

typedef struct {
    uint8_t  i2c_addr;       /* default BOARD_CODEC_I2C_ADDR (0x10, CE tied low) */
    uint32_t sample_rate_hz; /* 8000 or 48000 supported here */
} es8388_config_t;

#define ES8388_DEFAULT_CONFIG() (es8388_config_t){ .i2c_addr = 0x10, .sample_rate_hz = 48000 }

/* Initialize the codec for telephony: LIN1 -> left ADC (Line 0 rx), RIN1 ->
 * right ADC (Line 1 rx), DAC -> LOUT1/ROUT1, stereo 16-bit I2S (Philips,
 * slave), given sample rate. Uses the shared board I2C bus. */
esp_err_t es8388_init(const es8388_config_t *cfg);

/* Probe: returns ESP_OK if the device ACKs on the I2C bus. */
esp_err_t es8388_probe(uint8_t i2c_addr);

/* Write one 8-bit register (low-level). */
esp_err_t es8388_write_reg(es8388_reg_t reg, uint8_t value);

/* Mirror of the last value written to each register (what the driver *thinks*
 * it wrote). reg must be 0..0x3F. */
uint8_t es8388_shadow(uint8_t reg);

/* Actually reads the register back over I2C (datasheet section 4, "I2C
 * Read": write reg addr, repeated start, read data) rather than trusting the
 * shadow copy -- use this to catch a write that didn't really take. */
esp_err_t es8388_read_reg(uint8_t reg, uint8_t *out_value);

esp_err_t es8388_set_dac_mute(bool mute);
esp_err_t es8388_set_adc_volume(uint8_t left_step, uint8_t right_step);
esp_err_t es8388_set_dac_volume(uint8_t left_step, uint8_t right_step);

/* Full software reset + power-down. */
esp_err_t es8388_reset(void);

#ifdef __cplusplus
}
#endif

#include "es8388.h"
#include "board.h"
#include "board_pins.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "es8388";

static i2c_master_dev_handle_t s_dev;
static uint8_t s_addr = BOARD_CODEC_I2C_ADDR;
static uint8_t s_shadow[0x40];

/* ES8388 control word: plain 8-bit register address + 8-bit data. */
static esp_err_t raw_write(uint8_t reg, uint8_t value)
{
    if (!s_dev) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t buf[2] = { reg, value };
    esp_err_t err = i2c_master_transmit(s_dev, buf, sizeof(buf), 100 /*ms*/);
    if (err == ESP_OK && reg < sizeof(s_shadow)) {
        s_shadow[reg] = value;
    }
    return err;
}

esp_err_t es8388_write_reg(es8388_reg_t reg, uint8_t value)
{
    return raw_write((uint8_t)reg, value);
}

uint8_t es8388_shadow(uint8_t reg)
{
    return (reg < sizeof(s_shadow)) ? s_shadow[reg] : 0;
}

esp_err_t es8388_read_reg(uint8_t reg, uint8_t *out_value)
{
    if (!s_dev || !out_value) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t addr = reg;
    return i2c_master_transmit_receive(s_dev, &addr, 1, out_value, 1, 100 /*ms*/);
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

esp_err_t es8388_probe(uint8_t i2c_addr)
{
    ESP_ERROR_CHECK(board_i2c_init());
    return i2c_master_probe(board_i2c_bus(), i2c_addr, 100 /*ms*/);
}

esp_err_t es8388_reset(void)
{
    /* CONTROL1: bits1:0 = RESET (active). Hold then release, then power
     * everything down until init() brings blocks up in the right order. */
    esp_err_t err = raw_write(ES8388_REG_CONTROL1, 0x03);
    vTaskDelay(pdMS_TO_TICKS(10));
    raw_write(ES8388_REG_CONTROL1, 0x00);
    raw_write(ES8388_REG_CHIPPOWER, 0xFF); /* power down all blocks */
    return err;
}

esp_err_t es8388_set_dac_mute(bool mute)
{
    /* DACCONTROL3 bit2 = DACMUTE. */
    uint8_t cur = s_shadow[ES8388_REG_DACCONTROL3];
    if (mute) cur |= (1 << 2); else cur &= ~(1 << 2);
    return raw_write(ES8388_REG_DACCONTROL3, cur);
}

esp_err_t es8388_set_adc_volume(uint8_t left_step, uint8_t right_step)
{
    /* LADCVOL/RADCVOL: 0x00 = 0 dB, each step = -0.5 dB, clamps at 0xC0. */
    esp_err_t e1 = raw_write(ES8388_REG_LADCVOL, left_step);
    esp_err_t e2 = raw_write(ES8388_REG_RADCVOL, right_step);
    return (e1 != ESP_OK) ? e1 : e2;
}

esp_err_t es8388_set_dac_volume(uint8_t left_step, uint8_t right_step)
{
    /* LOUT1VOL/ROUT1VOL (datasheet Reg. 46/47): 0x00 = -45 dB, 0x1E (30) =
     * 0 dB, 1.5 dB/step, only documented up through 0x21 (33) = +4.5 dB.
     * Confirmed live 2026-09-25 on real hardware (scope on the SLIC VIN
     * pin) that values above 33 are genuinely undefined, not just "more
     * gain": 35/36 gave plausibly-increasing output, but 37 and 40 cut the
     * AC signal to a dead 0 Vpp -- the chip's internal gain-control DAC
     * isn't wired for anything past the documented 34 codes, even though
     * the register field itself is 6 bits wide. Clamp here (not just at
     * the `spkamp` console command) since this is the one place every
     * caller -- console, board_speaker_volume persistence -- funnels
     * through. */
    if (left_step > 33) left_step = 33;
    if (right_step > 33) right_step = 33;
    esp_err_t e1 = raw_write(ES8388_REG_LOUT1VOL, left_step);
    esp_err_t e2 = raw_write(ES8388_REG_ROUT1VOL, right_step);
    return (e1 != ESP_OK) ? e1 : e2;
}

esp_err_t es8388_init(const es8388_config_t *cfg)
{
    es8388_config_t c = cfg ? *cfg : ES8388_DEFAULT_CONFIG();
    if (c.i2c_addr == 0) c.i2c_addr = BOARD_CODEC_I2C_ADDR;
    if (c.sample_rate_hz == 0) c.sample_rate_hz = 48000;

    esp_err_t err = ensure_dev(c.i2c_addr);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "add device 0x%02X failed: %s", c.i2c_addr, esp_err_to_name(err));
        return err;
    }

    if ((err = es8388_reset()) != ESP_OK) return err;

    /* Sequence below follows Espressif's proven es8388_init()/es8388_start()
     * order (esp-adf audio_hal/driver/es8388), adapted for: I2S slave (ESP32
     * is master), 16-bit Philips I2S, all four outputs enabled -- LOUT1/ROUT1
     * feed the two phone lines' SLICs, LOUT2/ROUT2 feed the J7 debug/monitor
     * header (same left/right mix duplicated onto a second driver pair per
     * the ES8388 datasheet section 9.1, not an independent signal) -- and ADC
     * capturing line 0 (LINPUT1) by default. */
    raw_write(ES8388_REG_DACCONTROL3, 0x04);           /* pre-power VROI setting */
    raw_write(ES8388_REG_CONTROL2, 0x50);              /* VMID/bias, normal ramp */
    raw_write(ES8388_REG_CHIPPOWER, 0x00);              /* power up all blocks */

    /* Vendor-recommended anti-pop/anti-click writes (undocumented bits,
     * carried over verbatim from the reference driver). */
    raw_write(0x35, 0xA0);
    raw_write(0x37, 0xD0);
    raw_write(0x39, 0xD0);

    raw_write(ES8388_REG_MASTERMODE, 0x00);            /* I2S slave */
    raw_write(ES8388_REG_DACPOWER, 0x3C);              /* DAC L/R + LOUT1/ROUT1/LOUT2/ROUT2 up */
    raw_write(ES8388_REG_CONTROL1, 0x12);              /* VMID/analog output enable */

    raw_write(ES8388_REG_DACCONTROL1, 0x18);           /* I2S, 16-bit, slave */
    raw_write(ES8388_REG_DACCONTROL2, 0x02);           /* 256fs MCLK/LRCK ratio */
    raw_write(ES8388_REG_DACCONTROL16, 0x00);          /* DAC (not ADC) feeds mixer */
    raw_write(ES8388_REG_DACCONTROL17, 0x90);          /* LDAC -> LMIX, 0 dB */
    raw_write(ES8388_REG_DACCONTROL20, 0x90);          /* RDAC -> RMIX, 0 dB */
    raw_write(ES8388_REG_DACCONTROL21, 0x80);          /* enable output mixers */
    raw_write(ES8388_REG_DACCONTROL23, 0x00);

    /* LDACVOL/RDACVOL (digital DAC gain, separate from LOUT1VOL/ROUT1VOL
     * below which is the analog output driver stage) reset to 0xC0 = -96 dB
     * = muted, and nothing else in this sequence ever touches them. Left at
     * the reset default this mutes the DAC before its output even reaches
     * the LOUT1/LOUT2 mixers -- confirmed via es8388_read_reg() readback, a
     * real silent-audio bug, not just a theoretical one. */
    raw_write(ES8388_REG_LDACVOL, 0x00);               /* 0 dB */
    raw_write(ES8388_REG_RDACVOL, 0x00);               /* 0 dB */

    /* LOUT1/ROUT1: per-line earpiece drive level (0/30/1.5dB-step
     * convention, see es8388_set_dac_volume()). 21 (-13.5 dB) is the compiled
     * fallback: measured with the console `tonetest` command, the dial tone
     * comes back from the SLIC at ~2% distortion at 21 but 19-37% at 30 -- the
     * AG1171's audio input is overdriven by a near-full-scale DAC. (31 was
     * previously picked by ear, which doesn't show distortion.)
     * board_speaker_volume_get() returns whatever was persisted via `spkamp` or
     * the web UI slider, so a level set by hand survives a reboot. */
    uint8_t spk_l = board_speaker_volume_get(0, ES8388_DEFAULT_SPK_VOL);
    uint8_t spk_r = board_speaker_volume_get(1, ES8388_DEFAULT_SPK_VOL);
    es8388_set_dac_volume(spk_l, spk_r);
    raw_write(ES8388_REG_LOUT2VOL, spk_l);             /* LOUT2 mirrors LOUT1 (J7 monitor header) */
    raw_write(ES8388_REG_ROUT2VOL, spk_r);             /* ROUT2 mirrors ROUT1 (J7 monitor header) */

    raw_write(ES8388_REG_ADCPOWER, 0xFF);              /* hold ADC down while configuring */
    /* MicAmpL=MicAmpR=0dB (chip's own power-on default -- explicit here so
     * the intent is documented, not left implicit). This used to be +3dB,
     * tuned 2026-07-20 to break a real acoustic/sidetone feedback loop that
     * appeared once both lines' mic+speaker were live simultaneously; 0dB
     * is strictly less gain than the +3dB that already killed that loop, so
     * it stays broken here too.
     *
     * Dropped from +3dB back to 0dB on 2026-08-21 for an unrelated reason:
     * once the SLIC->codec voltage divider was added (see
     * docs/signal-levels.md Sec.1B) to stop DTMF from clipping the ADC,
     * normal speech on the divided line came through as near-silence
     * (capture RMS ~0-300) at +3dB/+9dB, while DTMF still classified fine.
     * Root cause: the ES8388's mic PGA is (empirically, not documented)
     * a resistive-feedback gain stage whose INPUT impedance drops as gain
     * increases, which the divider's fixed 10k series resistor is sensitive
     * to -- at +3dB+ it collapses the divider ratio far below what the
     * resistor values alone predict. DTMF still worked at +3dB because it's
     * far louder than voice to begin with (that's why it was clipping
     * pre-divider) -- enough margin survived the collapse to clear the
     * detector's thresholds; ordinary speech didn't. At 0dB the codec's
     * input impedance matches the ~20k the divider was designed against,
     * giving a consistent, correctly-scaled signal (confirmed via `capture`
     * live on real hardware, RMS tracking the divider's expected ratio) --
     * confirmed DTMF still classifies all 12 keys correctly at 0dB too, via
     * `tonemon` on the real handset. */
    raw_write(ES8388_REG_ADCCONTROL1, 0x00);
    /* LINSEL=00 (LINPUT1, Line 0), RINSEL=00 (RINPUT1, Line 1) -- both fixed
     * forever now that each has its own dedicated ADC channel (see file
     * header). DSSEL/DSR (differential mode) left at 0, unused. */
    raw_write(ES8388_REG_ADCCONTROL2, 0x00);
    raw_write(ES8388_REG_ADCCONTROL3, 0x02);           /* single-ended inputs */
    raw_write(ES8388_REG_ADCCONTROL4, 0x0C);           /* I2S, 16-bit, slave */
    raw_write(ES8388_REG_ADCCONTROL5, 0x02);           /* 256fs MCLK/LRCK ratio */
    es8388_set_adc_volume(0x00, 0x00);                 /* 0 dB */
    raw_write(ES8388_REG_ADCPOWER, 0x09);              /* power up ADC L/R */

    es8388_set_dac_mute(false);

    ESP_LOGI(TAG, "init ok: addr=0x%02X fs=%uHz", c.i2c_addr, (unsigned)c.sample_rate_hz);
    return ESP_OK;
}

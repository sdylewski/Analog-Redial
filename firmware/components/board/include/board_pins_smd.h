/*
 * board_pins_smd.h — Pin map for Rotary_phone_SMD2 (ESP32-C6-WROOM-1 + ES8388).
 *
 * Verified against hardware/kicad/Rotary_phone_SMD2/Rotary_phone_SMD2.kicad_sch
 * (U2 = ESP32-C6-WROOM-1, U4 = ES8388, U5/U6 = AG1171 SLICs).
 *
 * The PCB supports two build options on GPIO 18–21 (mutually exclusive):
 *
 *   A) Line 2 populated (U5 + J7)     → dual-line gateway, no microSD
 *   B) Line 2 DNP, microSD populated    → single-line + SPI SD card
 *
 * Same copper on all boards; stuffing / CONFIG selects behavior.
 * See hardware/ROTARY_PHONE_SMD.md.
 *
 * Firmware "line 0" is wired to the SLIC labeled U6 on the schematic (net
 * labels /Line1_SHK etc.), and "line 1" to U5 (/Line2_SHK etc.) — the
 * schematic's Line1/Line2 net-label convention is the reverse of the
 * Tip/Ring-based U5/U6 naming used in CLAUDE.md's clearance table. Double
 * check against silkscreen (which RJ12 is J6 vs J7) before wiring up phones
 * during bring-up so "line 0"/"line 1" in the console matches the jack you
 * plugged in.
 */
#pragma once

#include "driver/gpio.h"
#include "board_config.h"

#ifndef CONFIG_BOARD_MICROSD
#define CONFIG_BOARD_MICROSD 0
#endif

#ifndef CONFIG_BOARD_NUM_LINES
#define CONFIG_BOARD_NUM_LINES 2
#endif

#define BOARD_NUM_LINES CONFIG_BOARD_NUM_LINES

/* ---- I2S (ES8388) ---- */
#define BOARD_I2S_MCLK    GPIO_NUM_0
#define BOARD_I2S_BCLK    GPIO_NUM_4
#define BOARD_I2S_LRCK    GPIO_NUM_5
#define BOARD_I2S_DOUT    GPIO_NUM_6
#define BOARD_I2S_DIN     GPIO_NUM_7

/* ---- I2C (ES8388, CE tied to GND -> 0x10) ---- */
#define BOARD_I2C_SDA     GPIO_NUM_22
#define BOARD_I2C_SCL     GPIO_NUM_23
#define BOARD_CODEC_I2C_ADDR 0x10

/* ---- Line 1 SLIC (always populated) ---- */
#define BOARD_LINE0_SHK   GPIO_NUM_1
#define BOARD_LINE0_FR    GPIO_NUM_2
#define BOARD_LINE0_RM    GPIO_NUM_3
#define BOARD_LINE0_PD    GPIO_NUM_10  /* GPIO14 not routed on WROOM-1 module */

/* ---- GPIO 18–21: Line 2 SLIC *or* microSD (same PCB pads, XOR populate) ----
 * Physical pins, unconditional regardless of CONFIG_BOARD_MICROSD --
 * board_detect_line2_variant() samples all four before either peripheral
 * claims them. Which *meaning* applies (SLIC control vs. SPI) is still the
 * build-time Kconfig choice below. */
#define BOARD_LINE2_PIN_PD_CS     GPIO_NUM_18
#define BOARD_LINE2_PIN_SHK_SCLK  GPIO_NUM_19
#define BOARD_LINE2_PIN_RM_MOSI   GPIO_NUM_20
#define BOARD_LINE2_PIN_FR_MISO   GPIO_NUM_21

#if CONFIG_BOARD_MICROSD
#define BOARD_SD_CS       BOARD_LINE2_PIN_PD_CS
#define BOARD_SD_SCLK     BOARD_LINE2_PIN_SHK_SCLK
#define BOARD_SD_MOSI     BOARD_LINE2_PIN_RM_MOSI
#define BOARD_SD_MISO     BOARD_LINE2_PIN_FR_MISO
#else
#define BOARD_LINE1_SHK   BOARD_LINE2_PIN_SHK_SCLK
#define BOARD_LINE1_FR    BOARD_LINE2_PIN_FR_MISO
#define BOARD_LINE1_RM    BOARD_LINE2_PIN_RM_MOSI
#define BOARD_LINE1_PD    BOARD_LINE2_PIN_PD_CS
#endif

#define BOARD_PD_LEVEL_RUN        0
#define BOARD_PD_LEVEL_POWERDOWN  1
#define BOARD_SHK_OFFHOOK_LEVEL   1

#define BOARD_UART_TX     GPIO_NUM_16
#define BOARD_UART_RX     GPIO_NUM_17

/* Status LED (D2 via R6 220R), active-high. SMD only -- no LED on THT. */
#define BOARD_STATUS_LED  GPIO_NUM_11

typedef struct {
    gpio_num_t shk;
    gpio_num_t fr;
    gpio_num_t rm;
    gpio_num_t pd;
} board_line_pins_t;

static inline board_line_pins_t board_line_pins(int line)
{
    if (line == 0) {
        return (board_line_pins_t){ BOARD_LINE0_SHK, BOARD_LINE0_FR,
                                    BOARD_LINE0_RM,  BOARD_LINE0_PD };
    }
#if !CONFIG_BOARD_MICROSD
    return (board_line_pins_t){ BOARD_LINE1_SHK, BOARD_LINE1_FR,
                                BOARD_LINE1_RM,  BOARD_LINE1_PD };
#else
    return (board_line_pins_t){ GPIO_NUM_NC, GPIO_NUM_NC, GPIO_NUM_NC, GPIO_NUM_NC };
#endif
}

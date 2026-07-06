/*
 * board_pins_smd.h — Pin map for rotary_phone_gateway_smd.
 *
 * The PCB supports two build options on GPIO 18–21 (mutually exclusive):
 *
 *   A) Line 2 populated (U4 + J2)     → dual-line gateway, no microSD
 *   B) Line 2 DNP, microSD populated    → single-line + SPI SD card
 *
 * Same copper on all boards; stuffing / CONFIG selects behavior.
 * See hardware/ROTARY_PHONE_SMD.md.
 */
#pragma once

#include "driver/gpio.h"

#ifndef CONFIG_BOARD_MICROSD
#define CONFIG_BOARD_MICROSD 0
#endif

#ifndef CONFIG_BOARD_NUM_LINES
#define CONFIG_BOARD_NUM_LINES 2
#endif

#define BOARD_NUM_LINES CONFIG_BOARD_NUM_LINES

/* ---- I2S (WM8731) ---- */
#define BOARD_I2S_MCLK    GPIO_NUM_0
#define BOARD_I2S_BCLK    GPIO_NUM_4
#define BOARD_I2S_LRCK    GPIO_NUM_5
#define BOARD_I2S_DOUT    GPIO_NUM_6
#define BOARD_I2S_DIN     GPIO_NUM_7

/* ---- I2C (WM8731 @ 0x1A) ---- */
#define BOARD_I2C_SDA     GPIO_NUM_22
#define BOARD_I2C_SCL     GPIO_NUM_23
#define BOARD_WM8731_ADDR 0x1A

/* ---- Line 1 SLIC (always populated) ---- */
#define BOARD_LINE0_SHK   GPIO_NUM_1
#define BOARD_LINE0_FR    GPIO_NUM_2
#define BOARD_LINE0_RM    GPIO_NUM_3
#define BOARD_LINE0_PD    GPIO_NUM_10  /* GPIO14 not routed on WROOM-1 module */

/* ---- GPIO 18–21: Line 2 SLIC *or* microSD (same PCB pads, XOR populate) ---- */
#if CONFIG_BOARD_MICROSD
#define BOARD_SD_CS       GPIO_NUM_18
#define BOARD_SD_SCLK     GPIO_NUM_19
#define BOARD_SD_MOSI     GPIO_NUM_20
#define BOARD_SD_MISO     GPIO_NUM_21
#else
#define BOARD_LINE1_SHK   GPIO_NUM_19
#define BOARD_LINE1_FR    GPIO_NUM_21
#define BOARD_LINE1_RM    GPIO_NUM_20
#define BOARD_LINE1_PD    GPIO_NUM_18
#endif

#define BOARD_PD_LEVEL_RUN        0
#define BOARD_PD_LEVEL_POWERDOWN  1
#define BOARD_SHK_OFFHOOK_LEVEL   1

#define BOARD_UART_TX     GPIO_NUM_16
#define BOARD_UART_RX     GPIO_NUM_17

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

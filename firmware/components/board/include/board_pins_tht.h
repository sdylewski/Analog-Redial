/*
 * board_pins_tht.h — ESP32-C6 Rotary Phone Gateway pin map.
 *
 * SINGLE SOURCE OF TRUTH for GPIO assignments. Mirrors the routed PCB
 * (hardware/kicad/rotary_phone_gateway) and hardware/WM8731.md.
 *
 * Lines are 0-indexed in firmware:
 *   LINE 0 = "Line 1" on the schematic (SLIC U5, jack J1, codec LEFT  channel)
 *   LINE 1 = "Line 2" on the schematic (SLIC U4, jack J2, codec RIGHT channel)
 *
 * Do NOT use GPIO8, GPIO9, GPIO15 (boot/JTAG straps) or GPIO12/GPIO13 (USB).
 */
#pragma once

#include "driver/gpio.h"
#include "board_config.h"

#ifndef CONFIG_BOARD_NUM_LINES
#define CONFIG_BOARD_NUM_LINES 2
#endif

#define BOARD_NUM_LINES CONFIG_BOARD_NUM_LINES

/* ---- I2S (WM8731 codec, ESP32 is master) ---- */
#define BOARD_I2S_MCLK    GPIO_NUM_0   /* XTI/MCLK  -> codec pin 25 */
#define BOARD_I2S_BCLK    GPIO_NUM_4   /* BCLK      -> codec pin 3  */
#define BOARD_I2S_LRCK    GPIO_NUM_5   /* DACLRC/ADCLRC (tied)      */
#define BOARD_I2S_DOUT    GPIO_NUM_6   /* MCU -> DACDAT (codec pin 4, playback) */
#define BOARD_I2S_DIN     GPIO_NUM_7   /* ADCDAT -> MCU (codec pin 6, record)   */

/* ---- I2C (WM8731 control) ---- */
#define BOARD_I2C_SDA     GPIO_NUM_22
#define BOARD_I2C_SCL     GPIO_NUM_23
#define BOARD_WM8731_ADDR 0x1A         /* MODE=GND, CSB=GND */
#define BOARD_CODEC_I2C_ADDR BOARD_WM8731_ADDR

/* ---- AG1170 SLIC control, per line ----
 * SHK : SLIC -> MCU (input)  1 = off-hook (after level conditioning)
 * F/R : MCU  -> SLIC (out)   toggled 20-25 Hz to ring; static otherwise
 * RM  : MCU  -> SLIC (out)   1 only during ringing
 * PD  : MCU  -> SLIC (out)   open-collector; see BOARD_PD_LEVEL_RUN below
 */
#define BOARD_LINE0_SHK   GPIO_NUM_1
#define BOARD_LINE0_FR    GPIO_NUM_2
#define BOARD_LINE0_RM    GPIO_NUM_3
#define BOARD_LINE0_PD    GPIO_NUM_14

#define BOARD_LINE1_SHK   GPIO_NUM_19
#define BOARD_LINE1_FR    GPIO_NUM_21
#define BOARD_LINE1_RM    GPIO_NUM_20
#define BOARD_LINE1_PD    GPIO_NUM_18

/*
 * PD drive polarity.
 * The AG1170 PD pin must NEVER see a logic high directly; it is driven by an
 * open-collector transistor (BC846) on this board. Per the AG1170 figure:
 *   "High input for Power Down, Low input for free run".
 * The MCU GPIO drives the transistor base, so:
 *   GPIO LOW  -> transistor off -> PD open  -> SLIC RUNS
 *   GPIO HIGH -> transistor on  -> PD low   -> SLIC POWERED DOWN
 * If your transistor stage inverts this, flip these two macros.
 */
#define BOARD_PD_LEVEL_RUN        0
#define BOARD_PD_LEVEL_POWERDOWN  1

/* SHK active level (after conditioning): 1 = off-hook. */
#define BOARD_SHK_OFFHOOK_LEVEL   1

/* ---- Debug UART headers (informational; default console uses these) ---- */
#define BOARD_UART_TX     GPIO_NUM_16
#define BOARD_UART_RX     GPIO_NUM_17

/* Convenience accessors keyed by line index. */
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
    return (board_line_pins_t){ BOARD_LINE1_SHK, BOARD_LINE1_FR,
                                BOARD_LINE1_RM,  BOARD_LINE1_PD };
}

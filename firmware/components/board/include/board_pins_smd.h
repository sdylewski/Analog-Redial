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

/* Array-sizing width, NOT how many lines are actually active -- that's now
 * runtime (board_hw_config_get(), board.h), not a build-time Kconfig choice.
 * Matches AUDIO_NUM_LINES (audio.h) and telephony.c's own NLINES vs.
 * NLINES_ACTIVE split, which this project already used for exactly this
 * "array is always max width, active count varies" pattern before this
 * header caught up to it. Fixed at 2: this PCB never has more than 2 lines
 * regardless of build option. */
#define BOARD_NUM_LINES 2

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
 * Physical pins, unconditional -- board_hw_detect_evidence() (board.h)
 * samples all four before either peripheral claims them. Which *meaning*
 * applies (SLIC control vs. SPI) used to be a build-time Kconfig choice;
 * now it's board_hw_config_get() at runtime (board.h) -- both naming
 * schemes are defined unconditionally here since they're just two names for
 * the same four physical pins, and it's up to whichever driver actually
 * initializes them (ag1170's line 2, or sdcard_init()) to check the runtime
 * config first and only claim them if it's actually the one selected. */
#define BOARD_LINE2_PIN_PD_CS     GPIO_NUM_18
#define BOARD_LINE2_PIN_SHK_SCLK  GPIO_NUM_19
#define BOARD_LINE2_PIN_RM_MOSI   GPIO_NUM_20
#define BOARD_LINE2_PIN_FR_MISO   GPIO_NUM_21

#define BOARD_SD_CS       BOARD_LINE2_PIN_PD_CS
#define BOARD_SD_SCLK     BOARD_LINE2_PIN_SHK_SCLK
#define BOARD_SD_MOSI     BOARD_LINE2_PIN_RM_MOSI
#define BOARD_SD_MISO     BOARD_LINE2_PIN_FR_MISO

#define BOARD_LINE1_SHK   BOARD_LINE2_PIN_SHK_SCLK
#define BOARD_LINE1_FR    BOARD_LINE2_PIN_FR_MISO
#define BOARD_LINE1_RM    BOARD_LINE2_PIN_RM_MOSI
#define BOARD_LINE1_PD    BOARD_LINE2_PIN_PD_CS

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

/* Always returns line 1's real pins regardless of runtime config -- this is
 * just the pin *map*, not a claim that line 1 is actually populated/active.
 * Callers (ag1170's line-2 init) must check board_hw_config_get().num_lines
 * before actually configuring these as a SLIC; on a board where GPIO18-21
 * are wired to a microSD instead, driving them as SLIC control lines would
 * just be wrong, not merely unused. */
static inline board_line_pins_t board_line_pins(int line)
{
    if (line == 0) {
        return (board_line_pins_t){ BOARD_LINE0_SHK, BOARD_LINE0_FR,
                                    BOARD_LINE0_RM,  BOARD_LINE0_PD };
    }
    return (board_line_pins_t){ BOARD_LINE1_SHK, BOARD_LINE1_FR,
                                BOARD_LINE1_RM,  BOARD_LINE1_PD };
}

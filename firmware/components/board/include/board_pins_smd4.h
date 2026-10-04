/*
 * board_pins_smd4.h — Pin map for Analog Redial v3 / v4 (ESP32-C6-DevKitC-1
 * + ES8388). SMD4 is a from-scratch 2-layer re-route of SMD3's unchanged
 * schematic (same nets, same pin assignments, same components) -- this one
 * header covers both spins.
 *
 * Verified against the schematic netlist (kicad-cli sch export netlist),
 * not silkscreen or a guess -- see archive/hardware/ROTARY_PHONE_SMD3.md
 * ("Pin map (ESP32-C6-DevKitC-1, U1)", confirmed 2026-08-30) and
 * docs/hardware.md section 3. Genuinely different from board_pins_smd.h
 * (SMD2): moving the MCU carrier from a soldered WROOM-1 module to a
 * DevKitC-1 dev board freed up GPIO0/1/2 and remapped I2S there, which in
 * turn pushed line 0's SLIC control onto GPIO4-7 instead of SMD2's I2S
 * pins there. Two specific gotchas worth calling out because they're easy
 * to get backwards from memory or from SMD2's header:
 *
 *   - I2S pins are NOT the same GPIOs as SMD2 (compare BOARD_I2S_* below to
 *     board_pins_smd.h) -- SMD2 put I2S on GPIO0/4/5/6/7; here it's on
 *     GPIO0/1/2/10/11, freeing GPIO4-7 for line 0's SLIC.
 *   - The GPIO18-21 to signal mapping is NOT the same as SMD2's, even
 *     though both boards XOR the same four pins between the shared line's
 *     SLIC and a microSD socket: SMD2 pairs GPIO18 with PD/CS and GPIO21
 *     with F/R and MISO; this board pairs GPIO18 with F/R and MISO, and
 *     GPIO21 with PD and CS -- 18 and 21 are swapped. GPIO19 (SHK/CLK) and
 *     GPIO20 (RM/MOSI) match SMD2. Confirmed against
 *     archive/hardware/ROTARY_PHONE_SMD3.md's J5 jumper table, not carried
 *     over from SMD2's header by assumption.
 *
 * No status LED on this board (GPIO11, SMD2's LED pin, is I2S BCLK here) --
 * BOARD_STATUS_LED is deliberately not defined; board.c's LED code is
 * gated on CONFIG_BOARD_TYPE_SMD2 specifically (not the general "is this
 * an SMD board" check) for exactly this reason, so it correctly no-ops on
 * this board.
 *
 * The PCB supports the same two build options on GPIO 18-21 as SMD2
 * (mutually exclusive, runtime-selected via board_hw_config_get(), not a
 * separate firmware build):
 *
 *   A) The shared line's SLIC populated (U3 + J8)  -> dual-line gateway, no microSD
 *   B) That SLIC DNP, microSD populated instead    -> single-line + SPI SD card
 *
 * Reference designators (schematic, not silk): U4 = line 0's AG1171 @ J9,
 * U3 = line 1's AG1171 @ J8 -- see docs/hardware.md section 3's
 * "Reference/silk numbering note" if cross-checking against the board
 * itself.
 *
 * Macro naming note: the shared GPIO18-21 group keeps the BOARD_LINE2_PIN_*
 * prefix board.c (board_hw_detect_evidence()) hardcodes by that exact
 * name, even though this board's own documentation calls that same shared
 * line "line 1" -- a naming holdover from SMD2 (where the shared line
 * really was numbered "line 2"), now just a fixed cross-board identifier
 * every SMD-family pin-map header has to define, not a claim about which
 * line number this board calls it.
 */
#pragma once

#include "driver/gpio.h"
#include "board_config.h"

/* Array-sizing width, NOT how many lines are actually active -- see
 * board_pins_smd.h's identical comment on this same pattern. Fixed at 2:
 * this PCB never has more than 2 lines regardless of build option. */
#define BOARD_NUM_LINES 2

/* ---- I2S (ES8388) ---- */
#if CONFIG_BOARD_TYPE_SMD5_CHEAP
/* The dual MCU footprint swaps pads 12/13 <-> 19/20 between the real
 * Espressif module and the AliExpress clone (archive/hardware/
 * ANALOG_REDIAL_SMD5.md). The I2S_MCLK and SD_CD nets sit on pads 12/13,
 * which are GPIO2/3 on the real module but the clone's GPIO12/13 -- the
 * native-USB pins, freed up by board_early_pin_init() before audio starts. */
#define BOARD_I2S_MCLK    GPIO_NUM_12
#else
#define BOARD_I2S_MCLK    GPIO_NUM_2
#endif
#define BOARD_I2S_BCLK    GPIO_NUM_11
#define BOARD_I2S_LRCK    GPIO_NUM_1
#define BOARD_I2S_DOUT    GPIO_NUM_10
#define BOARD_I2S_DIN     GPIO_NUM_0

/* ---- I2C (ES8388, CE tied to GND -> 0x10) ---- */
#define BOARD_I2C_SDA     GPIO_NUM_22
#define BOARD_I2C_SCL     GPIO_NUM_23
#define BOARD_CODEC_I2C_ADDR 0x10

/* ---- Line 0 SLIC (U4 @ J9, always populated) ---- */
#define BOARD_LINE0_FR    GPIO_NUM_4  /* MTMS, strap -- safe: ESP32 drives this as an output */
#define BOARD_LINE0_RM    GPIO_NUM_5  /* MTDI, strap -- same reasoning */
#define BOARD_LINE0_SHK   GPIO_NUM_6
#define BOARD_LINE0_PD    GPIO_NUM_7

/* ---- GPIO 18-21: line 1's SLIC (U3 @ J8) or microSD (same pads, XOR
 * populate, jumper-selected on header J5 rather than a build-time
 * choice) ----
 * Physical pins, unconditional -- board_hw_detect_evidence() (board.h)
 * samples all four before either peripheral claims them. Which meaning
 * applies is a runtime choice (board_hw_config_get(), board.h); it's up to
 * whichever driver actually initializes them (ag1170's line 1, or
 * sdcard_init()) to check that config first. NOTE the 18/21 swap vs
 * board_pins_smd.h documented at the top of this file -- do not copy
 * SMD2's exact pin assignments here without re-deriving them, the
 * pairings are different even though the macro names match. */
#define BOARD_LINE2_PIN_FR_MISO   GPIO_NUM_18
#define BOARD_LINE2_PIN_SHK_SCLK  GPIO_NUM_19
#define BOARD_LINE2_PIN_RM_MOSI   GPIO_NUM_20
#define BOARD_LINE2_PIN_PD_CS     GPIO_NUM_21

#define BOARD_SD_MISO     BOARD_LINE2_PIN_FR_MISO
#define BOARD_SD_SCLK     BOARD_LINE2_PIN_SHK_SCLK
#define BOARD_SD_MOSI     BOARD_LINE2_PIN_RM_MOSI
#define BOARD_SD_CS       BOARD_LINE2_PIN_PD_CS

#define BOARD_LINE1_FR    BOARD_LINE2_PIN_FR_MISO
#define BOARD_LINE1_SHK   BOARD_LINE2_PIN_SHK_SCLK
#define BOARD_LINE1_RM    BOARD_LINE2_PIN_RM_MOSI
#define BOARD_LINE1_PD    BOARD_LINE2_PIN_PD_CS

/* microSD card-detect (Card1 pin 9), SMD4 only -- SMD3 left this pin
 * unconnected. Internal weak pull-up (~45k), no external resistor needed;
 * normally-open switch shorts to GND when a card is seated, so LOW means
 * card present. See docs/hardware.md section 6 and sdcard.c's BOARD_SD_CD
 * handling (compiles to a no-op card-always-present check on any board,
 * like this one until now, that doesn't define this macro).
 *
 * NOTE: as of the last design-doc update before this board was fabricated,
 * the GPIO3-to-Card1-pin-9 schematic net was still marked "planned, not
 * yet drawn". Confirm this actually reads a real card-detect signal (not
 * floating) during bring-up before trusting it, same as every other pin
 * on a first-fabrication board. */
#if CONFIG_BOARD_TYPE_SMD5_CHEAP
#define BOARD_SD_CD       GPIO_NUM_13   /* clone: pad 13 -- see BOARD_I2S_MCLK above */
#else
#define BOARD_SD_CD       GPIO_NUM_3
#endif

#define BOARD_PD_LEVEL_RUN        0
#define BOARD_PD_LEVEL_POWERDOWN  1
#define BOARD_SHK_OFFHOOK_LEVEL   1

#define BOARD_UART_TX     GPIO_NUM_16
#define BOARD_UART_RX     GPIO_NUM_17

/* Bring-up/bench-only: BM64 MFB/RST_N/P2_0 control, for boards that have
 * temporarily wired a BM64 companion module to the same GPIO18-20 pads line
 * 1's SLIC/microSD would otherwise use (see the shared-pad comment above).
 * These #defines exist on every SMD4 board unconditionally (harmless if
 * unused), but the pins themselves are only safe to actually drive when
 * this specific board's runtime config (board_hw_config_get(), board.h) is
 * single-line with no microSD -- bringup.c's bm64_pin console command
 * checks that before touching any of them, same pattern ag1170.c/sdcard.c
 * already use for these same shared pads. Not part of bt_companion's normal
 * runtime path -- dualphone's bench BM64 wiring ties MFB to VCC with a
 * fixed resistor instead and has no GPIO control over any of these three. */
#if CONFIG_BOARD_TYPE_SMD5 || CONFIG_BOARD_TYPE_SMD5_CHEAP
/* SMD5 gives MFB a pin of its own on the expanded J12 header: GPIO15, the
 * only genuinely spare pin on this pin map (GPIO8 drives the DevKit's RGB
 * LED, GPIO9 is the download-mode strap -- MFB's pull-down there would boot
 * the board into download mode every time). GPIO15 is a strapping pin too,
 * but it only selects the JTAG source and only when an eFuse that ships
 * unburned is set, so an external 100k pull-down on it is harmless.
 * Because it's dedicated, MFB works on a 2-line/microSD board as well --
 * unlike RST_N/P2_0, which SMD5 only borrows through J5 for one-time
 * EEPROM programming (see archive/hardware/ANALOG_REDIAL_SMD5.md). */
#define BOARD_BT_MFB_PIN  GPIO_NUM_15
#define BOARD_BT_MFB_DEDICATED 1
#else
#define BOARD_BT_MFB_PIN  BOARD_LINE2_PIN_FR_MISO  /* GPIO18 */
#endif
#define BOARD_BT_RST_PIN  BOARD_LINE2_PIN_SHK_SCLK /* GPIO19 */
#define BOARD_BT_P20_PIN  BOARD_LINE2_PIN_RM_MOSI  /* GPIO20 */

/* No status LED on this board -- BOARD_STATUS_LED intentionally not
 * defined, see this file's own top-of-file comment. */

typedef struct {
    gpio_num_t shk;
    gpio_num_t fr;
    gpio_num_t rm;
    gpio_num_t pd;
} board_line_pins_t;

/* Always returns line 1's real pins regardless of runtime config -- this is
 * just the pin map, not a claim that line 1 is actually populated/active.
 * Callers (ag1170's line-1 init) must check board_hw_config_get().num_lines
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

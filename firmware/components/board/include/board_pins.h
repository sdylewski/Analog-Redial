/*
 * board_pins.h — board-type wrapper for the ESP32-C6 gateway.
 *
 * Selects the SMD board's pin map at compile time.
 */
#pragma once

#include "board_config.h"

#if CONFIG_BOARD_TYPE_SMD2
#include "board_pins_smd.h"
#elif CONFIG_BOARD_TYPE_SMD4 || CONFIG_BOARD_TYPE_SMD5 || CONFIG_BOARD_TYPE_SMD5_CHEAP
/* SMD5 (either MCU module variant) shares SMD4's pin map exactly -- see
 * Kconfig's help text and archive/hardware/ANALOG_REDIAL_SMD5.md. The two
 * module variants differ only in which physical pad carries GPIO2/3 vs.
 * GPIO12/13 (native USB), which nothing in this pin map currently uses. */
#include "board_pins_smd4.h"
#else
#error "Unknown CONFIG_BOARD_TYPE_* -- pick a board in menuconfig"
#endif

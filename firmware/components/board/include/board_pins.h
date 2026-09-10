/*
 * board_pins.h — board-type wrapper for the ESP32-C6 gateway.
 *
 * The firmware builds the same code for the through-hole gateway and the
 * SMD variants. This header selects the correct pin map at compile time.
 */
#pragma once

#include "board_config.h"

#if CONFIG_BOARD_TYPE_SMD2
#include "board_pins_smd.h"
#elif CONFIG_BOARD_TYPE_SMD4
#include "board_pins_smd4.h"
#else
#include "board_pins_tht.h"
#endif

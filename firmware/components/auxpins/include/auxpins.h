/*
 * auxpins.h -- the four spare GPIOs of a one-line board, as outputs.
 *
 * GPIO18-21 are shared: line 1's SLIC (through the J5 jumper shunts) and the
 * microSD socket both use them. A one-line board with neither a second line
 * nor a card leaves them free, and they're wired out to the 2.54 mm J5
 * header, so a dial code can drive one high to switch a relay or a MOSFET (a
 * solenoid needs more current than a pin can give), sound a buzzer, flash a
 * light.
 *
 * All of these are refused unless the pins really are free: the board is
 * configured as one line with no microSD (Settings -> Hardware, "Neither"),
 * and no card is in the socket. J5's shunts must be OUT: with a shunt in,
 * the pin is wired to line 1's SLIC control inputs.
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AUXPINS_COUNT 4

/* The GPIO numbers usable as outputs, in a fixed order (18, 19, 20, 21 on
 * every board so far). Returns the number written (<= max). */
size_t auxpins_list(int *gpio_out, size_t max);
bool   auxpins_valid(int gpio);

/* True when the pins are free to drive. If not, `why` (if non-NULL) says
 * what's in the way, in a sentence the web UI can show. */
bool auxpins_usable(char *why, size_t why_len);

/* secs > 0: drive high for that many seconds, then low (a second dial of the
 * same code while it's high restarts the time). secs == 0: toggle.
 * ESP_ERR_INVALID_STATE if the pins aren't usable, ESP_ERR_INVALID_ARG for a
 * GPIO that isn't one of the four. */
esp_err_t auxpins_pulse(int gpio, int secs);

/* Current pad level (read back through the input buffer). */
bool auxpins_level(int gpio);

/* All four low and released. Called when the pins stop being usable. */
void auxpins_all_off(void);

#ifdef __cplusplus
}
#endif

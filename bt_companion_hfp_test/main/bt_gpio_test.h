#pragma once

/* Raw GPIO console commands for bench bring-up -- see bt_gpio_test.c.
 * Bypasses Bluetooth/bt_audio_io.c entirely: for confirming basic wiring
 * continuity with a multimeter/scope when something upstream (the tone/
 * level diagnostics) isn't showing what's expected, to rule software
 * complexity in or out. */
void bt_gpio_test_register_commands(void);

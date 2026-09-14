#pragma once
#include <stdbool.h>

/* Onboard status LED on a standard ESP32 DevKitC-style board -- GPIO2, the
 * near-universal default on genuine DevKitC boards (not all clones). If
 * this board's LED doesn't light up, it's likely on a different GPIO;
 * change STATUS_LED_GPIO in bt_led.c. */

void bt_led_init(void);

/* Starts a ~300ms blink and auto-stops after a timeout (see
 * PAIRING_TIMEOUT_US in bt_led.c) if nothing connects in the meantime.
 * Called from bt_uart_link.c on a "PAIR" request from the C6. */
void bt_led_start_pairing(void);

/* Stops any blink in progress; solid on if connected, off if not. Called
 * from bt_app_hf.c's connection-state handler. */
void bt_led_set_connected(bool connected);

/* bt_uart_link.h — the ESP32 (companion) side of the line-based protocol
 * documented in firmware/components/bt_companion/include/bt_companion.h
 * (the C6 side, already merged into the main board's firmware). Listens on
 * UART2 (GPIO16=RX2, GPIO17=TX2 -- crossed against the C6's own GPIO16=TX/
 * GPIO17=RX, per archive/BLUETOOTH_AUDIO_UPGRADE.md's pin table) for
 * "STATUS?" and "PAIR" commands, replying "STATUS CONNECTED <addr>" /
 * "STATUS DISCONNECTED" so the C6's Settings -> Bluetooth tab shows real
 * status instead of "not responding".
 */
#pragma once

#include <stdbool.h>
#include "esp_bt_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Starts the UART2 link + its listener task. Call once at boot. */
void bt_uart_link_init(void);

/* Called from bt_app_hf.c's own connection-state handler so this module
 * always has an up to date "connected" flag to answer STATUS? with,
 * without needing its own duplicate HFP event subscription. */
void bt_uart_link_set_connected(bool connected, const esp_bd_addr_t addr);

#ifdef __cplusplus
}
#endif

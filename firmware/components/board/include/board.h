/* board.h — board-level init helpers shared across components. */
#pragma once

#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize the shared I2C master bus (codec control). Idempotent. */
esp_err_t board_i2c_init(void);

/* Handle to the shared I2C master bus, or NULL if not initialized. */
i2c_master_bus_handle_t board_i2c_bus(void);

/* Probes the I2C bus for both known codec addresses (ES8388 @ 0x10, WM8731 @
 * 0x1A) and logs a warning if neither ACKs, or if the codec that answered
 * doesn't match the board type this firmware was built for (CONFIG_BOARD_TYPE_*).
 * This is a sanity check, not a substitute for selecting the right Kconfig
 * board type at build time — GPIO pin routing is fixed per PCB and can't be
 * switched at runtime. Safe to call before or after board_i2c_init(). */
void board_detect_codec(void);

/* SMD boards only: samples GPIO19 (Line2_SHK / SD_SCLK, same PCB net) before
 * either the AG1170 or an SD driver has configured it, and warns if what it
 * reads doesn't match CONFIG_BOARD_MICROSD. Same caveat as
 * board_detect_codec(): sanity check against the build-time Kconfig choice,
 * not a runtime switch -- GPIO 18-21's peripheral routing (SLIC control vs.
 * SPI) still has to be selected at build time. No-op on the THT board.
 * Call before ag1170_init()/any SD driver init claims the pin. */
void board_detect_line2_variant(void);

typedef enum {
    BOARD_LED_BOOTING,         /* fast blink -- subsystems still coming up */
    BOARD_LED_OK,               /* solid off -- everything initialized cleanly */
    BOARD_LED_ERROR,            /* slow blink -- a subsystem failed to init but
                                  * the device kept running (see app_main.c) */
    BOARD_LED_WIFI_CONNECTING,  /* slow PWM breathe -- WiFi connect in progress */
} board_led_state_t;

/* Configures the status LED (SMD only, GPIO11 -- no-op on THT, which has no
 * LED) and starts it blinking fast (BOARD_LED_BOOTING). Call once, early in
 * app_main(), before other subsystem init so there's visible boot feedback
 * even if something later hangs. */
void board_led_init(void);

/* Change the blink pattern. Safe to call from any task. */
void board_led_set_state(board_led_state_t state);

#ifdef __cplusplus
}
#endif

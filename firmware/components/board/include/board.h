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

#ifdef __cplusplus
}
#endif

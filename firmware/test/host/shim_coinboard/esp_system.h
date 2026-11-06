#pragma once
#include "esp_err.h"
typedef void (*shutdown_handler_t)(void);
esp_err_t esp_register_shutdown_handler(shutdown_handler_t h);

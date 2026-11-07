#pragma once
#include <stdio.h>
/* Evaluate the arguments (and check the format), print nothing. */
#define ESP_LOGD(t, ...) ((void)(t), (void)snprintf(NULL, 0, __VA_ARGS__))
#define ESP_LOGI(t, ...) ((void)(t), (void)snprintf(NULL, 0, __VA_ARGS__))
#define ESP_LOGW(t, ...) ((void)(t), (void)snprintf(NULL, 0, __VA_ARGS__))

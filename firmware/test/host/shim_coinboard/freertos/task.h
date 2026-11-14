#pragma once
#include "FreeRTOS.h"
#define ulTaskNotifyTake(a, b) (0)
#define xTaskNotifyGive(t) ((void)(t))
#define xTaskCreate(f, n, s, a, p, h) ((void)(f), *(h) = (void *)1, pdPASS)
#define vTaskDelay(t) ((void)(t))
#define vTaskDelete(h) ((void)(h))

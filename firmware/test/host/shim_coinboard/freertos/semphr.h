#pragma once
#include "FreeRTOS.h"
#define xSemaphoreCreateMutex() ((SemaphoreHandle_t)1)
#define xSemaphoreTake(s, t) (pdTRUE)
#define xSemaphoreGive(s) (pdTRUE)

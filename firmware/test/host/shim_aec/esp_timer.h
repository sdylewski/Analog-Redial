#pragma once
#include <stdint.h>
#include <time.h>
/* Host test: a real clock, so the test can report what the filter costs here. */
static inline int64_t esp_timer_get_time(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

#pragma once
#include <stdint.h>
/* Host-test stand-in: no NVS, so every line gets the caller's default delay. */
static inline uint32_t board_aec_delay_get(int line, uint32_t fallback) { (void)line; return fallback; }

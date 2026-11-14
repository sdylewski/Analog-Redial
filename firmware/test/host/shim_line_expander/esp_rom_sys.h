#pragma once
#include <stdint.h>
/* Host test: the probe's short waits don't need to wait. */
static inline void esp_rom_delay_us(uint32_t us) { (void)us; }

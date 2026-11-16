/*
 * green_box.h -- green box tone generation: 2200 + 2600 Hz burst patterns
 * sent down the phone line for coin collect and return.
 *
 * Used by tone-controlled payphones (roughly 1950s-1980s) whose totalizer
 * has a built-in tone decoder.  No extra hardware needed -- just audio on
 * tip/ring from the AG1171.
 *
 *   Collect:  1 burst  (~700 ms)
 *   Return:   2 bursts (~700 ms each, ~200 ms gap)
 *   Ringback: 3 bursts (~700 ms each, ~200 ms gaps)
 *
 * See PayPhone/payphone-types.md for background.
 */
#pragma once

#include "payphone.h"
#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Burst timing (ms).  Matches the most common Bell System references;
 * adjustable here if a phone's totalizer needs different timing. */
#define GB_BURST_MS     700
#define GB_GAP_MS       200
#define GB_F1           2200.0f
#define GB_F2           2600.0f
#define GB_AMP          0.8f    /* 0..1; same ballpark as DTMF */

void green_box_init(void);

/* Start a burst sequence on `line`.  Non-blocking: uses an esp_timer
 * internally to schedule the gaps between bursts.
 * Returns ESP_ERR_INVALID_STATE if a sequence is already running. */
esp_err_t green_box_fire(int line, payphone_relay_t action);

/* True while a burst sequence is in progress. */
bool green_box_busy(void);

#ifdef __cplusplus
}
#endif

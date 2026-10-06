/*
 * ag1170.h — Silvertel AG1170 SLIC driver (two lines).
 *
 * Responsibilities:
 *   - Drive PD (power), RM (ring mode), F/R (forward/reverse / ring toggle).
 *   - Read SHK with glitch filtering.
 *   - Detect on-hook / off-hook (debounced) and ROTARY PULSE digits.
 *   - Generate ring cadence (RM high + F/R toggled 20-25 Hz).
 *
 * A single monitor task (1 ms tick) services both lines and emits events to a
 * user callback. The telephony layer consumes these events; it never touches
 * GPIO directly.
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AG1170_EV_OFFHOOK = 0, /* handset lifted (or ring-trip answered) */
    AG1170_EV_ONHOOK,      /* handset hung up (sustained on-hook) */
    AG1170_EV_DIGIT,       /* rotary pulse digit decoded; .digit holds 0-9 */
} ag1170_event_type_t;

typedef struct {
    int                  line;
    ag1170_event_type_t  type;
    int                  digit; /* valid for AG1170_EV_DIGIT */
} ag1170_event_t;

typedef void (*ag1170_event_cb)(const ag1170_event_t *ev, void *ctx);

/* Tunable timings (ms). Defaults match the AG1170 datasheet guidance. */
typedef struct {
    uint16_t glitch_ms;        /* SHK glitch filter (default 2) */
    uint16_t offhook_confirm_ms;/* sustained off-hook => OFFHOOK (default 80) */
    uint16_t onhook_confirm_ms; /* sustained on-hook  => ONHOOK  (default 500) */
    uint16_t pulse_break_min_ms;/* loop-open >= this counts as a dial pulse (default 15) */
    uint16_t pulse_break_max_ms;/* loop-open <= this counts as a dial pulse (default 120) */
    uint16_t interdigit_ms;     /* quiet after pulses => digit done (default 300) */
    uint16_t ring_freq_hz;      /* F/R toggle frequency (default 20) */
    uint16_t ring_on_ms;        /* cadence on  (default 2000) */
    uint16_t ring_off_ms;       /* cadence off (default 4000) */
} ag1170_config_t;

#define AG1170_DEFAULT_CONFIG() (ag1170_config_t){ \
    .glitch_ms = 2, .offhook_confirm_ms = 80, .onhook_confirm_ms = 500, \
    .pulse_break_min_ms = 15, .pulse_break_max_ms = 120, .interdigit_ms = 300, \
    .ring_freq_hz = 20, .ring_on_ms = 2000, .ring_off_ms = 4000 }

esp_err_t ag1170_init(const ag1170_config_t *cfg);

void ag1170_set_event_cb(ag1170_event_cb cb, void *ctx);

/* Power: true = run (PD open), false = power down. */
esp_err_t ag1170_set_power(int line, bool run);

/* Ringing (US cadence by default). */
void ag1170_ring_start(int line);
/* Same, with the intercom cadence: two short bursts (0.4 s, 0.2 s gap, 0.4 s), 2 s pause. */
void ag1170_ring_start_intercom(int line);
void ag1170_ring_stop(int line);
bool ag1170_is_ringing(int line);

/* Configured cadence, for callers that need to reason about ring timing
 * (telephony.c's answering-machine ring counter) without duplicating the
 * constants from ag1170_config_t/AG1170_DEFAULT_CONFIG(). */
void ag1170_ring_cadence_ms(uint16_t *on_ms, uint16_t *off_ms);

/* Current debounced hook state. */
bool ag1170_is_offhook(int line);

/* Raw SHK level (for bring-up only). */
int  ag1170_raw_shk(int line);

/* In-progress pulse count for the digit currently being dialed (resets to 0
 * once the inter-digit timeout fires and the digit is emitted). Bring-up
 * only -- normal digit collection should use the AG1170_EV_DIGIT callback. */
int  ag1170_pulse_count(int line);
bool ag1170_is_counting_digit(int line);

/* Low-level line control (bring-up only). */
void ag1170_set_fr(int line, int level);
void ag1170_set_rm(int line, int level);

#ifdef __cplusplus
}
#endif

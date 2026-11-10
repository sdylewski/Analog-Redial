/*
 * coinboard.h -- the payphone coin board (PayPhone/Payphone_coin_board_v1 in the repo):
 * its coin switches as a coin source, and its coin relay for collect and
 * return. Part of the Payphone tab (payphone.h); runs only while that and
 * "Coin board connected" are both on, so a board without one never touches
 * the bus for it.
 *
 * On the shared I2C bus (the main board's J11 on v6; the same signals on any
 * other board): U1, a TCA9534A at 0x39, and U4, an ADS1115 at 0x48. A task
 * reads U1 every COINBOARD_POLL_MS for the coin inputs, drives EN_HV / POL /
 * FIRE through coin_fire.h's sequence, and keeps U4 converting: the 20 V
 * rail, HV, the coil current, and the spare AIN3. U4 is optional: without it
 * the sequence uses fixed times and can't tell whether the relay fired.
 *
 * Written before any coin board exists: not yet tested on hardware.
 */
#pragma once

#include "esp_err.h"
#include "coin_fire.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define COINBOARD_IO_ADDR   0x39    /* U1: A0 to VIO, A1 and A2 to GND */
#define COINBOARD_ADC_ADDR  0x48    /* U4: ADDR to GND */
#define COINBOARD_POLL_MS   3

/* U1's pins (coin-board.md Part 1, section 4). Inputs read low while closed. */
#define COINBOARD_BIT_COIN4   0
#define COINBOARD_BIT_QUARTER 1
#define COINBOARD_BIT_DIME    2
#define COINBOARD_BIT_NICKEL  3
#define COINBOARD_BIT_FIRE    4
#define COINBOARD_BIT_POL     5
#define COINBOARD_BIT_EN_HV   6
#define COINBOARD_BIT_SPARE   7     /* not connected: driven low so it can't float */
#define COINBOARD_INPUTS      0x0F  /* P0-P3 */
#define COINBOARD_CONFIG      COINBOARD_INPUTS   /* configuration register: 1 = input */

/* U4's channels (coin-board.md, Part 2, "The ADS1115"). */
#define COINBOARD_AIN_RAIL   0      /* 20V through R17 150 k / R19 27 k */
#define COINBOARD_AIN_HV     1      /* HV through R32 1 M / R33 27 k */
#define COINBOARD_AIN_ISENSE 2      /* R26 (10 ohm) through R44 1 k */
#define COINBOARD_AIN_SPARE  3      /* J13 pin 2, 0 to VIO */

typedef struct {
    bool     enabled;      /* a coin board is connected */
    uint8_t  line;         /* the phone line it's in */
    bool     auto_relay;   /* collect when a call is answered, return on hang-up */
    bool     swap;         /* collect is POL high: the relay's wires the other way round */
    uint16_t pulse_ms;     /* FIRE pulse, CF_PULSE_MIN_MS-CF_PULSE_MAX_MS */
    uint8_t  coin4_cents;  /* coin 4's value; 0 = not a coin (logged only) */
} coinboard_cfg_t;

/* Loads the settings, registers with payphone.h and starts the task. Call
 * after payphone_init() and board_i2c_init(). When enabled, the first thing
 * it does is set U1's outputs low: its registers survive an ESP32 reset, so a
 * crash mid-pulse could have left FIRE high. */
void coinboard_init(void);

coinboard_cfg_t coinboard_cfg(void);
esp_err_t coinboard_cfg_set(const coinboard_cfg_t *cfg);

/* Enabled and the Payphone features on: the task is talking to the board. */
bool coinboard_active(void);

/* A test from the web UI or console: collect, return, or charge HV and hold
 * it for CF_HVTEST_MS. ESP_ERR_INVALID_STATE unless active and U1 answers. */
esp_err_t coinboard_test(cf_what_t what);

typedef struct {
    bool        enabled, active;
    bool        io_ok, adc_ok;      /* U1, U4 answering */
    uint8_t     input, output;      /* U1's input register as last read, output as last written */
    uint8_t     closed;             /* coin inputs closed now (bit = COINBOARD_BIT_*) */
    uint8_t     stuck;              /* ... held closed for COIN_IN_STUCK_MS */
    int         rail_mv, hv_mv, i_ma, ain3_mv;  /* -1: not read */
    cf_state_t  state;
    cf_what_t   doing;
    cf_result_t result;
    char        result_msg[96];
    uint32_t    coins, fires, bus_errors, chip_resets;
} coinboard_status_t;

void coinboard_status(coinboard_status_t *out);

/* U1's pin names for an input bit, e.g. "quarter". */
const char *coinboard_input_name(int bit);

/* ADC conversions (raw ADS1115 counts at +-4.096 V full scale), exposed for
 * the host test. */
int coinboard_rail_mv(int raw);
int coinboard_hv_mv(int raw);
int coinboard_isense_ma(int raw);
int coinboard_ain_mv(int raw);

#ifdef __cplusplus
}
#endif

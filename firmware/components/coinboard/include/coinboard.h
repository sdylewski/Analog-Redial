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
 * Tested on coin board v1 with a GTE / Quadrum payphone (HD550002C relay).
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
    bool     auto_relay;   /* collect when a call is answered, return on hang-up (default off: bench testing) */
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
    cf_what_t   last_what;          /* what `result` is about */
    char        result_msg[96];
    uint32_t    coins, fires, bus_errors, chip_resets;
    /* For bring-up: inputs seen closed since coinboard_seen_clear() (or
     * boot), and the highest readings during the last (or current) collect,
     * return or HV test; -1: none measured. */
    uint8_t     seen;
    int         peak_rail_mv, peak_hv_mv, peak_i_ma;
    /* Per coin input (COINBOARD_BIT_*): coins counted, closures too short to count, longest closure ms */
    uint32_t    in_counted[4], in_short[4];
    int32_t     in_max_ms[4];
} coinboard_status_t;

void coinboard_status(coinboard_status_t *out);
void coinboard_seen_clear(void);

/* Bring-up: the rail, HV and coil current every COINBOARD_TRACE_MS from the
 * start of the last collect, return or HV test (-1: not measured). */
#define COINBOARD_TRACE_N  64
#define COINBOARD_TRACE_MS 50
typedef struct { int16_t rail_dv, hv_dv, i_ma; } coinboard_trace_pt_t;   /* rail, HV in 0.1 V */
int coinboard_trace(coinboard_trace_pt_t *out, int max);   /* returns the number of points */

/* Bring-up: do U1 and U4 answer on the bus? Works whether or not the coin
 * board is enabled (one probe each, no register writes). */
void coinboard_probe(bool *io_ok, bool *adc_ok);

/* ---- Fire trace and test pulse (bring-up) ----
 * The coil current (and HV) every time U4 converted it during the last collect or return's
 * FIRE and DRAIN: about every 1.7 ms, so the card can show when the relay's own contact cuts
 * the current. coinboard_test_pulse() runs a test with its own pulse length (not saved). */
#define COINBOARD_FT_MAX 200
typedef struct { uint16_t t_100us; int16_t i_ma; int16_t hv_dv; } coinboard_ft_pt_t;   /* since FIRE began; 0.1 V */
int coinboard_fire_trace(coinboard_ft_pt_t *out, int max, int *pulse_ms, int *what);
esp_err_t coinboard_test_pulse(cf_what_t what, uint16_t pulse_ms);

/* ---- Coin switch capture (bring-up) ----
 * Polls U1's input register as fast as the bus allows (400 kHz, or 100 kHz if that
 * errors) and records every change of the four coin inputs with a microsecond
 * timestamp, plus markers the web UI places between coins. Normal polling and the
 * relay are locked out while it runs (at most CAP_MAX_S), so nothing fires. The
 * result stays in RAM until the next capture. */
#define COIN_CAP_MAX_EV 512
#define COIN_CAP_MAX_S  240
typedef struct {
    uint32_t t_us;      /* since the capture started */
    uint8_t  bits;      /* U1's input register at that moment (P0-P3 low = closed) */
    uint8_t  mark;      /* 0: an input changed; 1-3: a coin window began (nickel, dime, quarter); 4: end */
} coin_cap_ev_t;
typedef struct {
    bool     running, overflow;
    uint32_t samples, errors, t_ms;
    uint32_t gap_min_us, gap_avg_us, gap_max_us;   /* time between reads */
    int      bus_khz;
    int      n_events;
} coin_cap_info_t;
/* ESP_ERR_INVALID_STATE: not answering, a relay sequence is running, or already capturing. */
esp_err_t coinboard_capture_start(void);
void      coinboard_capture_mark(int mark);   /* 1-3 */
void      coinboard_capture_stop(void);       /* waits for the task to finish */
void      coinboard_capture_info(coin_cap_info_t *out);
int       coinboard_capture_events(coin_cap_ev_t *out, int max);

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

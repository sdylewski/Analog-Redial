/*
 * coin_fire.h -- the coin relay's firing sequence (PayPhone/coin-board-
 * schematic.md section 7), as a state machine with no I/O: coinboard.c feeds
 * it the time and the latest ADC readings and writes the three outputs it
 * asks for. No ESP-IDF code, so firmware/test/host/test_coin_fire.c runs it.
 *
 *   POL      set K1 for collect or return, HV still off; wait for K1
 *   CHARGE   EN_HV high; wait until HV stops rising (U4), or a fixed 3 s
 *            without U4
 *   FIRE     FIRE high for the pulse; dropped early on an overcurrent
 *   DRAIN    FIRE and EN_HV low; POL held until the coil current is gone,
 *            so K1's contacts only ever switch dead
 *   COOL     everything low; a short rest before the next one
 *
 * One request can wait while another runs (a collect, then a return). Every
 * error ends in DRAIN, never with an output left high: HV that doesn't come
 * up, the 20 V rail missing, an overcurrent, current that won't stop after
 * FIRE drops (Q3 shorted), the bus failing (coin_fire_abort()).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CF_K1_SETTLE_MS     30      /* K1 (polarity) operates in about 10 ms [verify] */
#define CF_CHARGE_MIN_MS    300
#define CF_CHARGE_MAX_MS    4000    /* about 1 s to 119 V by the sizing; generous */
#define CF_CHARGE_NOADC_MS  3000    /* without U4: a fixed wait */
#define CF_STABLE_WIN_MS    250     /* HV rose less than CF_STABLE_MV over this: charged */
#define CF_STABLE_MV        1000
#define CF_HV_MIN_MV        15000   /* below this nothing is worth firing */
#define CF_RAIL_CHECK_MS    1000    /* the 20 V rail must be up by then */
#define CF_RAIL_MIN_MV      15000
#define CF_PULSE_MIN_MS     20
#define CF_PULSE_MAX_MS     1000    /* the doc's cap on FIRE */
#define CF_PULSE_DEFAULT_MS 100
/* Overcurrent: the relay's own contact can short the coil, and Q3 then
 * carries about 1 A at the full HV. AIN2 reads up to about 330 mA (U4's
 * input stops at VIO), so this is "pinned at full scale". Only above
 * CF_OC_HV_MV: a 12-24 V coil (J14 off) draws 0.3 A by design. */
#define CF_OC_MA            300
#define CF_OC_HV_MV         30000
#define CF_OC_MS            20
#define CF_I_OFF_MA         20      /* below this the coil current is gone */
#define CF_FIRED_MA         20      /* a peak above this: current flowed */
#define CF_DRAIN_MIN_MS     50
#define CF_DRAIN_MAX_MS     500
#define CF_COOL_MS          1500
#define CF_HVTEST_MS        10000   /* "HV test": charge and hold, no FIRE */

typedef enum {
    CF_IDLE = 0,
    CF_POL,
    CF_CHARGE,
    CF_FIRE,
    CF_DRAIN,
    CF_COOL,
} cf_state_t;

typedef enum {
    CF_COLLECT = 0,
    CF_RETURN,
    CF_HV_TEST,     /* charge to the set voltage and hold it, to measure; no FIRE */
} cf_what_t;

typedef enum {
    CF_RES_NONE = 0,
    CF_RES_FIRED,       /* current flowed (U4 saw it) */
    CF_RES_NO_CURRENT,  /* U4 saw no current: relay not connected, or open */
    CF_RES_UNMEASURED,  /* fired without U4: nothing known */
    CF_RES_HV_DONE,     /* HV test finished */
    CF_RES_FAULT,       /* stopped early; msg says why */
} cf_result_t;

typedef struct {
    uint16_t pulse_ms;
    bool     swap;      /* collect is POL high (a relay wired the other way round) */
} cf_cfg_t;

typedef struct {
    bool adc;           /* U4 answering: the three values below are real */
    int  rail_mv;       /* the 20 V rail */
    int  hv_mv;
    int  i_ma;          /* coil current */
} cf_meas_t;

typedef struct {
    bool en_hv, pol, fire;
} cf_out_t;

typedef struct {
    cf_state_t  state;
    cf_what_t   what;
    int         pending;        /* a queued cf_what_t, or -1 */
    int64_t     t_state;        /* when the state began, ms */
    cf_out_t    out;
    bool        adc_at_start;   /* U4 answered when CHARGE began */
    /* CHARGE: the reading CF_STABLE_WIN_MS ago */
    int64_t     ref_t;
    int         ref_mv;
    /* FIRE and DRAIN */
    int64_t     oc_since;       /* -1: not over */
    int         hv_before_mv, hv_min_mv, peak_ma;
    bool        fault;
    /* The last one finished */
    cf_result_t result;
    cf_what_t   result_what;
    char        msg[96];
    uint32_t    fires;          /* FIRE pulses since boot */
    uint32_t    results;        /* bumped each time result and msg change */
} coin_fire_t;

void coin_fire_init(coin_fire_t *f);

/* Starts it now if idle, else queues it (one slot: a second request while
 * one waits replaces it). False only for a bad `what`. */
bool coin_fire_request(coin_fire_t *f, cf_what_t what);

/* Call every poll (a few ms). Updates f->out. */
void coin_fire_step(coin_fire_t *f, const cf_cfg_t *cfg, const cf_meas_t *m, int64_t now_ms);

/* The bus failed or the chip reset: stop now (to DRAIN), dropping any queued
 * request, and record why. */
void coin_fire_abort(coin_fire_t *f, const char *why, int64_t now_ms);

bool coin_fire_busy(const coin_fire_t *f);
const char *coin_fire_state_name(cf_state_t s);
const char *coin_fire_what_name(cf_what_t w);

#ifdef __cplusplus
}
#endif

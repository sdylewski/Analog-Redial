/* coin_fire.c -- see coin_fire.h. */
#include "coin_fire.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void coin_fire_init(coin_fire_t *f)
{
    memset(f, 0, sizeof(*f));
    f->pending = -1;
    f->oc_since = -1;
}

const char *coin_fire_state_name(cf_state_t s)
{
    switch (s) {
        case CF_IDLE:   return "idle";
        case CF_POL:    return "setting polarity";
        case CF_CHARGE: return "charging";
        case CF_FIRE:   return "firing";
        case CF_DRAIN:  return "stopping";
        case CF_COOL:   return "resting";
        default:        return "?";
    }
}

const char *coin_fire_what_name(cf_what_t w)
{
    switch (w) {
        case CF_COLLECT: return "collect";
        case CF_RETURN:  return "return";
        case CF_HV_TEST: return "HV test";
        default:         return "?";
    }
}

bool coin_fire_busy(const coin_fire_t *f) { return f->state != CF_IDLE; }

static void go(coin_fire_t *f, cf_state_t s, int64_t now)
{
    f->state = s;
    f->t_state = now;
}

static void start(coin_fire_t *f, cf_what_t what, int64_t now)
{
    f->what = what;
    f->fault = false;
    f->peak_ma = 0;
    f->hv_before_mv = f->hv_min_mv = 0;
    f->oc_since = -1;
    go(f, CF_POL, now);
}

bool coin_fire_request(coin_fire_t *f, cf_what_t what)
{
    if (what != CF_COLLECT && what != CF_RETURN && what != CF_HV_TEST) return false;
    /* Idle or not, the next step that can start it does: it has the time. */
    f->pending = what;
    return true;
}

static void finish(coin_fire_t *f, cf_result_t r, int64_t now, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

static void finish(coin_fire_t *f, cf_result_t r, int64_t now, const char *fmt, ...)
{
    f->result = r;
    f->result_what = f->what;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(f->msg, sizeof(f->msg), fmt, ap);
    va_end(ap);
    f->results++;
    f->fault = (r == CF_RES_FAULT);
    /* FIRE and EN_HV drop now; POL waits in DRAIN for the current to stop. */
    go(f, CF_DRAIN, now);
}

void coin_fire_abort(coin_fire_t *f, const char *why, int64_t now)
{
    f->pending = -1;
    if (f->state == CF_IDLE || f->state == CF_COOL || f->state == CF_DRAIN) {
        if (f->state == CF_DRAIN) {
            f->result = CF_RES_FAULT;
            f->fault = true;
            snprintf(f->msg, sizeof(f->msg), "%s: %s", coin_fire_what_name(f->what), why);
            f->results++;
        }
        return;
    }
    finish(f, CF_RES_FAULT, now, "%s stopped: %s", coin_fire_what_name(f->what), why);
}

static int v10(int mv) { return mv / 1000; }   /* whole volts, for messages */

void coin_fire_step(coin_fire_t *f, const cf_cfg_t *cfg, const cf_meas_t *m, int64_t now)
{
    int pulse = cfg->pulse_ms;
    if (pulse < CF_PULSE_MIN_MS) pulse = CF_PULSE_MIN_MS;
    if (pulse > CF_PULSE_MAX_MS) pulse = CF_PULSE_MAX_MS;
    int64_t in_state = now - f->t_state;

    switch (f->state) {
    case CF_IDLE:
        if (f->pending >= 0) {
            cf_what_t w = (cf_what_t)f->pending;
            f->pending = -1;
            start(f, w, now);
            in_state = 0;
        } else {
            break;
        }
        /* fall through: POL goes out on this same step */
    case CF_POL:
        if (in_state >= CF_K1_SETTLE_MS) {
            go(f, CF_CHARGE, now);
            f->adc_at_start = m->adc;
            f->ref_t = now;
            f->ref_mv = m->adc ? m->hv_mv : 0;
        }
        break;

    case CF_CHARGE:
        if (!m->adc) {
            /* No U4 (or it stopped answering): the fixed wait. */
            if (in_state >= CF_CHARGE_NOADC_MS) {
                if (f->what == CF_HV_TEST) {
                    if (in_state >= CF_HVTEST_MS)
                        finish(f, CF_RES_HV_DONE, now, "HV test: held %d s (no ADC to read it)",
                               CF_HVTEST_MS / 1000);
                    break;
                }
                f->hv_before_mv = f->hv_min_mv = 0;
                go(f, CF_FIRE, now);
                f->fires++;
            }
            break;
        }
        if (in_state >= CF_RAIL_CHECK_MS && m->rail_mv < CF_RAIL_MIN_MV) {
            finish(f, CF_RES_FAULT, now, "%s: the 20 V rail is at %d V (stage 1 not running?)",
                   coin_fire_what_name(f->what), v10(m->rail_mv));
            break;
        }
        if (f->what == CF_HV_TEST) {
            if (in_state >= CF_HVTEST_MS)
                finish(f, CF_RES_HV_DONE, now, "HV test: %d V (20 V rail %d V)",
                       v10(m->hv_mv), v10(m->rail_mv));
            break;
        }
        {
            bool stable = false;
            if (now - f->ref_t >= CF_STABLE_WIN_MS) {
                stable = m->hv_mv - f->ref_mv < CF_STABLE_MV;
                f->ref_t = now;
                f->ref_mv = m->hv_mv;
            }
            bool ready = in_state >= CF_CHARGE_MIN_MS && stable && m->hv_mv >= CF_HV_MIN_MV;
            bool late = in_state >= CF_CHARGE_MAX_MS;
            if (late && m->hv_mv < CF_HV_MIN_MV) {
                finish(f, CF_RES_FAULT, now, "%s: HV only reached %d V",
                       coin_fire_what_name(f->what), v10(m->hv_mv));
                break;
            }
            if (ready || late) {
                f->hv_before_mv = f->hv_min_mv = m->hv_mv;
                f->peak_ma = 0;
                f->oc_since = -1;
                go(f, CF_FIRE, now);
                f->fires++;
            }
        }
        break;

    case CF_FIRE:
        if (m->adc) {
            if (m->i_ma > f->peak_ma) f->peak_ma = m->i_ma;
            if (m->hv_mv < f->hv_min_mv) f->hv_min_mv = m->hv_mv;
            if (m->i_ma >= CF_OC_MA && f->hv_before_mv >= CF_OC_HV_MV) {
                if (f->oc_since < 0) f->oc_since = now;
                if (now - f->oc_since >= CF_OC_MS) {
                    finish(f, CF_RES_FAULT, now,
                           "%s: overcurrent at %d V, FIRE dropped after %d ms (coil shorted?)",
                           coin_fire_what_name(f->what), v10(f->hv_before_mv), (int)in_state);
                    break;
                }
            } else {
                f->oc_since = -1;
            }
        }
        if (in_state >= pulse) {
            if (!m->adc && !f->adc_at_start)
                finish(f, CF_RES_UNMEASURED, now, "%s: fired %d ms (no ADC: not measured)",
                       coin_fire_what_name(f->what), pulse);
            else if (f->peak_ma >= CF_FIRED_MA)
                finish(f, CF_RES_FIRED, now, "%s: fired at %d V, %d mA peak, HV fell %d V",
                       coin_fire_what_name(f->what), v10(f->hv_before_mv), f->peak_ma,
                       v10(f->hv_before_mv - f->hv_min_mv));
            else
                finish(f, CF_RES_NO_CURRENT, now,
                       "%s: fired at %d V but no current flowed (relay not connected?)",
                       coin_fire_what_name(f->what), v10(f->hv_before_mv));
        }
        break;

    case CF_DRAIN:
        if (in_state < CF_DRAIN_MIN_MS) break;
        if (m->adc && m->i_ma >= CF_I_OFF_MA) {
            /* POL held: switching K1 now would break the current on its contacts. */
            if (in_state >= CF_DRAIN_MAX_MS && !f->fault) {
                f->result = CF_RES_FAULT;
                f->fault = true;
                snprintf(f->msg, sizeof(f->msg),
                         "%s: %d mA still flowing with FIRE off (Q3 shorted?)",
                         coin_fire_what_name(f->what), m->i_ma);
                f->results++;
            }
            break;
        }
        go(f, CF_COOL, now);
        break;

    case CF_COOL:
        if (in_state >= CF_COOL_MS) go(f, CF_IDLE, now);
        break;
    }

    /* The outputs for the state now. */
    bool pol = (f->what == CF_RETURN) != cfg->swap;
    if (f->what == CF_HV_TEST) pol = false;
    switch (f->state) {
        case CF_POL:    f->out = (cf_out_t){ .pol = pol }; break;
        case CF_CHARGE: f->out = (cf_out_t){ .pol = pol, .en_hv = true }; break;
        case CF_FIRE:   f->out = (cf_out_t){ .pol = pol, .en_hv = true, .fire = true }; break;
        case CF_DRAIN:  f->out = (cf_out_t){ .pol = pol }; break;
        default:        f->out = (cf_out_t){ 0 }; break;
    }
}

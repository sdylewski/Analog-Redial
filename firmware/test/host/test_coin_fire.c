/* Host test of the coin relay's firing sequence (coin_fire.c) against a model
 * of the coin board's power stages and a coin relay. Checks the rules of
 * coin-board.md Part 1, section 7 at every millisecond, on top of each
 * scenario's own result. */
#include "coin_fire.h"
#include <stdio.h>
#include <stdlib.h>
/* Not assert(): on the machine these were written on, a failed assert hangs
 * in abort() instead of exiting. */
#define assert(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)
#include <string.h>

/* ---- the board, 1 ms steps ---- */
typedef struct {
    /* setup */
    int  set_mv;        /* J7-J9 setting; 19000 = J14 off */
    int  coil_ohm;      /* 0 = no relay connected */
    bool rail_dead;     /* stage 1 broken */
    bool hv_dead;       /* stage 2 broken: HV stays at the rail */
    bool adc;
    bool q3_short;      /* current flows whatever FIRE is */
    bool coil_short_at_fire;  /* the relay's own contact shorts the coil */
    bool hv_stuck_on;   /* the supply runs whatever EN_HV says (Q1 shorted) */
    /* state */
    double rail, hv, i;
} plant_t;

static void plant_step(plant_t *p, const cf_out_t *o)
{
    /* Stage 1: 20 V in tens of ms while EN_HV, else idles at 4.6 V. */
    bool en = o->en_hv || p->hv_stuck_on;
    double rail_t = (en && !p->rail_dead) ? 20000 : 4600;
    p->rail += (rail_t - p->rail) * 0.05;
    /* Stage 2 runs from the rail above 8.4 V; about 1 s to 119 V. */
    double floor_mv = p->rail - 700;
    if (en && p->rail > 8400 && !p->hv_dead && p->hv < p->set_mv) p->hv += 150;  /* 150 V/s */
    if (p->hv > p->set_mv && en) p->hv = p->set_mv;
    if (p->hv < floor_mv) p->hv = floor_mv;
    p->hv -= p->hv / 50000.0;   /* the bleed: 50 s */
    /* The coil and Q3, with R26. */
    bool conducting = (o->fire || p->q3_short) && p->coil_ohm;
    double target = 0;
    if (conducting) {
        double r = (p->coil_short_at_fire ? 0.1 : p->coil_ohm) + 10;
        target = p->hv / r;               /* mA, from mV / ohm */
        if (target > 1000) target = 1000; /* Q3's limit */
    }
    p->i += (target - p->i) * 0.4;        /* coil inductance, ms-scale */
    if (p->i < 0.5 && !conducting) p->i = 0;
    /* C1 (220 uF) gives the coil its charge. */
    p->hv -= p->i * 1e-3 / 220e-6;        /* mA * 1 ms / 220 uF, in mV */
    if (p->hv < 0) p->hv = 0;
}

static cf_meas_t meas(const plant_t *p)
{
    cf_meas_t m = { .adc = p->adc, .rail_mv = (int)p->rail, .hv_mv = (int)p->hv, .i_ma = (int)p->i };
    /* AIN2 stops at VIO: 3.3 V on 10 ohm. */
    if (m.i_ma > 330) m.i_ma = 330;
    return m;
}

/* ---- the rules, checked every step ---- */
typedef struct {
    int  fire_ms, max_fire_ms, fire_pulses, en_ms;
    bool last_fire, last_pol;
    bool pol_switched_live;
    bool fire_without_en;
} watch_t;

static void check(watch_t *w, const cf_out_t *o, const plant_t *p)
{
    if (o->fire && !o->en_hv) w->fire_without_en = true;
    if (o->pol != w->last_pol && (o->fire || w->last_fire || p->i >= CF_I_OFF_MA))
        w->pol_switched_live = true;
    if (o->fire) {
        if (!w->last_fire) w->fire_pulses++;
        w->fire_ms++;
        if (w->fire_ms > w->max_fire_ms) w->max_fire_ms = w->fire_ms;
    } else {
        w->fire_ms = 0;
    }
    if (o->en_hv) w->en_ms++;
    w->last_fire = o->fire;
    w->last_pol = o->pol;
}

static int64_t now;
static void run(coin_fire_t *f, const cf_cfg_t *c, plant_t *p, watch_t *w, int ms)
{
    for (int k = 0; k < ms; k++) {
        cf_meas_t m = meas(p);
        coin_fire_step(f, c, &m, now);
        check(w, &f->out, p);
        plant_step(p, &f->out);
        now++;
    }
}

static void rules_ok(const watch_t *w)
{
    assert(!w->fire_without_en);
    assert(!w->pol_switched_live);
    assert(w->max_fire_ms <= CF_PULSE_MAX_MS);
}

static plant_t board(int set_mv, int coil)
{
    return (plant_t){ .set_mv = set_mv, .coil_ohm = coil, .adc = true, .rail = 4600, .hv = 3900 };
}

int main(void)
{
    cf_cfg_t cfg = { .pulse_ms = 100, .swap = false };
    coin_fire_t f;

    /* Collect at 60 V on a 600 ohm coil: POL low (white +), fires once. */
    {
        plant_t p = board(60000, 600);
        watch_t w = { 0 };
        coin_fire_init(&f);
        assert(coin_fire_request(&f, CF_COLLECT));
        run(&f, &cfg, &p, &w, 1);
        assert(f.state == CF_POL && !f.out.pol && !f.out.en_hv);
        run(&f, &cfg, &p, &w, 5000);
        rules_ok(&w);
        assert(f.state == CF_IDLE && w.fire_pulses == 1);
        assert(f.result == CF_RES_FIRED && f.result_what == CF_COLLECT);
        assert(w.max_fire_ms == 100);
        assert(f.hv_before_mv > 55000 && f.hv_before_mv <= 60000);
        assert(f.peak_ma > 80 && f.peak_ma < 110);
        assert(f.fires == 1 && f.results == 1);
        printf("  %s\n", f.msg);
    }

    /* Return: POL high for the whole sequence, low again after. */
    {
        plant_t p = board(60000, 600);
        watch_t w = { 0 };
        coin_fire_init(&f);
        coin_fire_request(&f, CF_RETURN);
        run(&f, &cfg, &p, &w, 1);
        assert(f.out.pol);
        bool pol_while_firing = true;
        for (int k = 0; k < 5000; k++) {
            run(&f, &cfg, &p, &w, 1);
            if (f.out.fire && !f.out.pol) pol_while_firing = false;
        }
        rules_ok(&w);
        assert(pol_while_firing && !f.out.pol && f.result == CF_RES_FIRED);
    }

    /* Swapped wiring: collect is POL high. */
    {
        plant_t p = board(60000, 600);
        watch_t w = { 0 };
        cf_cfg_t sw = { .pulse_ms = 100, .swap = true };
        coin_fire_init(&f);
        coin_fire_request(&f, CF_COLLECT);
        run(&f, &sw, &p, &w, 1);
        assert(f.out.pol);
        run(&f, &sw, &p, &w, 5000);
        rules_ok(&w);
    }

    /* 119 V: still charges in time, and isn't an overcurrent (600 ohm, 190 mA). */
    {
        plant_t p = board(119000, 600);
        watch_t w = { 0 };
        coin_fire_init(&f);
        coin_fire_request(&f, CF_COLLECT);
        run(&f, &cfg, &p, &w, 6000);
        rules_ok(&w);
        assert(f.result == CF_RES_FIRED && f.hv_before_mv > 110000);
    }

    /* J14 off: HV is the rail, about 19 V; a 50 ohm, 24 V coil draws about
     * 0.3 A, full scale on AIN2, and that's not an overcurrent there. */
    {
        plant_t p = board(19000, 50);
        watch_t w = { 0 };
        coin_fire_init(&f);
        coin_fire_request(&f, CF_COLLECT);
        run(&f, &cfg, &p, &w, 5000);
        rules_ok(&w);
        assert(f.result == CF_RES_FIRED && w.max_fire_ms == 100);
        assert(f.hv_before_mv > 15000 && f.hv_before_mv < 20000);
    }

    /* No relay connected: fires, says no current flowed. */
    {
        plant_t p = board(60000, 0);
        watch_t w = { 0 };
        coin_fire_init(&f);
        coin_fire_request(&f, CF_RETURN);
        run(&f, &cfg, &p, &w, 5000);
        rules_ok(&w);
        assert(f.result == CF_RES_NO_CURRENT);
        printf("  %s\n", f.msg);
    }

    /* No U4: a fixed 3 s, then the pulse, result unmeasured. */
    {
        plant_t p = board(60000, 600);
        p.adc = false;
        watch_t w = { 0 };
        coin_fire_init(&f);
        coin_fire_request(&f, CF_COLLECT);
        run(&f, &cfg, &p, &w, CF_K1_SETTLE_MS + CF_CHARGE_NOADC_MS - 5);
        assert(w.fire_pulses == 0 && f.out.en_hv);
        run(&f, &cfg, &p, &w, 3000);
        rules_ok(&w);
        assert(w.fire_pulses == 1 && f.result == CF_RES_UNMEASURED && f.state == CF_IDLE);
    }

    /* Stage 2 broken: HV sits at the rail (19 V) with J7 set for 87 V. That
     * looks like J14 off, so it fires at 19 V: not a fault, but the message
     * says what it got. */
    {
        plant_t p = board(87000, 600);
        p.hv_dead = true;
        watch_t w = { 0 };
        coin_fire_init(&f);
        coin_fire_request(&f, CF_COLLECT);
        run(&f, &cfg, &p, &w, 6000);
        rules_ok(&w);
        assert(f.hv_before_mv < 20000);
    }

    /* Stage 1 broken: the rail never comes up. A fault at 1 s, never a FIRE,
     * EN_HV off after. */
    {
        plant_t p = board(60000, 600);
        p.rail_dead = true;
        watch_t w = { 0 };
        coin_fire_init(&f);
        coin_fire_request(&f, CF_COLLECT);
        run(&f, &cfg, &p, &w, 3000);
        rules_ok(&w);
        assert(w.fire_pulses == 0 && f.result == CF_RES_FAULT && !f.out.en_hv);
        assert(w.en_ms <= CF_RAIL_CHECK_MS + 5);
        printf("  %s\n", f.msg);
    }

    /* The relay's contact shorts the coil at 60 V: FIRE drops after about
     * CF_OC_MS, well before the pulse ends. */
    {
        plant_t p = board(60000, 600);
        p.coil_short_at_fire = true;
        watch_t w = { 0 };
        cf_cfg_t longp = { .pulse_ms = 500 };
        coin_fire_init(&f);
        coin_fire_request(&f, CF_COLLECT);
        run(&f, &longp, &p, &w, 5000);
        rules_ok(&w);
        assert(f.result == CF_RES_FAULT && w.fire_pulses == 1);
        assert(w.max_fire_ms >= CF_OC_MS && w.max_fire_ms <= CF_OC_MS + 15);
        printf("  %s\n", f.msg);
    }

    /* Q3 shorted alone: C1 empties through the coil in a fraction of a
     * second with EN_HV off, and the sequence ends normally. */
    {
        plant_t p = board(60000, 600);
        watch_t w = { 0 };
        coin_fire_init(&f);
        coin_fire_request(&f, CF_RETURN);
        for (int k = 0; k < 3000 && f.state != CF_FIRE; k++) run(&f, &cfg, &p, &w, 1);
        p.q3_short = true;
        run(&f, &cfg, &p, &w, 4000);
        rules_ok(&w);
        assert(f.state == CF_IDLE);
    }

    /* Q3 shorted and the supply stuck on: current keeps flowing with FIRE
     * off. POL stays put (K1 never breaks the current), with a fault, until
     * it stops. */
    {
        plant_t p = board(60000, 600);
        watch_t w = { 0 };
        coin_fire_init(&f);
        coin_fire_request(&f, CF_RETURN);
        for (int k = 0; k < 3000 && f.state != CF_FIRE; k++) run(&f, &cfg, &p, &w, 1);
        assert(f.state == CF_FIRE);
        p.q3_short = true;
        p.hv_stuck_on = true;
        run(&f, &cfg, &p, &w, 100 + CF_DRAIN_MAX_MS + 10);
        assert(f.state == CF_DRAIN && f.out.pol && !f.out.en_hv && !f.out.fire);
        assert(f.result == CF_RES_FAULT);
        printf("  %s\n", f.msg);
        /* It clears: the sequence finishes. */
        p.q3_short = false;
        p.hv_stuck_on = false;
        run(&f, &cfg, &p, &w, 3000);
        rules_ok(&w);
        assert(f.state == CF_IDLE && !f.out.pol);
    }

    /* Aborted mid-charge (the bus failed): outputs low, no FIRE, queue dropped. */
    {
        plant_t p = board(60000, 600);
        watch_t w = { 0 };
        coin_fire_init(&f);
        coin_fire_request(&f, CF_COLLECT);
        run(&f, &cfg, &p, &w, 200);
        coin_fire_request(&f, CF_RETURN);
        coin_fire_abort(&f, "test", now);
        run(&f, &cfg, &p, &w, 1);
        assert(!f.out.en_hv && !f.out.fire);
        run(&f, &cfg, &p, &w, 5000);
        rules_ok(&w);
        assert(w.fire_pulses == 0 && f.result == CF_RES_FAULT && f.state == CF_IDLE);
    }

    /* Collect, then a return asked for while it runs: both, in order, one pulse each. */
    {
        plant_t p = board(60000, 600);
        watch_t w = { 0 };
        coin_fire_init(&f);
        coin_fire_request(&f, CF_COLLECT);
        run(&f, &cfg, &p, &w, 500);
        coin_fire_request(&f, CF_RETURN);
        run(&f, &cfg, &p, &w, 1000);
        assert(f.result_what == CF_COLLECT && f.result == CF_RES_FIRED && w.fire_pulses == 1);
        run(&f, &cfg, &p, &w, 6000);
        rules_ok(&w);
        assert(w.fire_pulses == 2 && f.result_what == CF_RETURN && f.state == CF_IDLE);
    }

    /* The pulse can't be set past CF_PULSE_MAX_MS. */
    {
        plant_t p = board(60000, 600);
        watch_t w = { 0 };
        cf_cfg_t huge = { .pulse_ms = 5000 };
        coin_fire_init(&f);
        coin_fire_request(&f, CF_COLLECT);
        run(&f, &huge, &p, &w, 8000);
        rules_ok(&w);
        assert(w.max_fire_ms == CF_PULSE_MAX_MS);
    }

    /* HV test: EN_HV for CF_HVTEST_MS, no FIRE, POL low, reports the voltage. */
    {
        plant_t p = board(94000, 600);
        watch_t w = { 0 };
        coin_fire_init(&f);
        coin_fire_request(&f, CF_HV_TEST);
        run(&f, &cfg, &p, &w, CF_HVTEST_MS + 3000);
        rules_ok(&w);
        assert(w.fire_pulses == 0 && f.result == CF_RES_HV_DONE && f.state == CF_IDLE);
        assert(w.en_ms >= CF_HVTEST_MS && w.en_ms <= CF_HVTEST_MS + CF_K1_SETTLE_MS + 5);
        printf("  %s\n", f.msg);
    }

    /* A bad request is refused. */
    coin_fire_init(&f);
    assert(!coin_fire_request(&f, (cf_what_t)7));

    printf("coin fire: all checks passed\n");
    return 0;
}

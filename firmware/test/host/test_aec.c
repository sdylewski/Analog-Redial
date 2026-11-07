/* Host test for the echo canceller (components/aec): a simulated far-end
 * talker is played into a simulated echo path (bulk delay plus a short
 * decaying response, at a chosen loss), and the canceller has to remove it
 * without eating a near-end talker who cuts in. All at 8 kHz, 10 ms blocks,
 * as audio.c drives it. */
#include "aec.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FS      8000
#define BLOCK   80
#define HLEN    96          /* echo response length, 12 ms */

static int s_fail;
#define CHECK(c, ...) do { if (!(c)) { s_fail++; printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint32_t s_rng = 12345;
static double urand(void) { s_rng = s_rng * 1664525u + 1013904223u; return ((s_rng >> 8) & 0xFFFF) / 65535.0 - 0.5; }

/* Speech-like: low-passed noise with a ~4 Hz syllable envelope, `amp` peak-ish. */
typedef struct { double lp, ph; double rate; } talker_t;
static double talk(talker_t *t, double amp)
{
    t->lp = 0.7 * t->lp + 0.3 * urand();
    t->ph += 2 * M_PI * t->rate / FS;
    double env = 0.55 + 0.45 * sin(t->ph);
    return amp * 3.0 * t->lp * env;
}

typedef struct {
    const char *name;
    double loss_db;      /* echo path loss: 6 = a badly matched phone, 20 = a good one */
    int onset_ms;        /* where the echo starts, from the played sample */
    int aec_delay_ms;    /* the canceller's anchor */
    int dt_from_ms, dt_to_ms;   /* near-end talker, 0/0 for none */
    int far_off_from_ms, far_off_to_ms; /* far end silent in this span */
} scen_t;

typedef struct { double erle_end, near_kept_db, dt_erle_after; double worst_gain_db; } res_t;

static res_t run(const scen_t *sc, int total_ms)
{
    double h[HLEN];
    double hp = 0;
    for (int k = 0; k < HLEN; k++) { h[k] = exp(-k / 18.0) * cos(2 * M_PI * 0.13 * k); hp += h[k] * h[k]; }
    double g = pow(10, -sc->loss_db / 20) / sqrt(hp);
    for (int k = 0; k < HLEN; k++) h[k] *= g;

    aec_init();
    aec_set_delay_ms(0, sc->aec_delay_ms);
    aec_set_enabled(0, true);

    int onset = sc->onset_ms * FS / 1000;
    int n = total_ms * FS / 1000;
    double *ref = calloc(n, sizeof(double));
    talker_t far = { 0, 0, 3.7 }, nearT = { 0, 1.0, 4.3 };
    for (int i = 0; i < n; i++) {
        int ms = i * 1000 / FS;
        bool off = ms >= sc->far_off_from_ms && ms < sc->far_off_to_ms;
        ref[i] = off ? 0 : talk(&far, 9000);
    }

    res_t r = { 0, 0, 0, -99 };
    double e_in_end = 0, e_out_end = 0;          /* last 500 ms, echo only */
    double e_near_dt = 0, e_out_dt = 0;          /* during double-talk */
    double e_in_after = 0, e_out_after = 0;      /* 300-800 ms after double-talk */
    int16_t rb[BLOCK], nb[BLOCK], ob[BLOCK];
    double nearsig[BLOCK];
    for (int b = 0; b + BLOCK <= n; b += BLOCK) {
        int ms = b * 1000 / FS;
        bool dt = ms >= sc->dt_from_ms && ms < sc->dt_to_ms;
        for (int k = 0; k < BLOCK; k++) {
            int i = b + k;
            double echo = 0;
            for (int j = 0; j < HLEN; j++) {
                int s = i - onset - j;
                if (s >= 0) echo += h[j] * ref[s];
            }
            nearsig[k] = dt ? talk(&nearT, 5000) : 0;
            double v = echo + nearsig[k] + 20 * urand();
            if (v > 32767) v = 32767; if (v < -32768) v = -32768;
            rb[k] = (int16_t)lrint(ref[i]);
            nb[k] = (int16_t)lrint(v);
        }
        aec_process(0, rb, nb, ob, BLOCK);
        double bi = 0, bo = 0, bn = 0, bres = 0;
        for (int k = 0; k < BLOCK; k++) {
            bi += (double)nb[k] * nb[k];
            bo += (double)ob[k] * ob[k];
            bn += nearsig[k] * nearsig[k];
            (void)bres;
        }
        if (bi > BLOCK * 400.0) {
            double gdb = 10 * log10((bo + 1) / bi);
            if (gdb > r.worst_gain_db) r.worst_gain_db = gdb;
        }
        if (ms >= total_ms - 500) { e_in_end += bi; e_out_end += bo; }
        if (dt) { e_near_dt += bn; e_out_dt += bo; }
        if (sc->dt_to_ms && ms >= sc->dt_to_ms + 300 && ms < sc->dt_to_ms + 800) { e_in_after += bi; e_out_after += bo; }
    }
    r.erle_end = 10 * log10((e_in_end + 1) / (e_out_end + 1));
    if (e_near_dt > 0) r.near_kept_db = 10 * log10((e_out_dt + 1) / e_near_dt);
    if (e_in_after > 0) r.dt_erle_after = 10 * log10((e_in_after + 1) / (e_out_after + 1));
    aec_set_enabled(0, false);
    free(ref);
    return r;
}

int main(void)
{
    printf("test_aec\n");

    /* Echo that's 20 dB down, anchor well placed: cancel it. */
    scen_t good = { "20 dB path", 20, 16, 12, 0, 0, 0, 0 };
    res_t r = run(&good, 4000);
    printf("  %-34s ERLE %.1f dB\n", good.name, r.erle_end);
    CHECK(r.erle_end >= 20, "20 dB path: ERLE %.1f < 20", r.erle_end);

    /* A badly matched phone: the echo is only 6 dB down. The old fixed
     * double-talk test called that near-end speech and never let it learn. */
    scen_t loud = { "6 dB path (badly matched phone)", 6, 16, 12, 0, 0, 0, 0 };
    r = run(&loud, 4000);
    printf("  %-34s ERLE %.1f dB\n", loud.name, r.erle_end);
    CHECK(r.erle_end >= 20, "6 dB path: ERLE %.1f < 20", r.erle_end);

    /* Near-end talker cuts in for 1.5 s while the far end keeps talking:
     * the near voice must come through, and the filter must still cancel after. */
    scen_t dts[] = {
        { "double-talk, 20 dB path", 20, 16, 12, 2500, 4000, 0, 0 },
        { "double-talk, 6 dB path", 6, 16, 12, 2500, 4000, 0, 0 },
    };
    for (int i = 0; i < 2; i++) {
        r = run(&dts[i], 6000);
        printf("  %-34s near voice %+.1f dB, ERLE after %.1f dB, end %.1f dB\n",
               dts[i].name, r.near_kept_db, r.dt_erle_after, r.erle_end);
        CHECK(r.near_kept_db > -3 && r.near_kept_db < 3, "%s: near voice changed %.1f dB", dts[i].name, r.near_kept_db);
        CHECK(r.dt_erle_after >= 15, "%s: ERLE after double-talk %.1f < 15", dts[i].name, r.dt_erle_after);
    }

    /* Near-end talking with the far end silent: passed through untouched. */
    scen_t solo = { "near end alone", 20, 16, 12, 2500, 4000, 2000, 4500 };
    r = run(&solo, 5000);
    printf("  %-34s near voice %+.1f dB\n", solo.name, r.near_kept_db);
    CHECK(r.near_kept_db > -1 && r.near_kept_db < 1, "near end alone: changed %.1f dB", r.near_kept_db);

    /* Anchored after the echo starts (what applying the probe's PEAK time
     * did): it can't cancel well, but it must never make the line louder. */
    scen_t late = { "anchor past the echo onset", 10, 12, 21, 0, 0, 0, 0 };
    r = run(&late, 4000);
    printf("  %-34s ERLE %.1f dB, worst block %+.1f dB\n", late.name, r.erle_end, r.worst_gain_db);
    CHECK(r.worst_gain_db <= 1.5, "late anchor: a block came out %.1f dB louder", r.worst_gain_db);

    aec_status_t st;
    aec_init(); aec_set_enabled(0, true);
    { int16_t z[BLOCK]; for (int k = 0; k < BLOCK; k++) z[k] = (int16_t)(k * 97 % 2000 - 1000);
      for (int i = 0; i < 300; i++) aec_process(0, z, z, z, BLOCK); }
    aec_get_status(0, &st);
    printf("  cost on this machine: %u us per 10 ms block (max %u)\n", (unsigned)st.avg_us, (unsigned)st.max_us);
    aec_set_enabled(0, false);

    if (s_fail) { printf("test_aec: %d failure(s)\n", s_fail); return 1; }
    printf("test_aec: all passed\n");
    return 0;
}

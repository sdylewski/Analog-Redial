/* Host test of the coin board's input counting (coin_inputs.c): bouncing
 * switches sampled the way the driver polls them. */
#include "coin_inputs.h"
#include <stdio.h>
#include <stdlib.h>
/* Not assert(): on the machine these were written on, a failed assert hangs
 * in abort() instead of exiting. */
#define assert(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)
#include <string.h>

#define POLL 3   /* COINBOARD_POLL_MS */

/* The switch as a function of time: closed between `on` and `off`, with
 * `bounce` ms of chatter (1 ms on, 1 ms off) at each end. */
typedef struct { int on, off, bounce; } pulse_t;

static int closed_at(const pulse_t *p, int n, int t)
{
    for (int i = 0; i < n; i++) {
        if (t < p[i].on || t >= p[i].off + p[i].bounce) continue;
        if (t < p[i].on + p[i].bounce) return ((t - p[i].on) & 1) == 0;
        if (t >= p[i].off) return ((t - p[i].off) & 1) == 1;
        return 1;
    }
    return 0;
}

/* Runs input 0 through `ms`, polling every `poll` ms starting at `phase`;
 * returns the coins counted. */
static int run(const pulse_t *p, int n, int ms, int poll, int phase, int *stuck_events)
{
    coin_in_t c;
    coin_in_init(&c);
    int coins = 0, st = 0;
    for (int t = phase; t < ms; t += poll) {
        uint8_t s = 0;
        uint8_t got = coin_in_push(&c, (uint8_t)closed_at(p, n, t), t, &s);
        coins += got & 1;
        st += s & 1;
    }
    if (stuck_events) *stuck_events = st;
    return coins;
}

int main(void)
{
    /* Starting open for a moment is what a real power-up looks like. */
    pulse_t clean[] = { { 100, 140, 0 } };
    assert(run(clean, 1, 400, POLL, 0, NULL) == 1);

    /* Bounce of up to 5 ms at both ends, at every poll phase: still one. */
    pulse_t bouncy[] = { { 100, 130, 5 } };
    for (int ph = 0; ph < POLL; ph++) assert(run(bouncy, 1, 400, POLL, ph, NULL) == 1);

    /* Ten coins 150 ms apart, each 25 ms with bounce: ten. */
    pulse_t ten[10];
    for (int i = 0; i < 10; i++) ten[i] = (pulse_t){ 100 + 150 * i, 125 + 150 * i, 4 };
    for (int ph = 0; ph < POLL; ph++) assert(run(ten, 10, 1800, POLL, ph, NULL) == 10);

    /* Coins close together: 40 ms apart, 10 ms closures. The gap (30 ms open,
     * less bounce) is right at the rearm time, so the coins must be cleaner. */
    pulse_t fast[5];
    for (int i = 0; i < 5; i++) fast[i] = (pulse_t){ 100 + 45 * i, 110 + 45 * i, 0 };
    assert(run(fast, 5, 500, POLL, 0, NULL) == 5);

    /* One poll's worth of noise isn't a coin. */
    pulse_t spike[] = { { 100, 101, 0 } };
    for (int ph = 0; ph < POLL; ph++) assert(run(spike, 1, 300, POLL, ph, NULL) == 0);

    /* Already closed when counting starts (a jammed switch at power-up): not
     * a coin, until it opens and closes again. */
    pulse_t boot[] = { { 0, 50, 0 }, { 200, 230, 0 } };
    assert(run(boot, 2, 400, POLL, 0, NULL) == 1);

    /* Held closed: one coin, then one "stuck" report, nothing more. */
    int stuck = 0;
    pulse_t held[] = { { 100, 3500, 0 } };
    assert(run(held, 1, 4000, POLL, 0, &stuck) == 1 && stuck == 1);
    /* Released and used again: counts. */
    pulse_t held2[] = { { 100, 3500, 0 }, { 3700, 3730, 0 } };
    assert(run(held2, 2, 4000, POLL, 0, &stuck) == 2 && stuck == 1);

    /* Slower polling (the bus busy, 6 ms) still counts a 20 ms coin. */
    pulse_t slow[] = { { 100, 120, 2 } };
    for (int ph = 0; ph < 6; ph++) assert(run(slow, 1, 300, 6, ph, NULL) == 1);

    /* A gap in polling (failed reads) in the middle of a coin: one. */
    {
        coin_in_t c;
        coin_in_init(&c);
        int coins = 0;
        for (int t = 0; t < 100; t += POLL) coins += coin_in_push(&c, 0, t, NULL) & 1;
        coins += coin_in_push(&c, 1, 100, NULL) & 1;
        coins += coin_in_push(&c, 1, 103, NULL) & 1;
        coins += coin_in_push(&c, 1, 160, NULL) & 1;   /* 57 ms later */
        for (int t = 163; t < 300; t += POLL) coins += coin_in_push(&c, 0, t, NULL) & 1;
        assert(coins == 1);
    }

    /* All four inputs, at once and staggered: one coin each. */
    {
        coin_in_t c;
        coin_in_init(&c);
        int per[COIN_IN_N] = { 0 };
        for (int t = 0; t < 600; t += POLL) {
            uint8_t closed = 0;
            for (int b = 0; b < COIN_IN_N; b++) {
                int on = 100 + (b < 2 ? 0 : 50 * b);
                if (t >= on && t < on + 30) closed |= (uint8_t)(1u << b);
            }
            uint8_t got = coin_in_push(&c, closed, t, NULL);
            for (int b = 0; b < COIN_IN_N; b++) per[b] += (got >> b) & 1;
        }
        for (int b = 0; b < COIN_IN_N; b++) assert(per[b] == 1);
    }

    printf("coin inputs: all checks passed\n");
    return 0;
}

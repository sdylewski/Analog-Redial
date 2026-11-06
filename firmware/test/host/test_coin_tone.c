/* Host test for components/payphone/coin_tone.c: synthetic coin tones at
 * 48 kHz, as the line's audio tap delivers them. */
#include "coin_tone.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define RATE 48000
static int fails;

typedef struct { double f[4]; int nf; double amp; } mix_t;

static unsigned s_seed = 1;
static double noise(void) { s_seed = s_seed * 1103515245u + 12345u; return ((s_seed >> 8) & 0xffff) / 32768.0 - 1.0; }

/* Appends ms of the given frequencies (none = silence) plus a background. */
static size_t add(int16_t *buf, size_t at, int ms, const double *f, int nf, double amp,
                  const mix_t *bg, double noise_amp)
{
    size_t n = (size_t)RATE * ms / 1000;
    for (size_t i = 0; i < n; i++) {
        double t = (double)(at + i) / RATE, v = 0;
        for (int k = 0; k < nf; k++) v += amp * sin(2 * M_PI * f[k] * t);
        if (bg) for (int k = 0; k < bg->nf; k++) v += bg->amp * sin(2 * M_PI * bg->f[k] * t);
        v += noise_amp * noise();
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        buf[at + i] = (int16_t)v;
    }
    return at + n;
}

static const double COIN[2] = { 1700, 2200 };

/* A coin's beeps: count, on/off ms scaled by `stretch`. */
static size_t coin(int16_t *buf, size_t at, int beeps, int on_ms, int off_ms, double stretch,
                   double amp, const mix_t *bg, double na)
{
    for (int b = 0; b < beeps; b++) {
        at = add(buf, at, (int)(on_ms * stretch), COIN, 2, amp, bg, na);
        at = add(buf, at, (int)(off_ms * stretch), NULL, 0, 0, bg, na);
    }
    return at;
}

/* Runs `buf` through a detector in 10 ms blocks (the tap's size is similar)
 * and collects what it reports. */
static int run(const int16_t *buf, size_t n, int *out, int max)
{
    coin_tone_det_t d;
    coin_tone_init(&d, RATE);
    int got = 0;
    for (size_t i = 0; i < n; i += 480) {
        size_t len = n - i < 480 ? n - i : 480;
        int r = coin_tone_push(&d, buf + i, len);
        if (r && got < max) out[got++] = r;
    }
    return got;
}

static void expect(const char *what, const int16_t *buf, size_t n, const int *want, int nwant)
{
    int got[16];
    int ng = run(buf, n, got, 16);
    int ok = ng == nwant;
    for (int i = 0; ok && i < ng; i++) ok = got[i] == want[i];
    if (!ok) {
        fails++;
        printf("FAIL %s: got", what);
        for (int i = 0; i < ng; i++) printf(" %d", got[i]);
        printf(", want");
        for (int i = 0; i < nwant; i++) printf(" %d", want[i]);
        printf("\n");
    }
}

int main(void)
{
    static int16_t buf[RATE * 6];
    const struct { const char *name; int beeps, on, off, cents; } coins[] = {
        { "nickel", 1, 66, 66, 5 }, { "dime", 2, 66, 66, 10 }, { "quarter", 5, 33, 33, 25 },
    };
    const double amps[] = { 600, 3000, 12000 };
    const double stretch[] = { 0.8, 1.0, 1.2 };
    /* Dial tone as it comes back from the line: an echo, a little above the
     * faintest coin here. The phone's own coin tones are as loud as its DTMF. */
    mix_t dial = { { 350, 440 }, 2, 800 };

    for (int c = 0; c < 3; c++)
        for (int a = 0; a < 3; a++)
            for (int s = 0; s < 3; s++)
                for (int bg = 0; bg < 2; bg++) {
                    size_t at = add(buf, 0, 100, NULL, 0, 0, bg ? &dial : NULL, bg ? 200 : 0);
                    at = coin(buf, at, coins[c].beeps, coins[c].on, coins[c].off, stretch[s], amps[a],
                              bg ? &dial : NULL, bg ? 200 : 0);
                    at = add(buf, at, 300, NULL, 0, 0, bg ? &dial : NULL, bg ? 200 : 0);
                    char what[96];
                    snprintf(what, sizeof(what), "%s amp %.0f x%.1f%s", coins[c].name, amps[a],
                             stretch[s], bg ? " over dial tone" : "");
                    int want = coins[c].cents;
                    expect(what, buf, at, &want, 1);
                }

    /* Two coins in a row, a quarter then a dime. */
    {
        size_t at = coin(buf, 0, 5, 33, 33, 1.0, 4000, NULL, 0);
        at = add(buf, at, 400, NULL, 0, 0, NULL, 0);
        at = coin(buf, at, 2, 66, 66, 1.0, 4000, NULL, 0);
        at = add(buf, at, 300, NULL, 0, 0, NULL, 0);
        int want[2] = { 25, 10 };
        expect("quarter then dime", buf, at, want, 2);
    }

    /* Things that must not count. */
    {
        mix_t loud = { { 350, 440 }, 2, 8000 };
        size_t at = add(buf, 0, 3000, NULL, 0, 0, &loud, 300);
        expect("dial tone alone", buf, at, NULL, 0);
    }
    {
        const double rows[4] = { 697, 770, 852, 941 }, cols[4] = { 1209, 1336, 1477, 1633 };
        size_t at = 0;
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++) {
                double f[2] = { rows[r], cols[c] };
                at = add(buf, at, 70, f, 2, 6000, NULL, 0);
                at = add(buf, at, 70, NULL, 0, 0, NULL, 0);
            }
        at = add(buf, at, 300, NULL, 0, 0, NULL, 0);
        expect("all 16 DTMF keys", buf, at, NULL, 0);
    }
    {
        double f1 = 1700, f2 = 2200;
        size_t at = add(buf, 0, 500, &f1, 1, 8000, NULL, 0);
        at = add(buf, at, 300, NULL, 0, 0, NULL, 0);
        at = add(buf, at, 500, &f2, 1, 8000, NULL, 0);
        at = add(buf, at, 300, NULL, 0, 0, NULL, 0);
        expect("1700 or 2200 alone", buf, at, NULL, 0);
    }
    {
        size_t at = add(buf, 0, 2000, NULL, 0, 0, NULL, 8000);
        expect("loud noise", buf, at, NULL, 0);
    }
    {
        /* A speech-like warble sweeping through the band. */
        size_t at = 0;
        for (int i = 0; i < 40; i++) {
            double f = 300 + 80 * i;
            at = add(buf, at, 25, &f, 1, 7000, NULL, 0);
        }
        at = add(buf, at, 300, NULL, 0, 0, NULL, 0);
        expect("sweep", buf, at, NULL, 0);
    }

    if (fails) { printf("coin_tone: %d failures\n", fails); return 1; }
    printf("coin_tone: all passed\n");
    return 0;
}

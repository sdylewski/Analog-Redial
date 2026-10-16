/* Host test for the DTMF detector: synthetic dialing at the speeds real
 * handsets and tone dialers use, with noise, twist, quiet and clipped tones,
 * plus things that must NOT produce digits (noise, a synthetic "voice").
 * Run via run.sh. */
#include "dtmf.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int64_t g_now_us;
/* dtmf.c's dtmf_make_osc() needs this; the detector itself doesn't. */
void tone_osc_init(tone_osc_t *o, uint32_t sr, float f1, float f2, float a) { (void)o; (void)sr; (void)f1; (void)f2; (void)a; }

#define FS 48000
static const float R[4] = { 697, 770, 852, 941 }, C[4] = { 1209, 1336, 1477, 1633 };
static const char K[4][4] = { { '1', '2', '3', 'A' }, { '4', '5', '6', 'B' }, { '7', '8', '9', 'C' }, { '*', '0', '#', 'D' } };
static int16_t buf[FS * 12];
static size_t n;
static unsigned rng = 12345;
static double frand(void) { rng = rng * 1664525u + 1013904223u; return ((rng >> 8) & 0xFFFF) / 32768.0 - 1.0; }
static void put(double v) { if (v > 1) v = 1; if (v < -1) v = -1; buf[n++] = (int16_t)(v * 32767); }
static void silence(int ms, double noise) { for (size_t i = 0, m = (size_t)FS * ms / 1000; i < m; i++) put(frand() * noise); }
static void tone(char k, int ms, double amp, double twist_db, double noise)
{
    float r = 0, c = 0;
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) if (K[i][j] == k) { r = R[i]; c = C[j]; }
    double ac = amp * pow(10, twist_db / 20);
    for (size_t i = 0, m = (size_t)FS * ms / 1000; i < m; i++) {
        double t = (double)i / FS;
        put(amp * sin(2 * M_PI * r * t) + ac * sin(2 * M_PI * c * t) + frand() * noise);
    }
}
static void detect(char *out)
{
    dtmf_detector_t d;
    dtmf_detector_init(&d, FS);
    g_now_us = 0;
    int o = 0;
    for (size_t i = 0; i + 480 <= n; i += 480) {
        g_now_us = (int64_t)i * 1000000 / FS;
        char c = dtmf_detector_push(&d, buf + i, 480);
        if (c && o < 60) out[o++] = c;
    }
    out[o] = 0;
}

static int fails;
static void dial(const char *digits, int tone_ms, int gap_ms, double amp, double twist, double noise)
{
    n = 0;
    silence(300, noise);
    for (const char *p = digits; *p; p++) { tone(*p, tone_ms, amp, twist, noise); silence(gap_ms, noise); }
    silence(300, noise);
    char out[64];
    detect(out);
    if (strcmp(out, digits)) {
        printf("FAIL  dial %s  tone %d ms, gap %d ms, amp %.2f, twist %.0f dB, noise %.3f -> got '%s'\n",
               digits, tone_ms, gap_ms, amp, twist, noise, out);
        fails++;
    }
}
static void expect_nothing(const char *what)
{
    char out[64];
    detect(out);
    if (out[0]) { printf("FAIL  %s produced digits '%s'\n", what, out); fails++; }
}

int main(void)
{
    const char *keys = "123456789*0#";
    int speeds[][2] = { { 40, 40 }, { 50, 50 }, { 50, 30 }, { 60, 60 }, { 70, 70 }, { 100, 100 }, { 150, 150 } };
    for (size_t s = 0; s < sizeof(speeds) / sizeof(speeds[0]); s++) {
        dial(keys, speeds[s][0], speeds[s][1], 0.3, 0, 0.01);
        dial("8675309", speeds[s][0], speeds[s][1], 0.3, 0, 0.01);
    }
    dial(keys, 70, 70, 0.3, 4, 0.05);      /* twist (the classifier rejects more than ~4.8 dB, same as before) */
    dial(keys, 70, 70, 0.3, -3, 0.05);
    dial(keys, 70, 70, 0.3, 0, 0.12);      /* noisy line */
    dial(keys, 70, 70, 0.04, 0, 0.01);     /* quiet phone */
    dial(keys, 70, 70, 0.9, 0, 0.01);      /* clipped */
    dial(keys, 50, 50, 0.9, 0, 0.01);      /* clipped and fast */

    /* Not a key press: white noise, and a rough "voice" (harmonics of a
     * wandering pitch) loud enough to be mistaken for DTMF. */
    n = 0; for (size_t i = 0; i < (size_t)FS * 10; i++) put(frand() * 0.4);
    expect_nothing("white noise");
    n = 0;
    double ph = 0, f0 = 120;
    for (size_t i = 0; i < (size_t)FS * 10; i++) {
        f0 = 100 + 80 * (0.5 + 0.5 * sin(2 * M_PI * 0.7 * (double)i / FS));
        ph += 2 * M_PI * f0 / FS;
        double v = 0;
        for (int h = 1; h <= 30; h++) v += sin(h * ph) / h;
        put(0.25 * v * (0.6 + 0.4 * sin(2 * M_PI * 3.1 * (double)i / FS)));
    }
    expect_nothing("synthetic voice");

    if (fails) { printf("test_dtmf: %d FAILED\n", fails); return 1; }
    printf("test_dtmf: all checks passed\n");
    return 0;
}

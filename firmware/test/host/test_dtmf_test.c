/* Host test for the touch-tone test: simulated phones go through the capture
 * and the analysis, and the recommended limits must make them dial, without
 * letting speech-like audio through. Run via run.sh. */
#include "dtmf_test.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int64_t g_now_us;
void tone_osc_init(tone_osc_t *o, uint32_t sr, float f1, float f2, float a) { (void)o; (void)sr; (void)f1; (void)f2; (void)a; }

#define FS 48000
#define FRAME 480
static const float R[4] = { 697, 770, 852, 941 }, C[4] = { 1209, 1336, 1477, 1633 };
static const char K[4][4] = { { '1', '2', '3', 'A' }, { '4', '5', '6', 'B' }, { '7', '8', '9', 'C' }, { '*', '0', '#', 'D' } };
static const char *KEYS = "123456789*0#";
static unsigned rng = 12345;
static double frand(void) { rng = rng * 1664525u + 1013904223u; return ((rng >> 8) & 0xFFFF) / 32768.0 - 1.0; }
static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL  " __VA_ARGS__); printf("\n"); fails++; } } while (0)

typedef struct { double amp, twist_db, noise, cross; } phone_t;

static dtmf_detector_t det;
static dtmf_test_capture_t *cap;
static char dialed[64];
static int dialed_n;
static long t_samp;

static void feed(const int16_t *f)
{
    g_now_us = t_samp * 1000000 / FS;
    t_samp += FRAME;
    char d = dtmf_detector_push(&det, f, FRAME);
    if (d) {
        if (cap) dtmf_test_on_digit(cap, d);
        if (dialed_n < 63) { dialed[dialed_n++] = d; dialed[dialed_n] = 0; }
    }
}
static int16_t clip(double v) { if (v > 1) v = 1; if (v < -1) v = -1; return (int16_t)(v * 32767); }

static void quiet(const phone_t *p, int ms)
{
    int16_t f[FRAME];
    for (int m = 0; m < ms / 10; m++) {
        for (int i = 0; i < FRAME; i++) f[i] = clip(frand() * p->noise);
        feed(f);
    }
}
/* cross: a second row tone this far (dB) under the real one, as a phone with
 * sloppy filters sends. */
static void press(const phone_t *p, char key, int ms)
{
    double r = 0, c = 0, other = 0;
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) if (K[i][j] == key) { r = R[i]; c = C[j]; other = R[(i + 2) % 4]; }
    double ac = p->amp * pow(10, p->twist_db / 20), ao = p->cross ? p->amp * pow(10, p->cross / 20) : 0;
    int16_t f[FRAME];
    for (int m = 0; m < ms / 10; m++) {
        for (int i = 0; i < FRAME; i++) {
            double t = (double)(t_samp + i) / FS;
            f[i] = clip(p->amp * sin(2 * M_PI * r * t) + ac * sin(2 * M_PI * c * t) + ao * sin(2 * M_PI * other * t) + frand() * p->noise);
        }
        feed(f);
    }
}
static void reset(const dtmf_tuning_t *t)
{
    dtmf_detector_init(&det, FS);
    if (t) dtmf_detector_set_tuning(&det, t);
    dialed_n = 0; dialed[0] = 0; t_samp = 0;
}
static void run_test(const phone_t *p, const char *press_keys, const dtmf_tuning_t *cur, dtmf_test_result_t *r)
{
    reset(cur);
    dtmf_test_init(cap);
    dtmf_detector_set_capture(&det, dtmf_test_on_window, dtmf_test_on_sample, cap);
    quiet(p, 1200);
    for (const char *k = press_keys; *k; k++) {
        if (*k == '_') { cap->skip = true; quiet(p, 400); }
        else { press(p, *k, 1500); quiet(p, 400); }
    }
    dtmf_detector_set_capture(&det, NULL, NULL, NULL);
    const dtmf_tuning_t std = DTMF_TUNING_DEFAULT;
    dtmf_test_analyze(cap, cur ? cur : &std, r);
}
static void dial_normally(const phone_t *p, const dtmf_tuning_t *t, const char *keys, char *out)
{
    dtmf_test_capture_t *saved = cap;
    cap = NULL;
    reset(t);
    quiet(p, 200);
    for (const char *k = keys; *k; k++) { press(p, *k, 100); quiet(p, 100); }
    quiet(p, 200);
    strcpy(out, dialed);
    cap = saved;
}
static size_t report_n, report_lines_bad;
static void count_out(void *ctx, const char *s, size_t n) { (void)ctx; (void)s; report_n += n; if (!n) report_lines_bad++; }

int main(void)
{
    static dtmf_test_capture_t c;
    static dtmf_test_result_t r;
    cap = &c;
    const dtmf_tuning_t std = DTMF_TUNING_DEFAULT;
    char out[64];

    /* A good phone: every key heard, nothing to change. */
    phone_t good = { 0.3, 0, 0.005, 0 };
    run_test(&good, KEYS, NULL, &r);
    CHECK(r.verdict == DTMF_VERDICT_GOOD, "good phone: verdict %d", r.verdict);
    CHECK(r.tested == 12, "good phone: tested %d", r.tested);
    CHECK(!memcmp(&r.recommended, &std, sizeof(std)), "good phone: limits changed");
    for (int k = 0; k < 12; k++) {
        CHECK(r.keys[k].heard == KEYS[k], "good phone: %c heard as %c", KEYS[k], r.keys[k].heard);
        CHECK(r.keys[k].sent == KEYS[k], "good phone: %c sent as %c", KEYS[k], r.keys[k].sent);
        CHECK(r.keys[k].pass_std >= 90, "good phone: %c pass %d", KEYS[k], r.keys[k].pass_std);
    }

    /* Out-of-spec phones the standard limits can't hear: more twist than they
     * allow in either direction, a stray row tone, and a noisy line. */
    struct { const char *name; phone_t p; } bad[] = {
        { "column 8 dB low",  { 0.3, -8, 0.005, 0 } },
        { "column 7 dB high", { 0.15, 7, 0.005, 0 } },
        { "stray row tone",   { 0.3, 0, 0.005, -5 } },
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        dial_normally(&bad[i].p, NULL, KEYS, out);
        CHECK(strcmp(out, KEYS) != 0, "%s: already dials on the standard limits", bad[i].name);
        run_test(&bad[i].p, KEYS, NULL, &r);
        CHECK(r.verdict == DTMF_VERDICT_TUNED, "%s: verdict %d, %d still poor", bad[i].name, r.verdict, r.poor_rec);
        dial_normally(&bad[i].p, &r.recommended, KEYS, out);
        CHECK(!strcmp(out, KEYS), "%s: with the recommended limits dialed '%s'", bad[i].name, out);
        report_n = 0;
        dtmf_test_report(&c, &r, "firmware: test\n", count_out, NULL);
        CHECK(report_n > 5000 && report_n < 60000, "%s: report is %u bytes", bad[i].name, (unsigned)report_n);
    }

    /* Too far out to fix: says so, and doesn't exceed the caps. */
    phone_t hopeless = { 0.3, -16, 0.005, 0 };
    run_test(&hopeless, KEYS, NULL, &r);
    CHECK(r.verdict == DTMF_VERDICT_PARTIAL, "16 dB twist: verdict %d", r.verdict);
    CHECK(r.recommended.row_over_col_x10 <= 100, "16 dB twist: limit %d", r.recommended.row_over_col_x10);

    /* A wrong key is recorded as such and doesn't steer the limits. */
    run_test(&good, "923456789*0#", NULL, &r);
    CHECK(r.keys[0].sent == '9', "wrong key: 1 reads as sent %c", r.keys[0].sent);
    CHECK(!memcmp(&r.recommended, &std, sizeof(std)), "wrong key: limits changed");
    CHECK(r.verdict == DTMF_VERDICT_PARTIAL, "wrong key: verdict %d", r.verdict);

    /* Skipped keys, and a phone that only sends short fixed bursts. */
    run_test(&good, "1_3456789*0#", NULL, &r);
    CHECK(r.keys[1].n == 0 && r.tested == 11, "skip: key 2 has %d windows, %d tested", r.keys[1].n, r.tested);
    reset(NULL);
    dtmf_test_init(&c);
    dtmf_detector_set_capture(&det, dtmf_test_on_window, dtmf_test_on_sample, &c);
    quiet(&good, 1200);
    for (const char *k = KEYS; *k; k++) { press(&good, *k, 100); quiet(&good, 400); }
    dtmf_detector_set_capture(&det, NULL, NULL, NULL);
    CHECK(c.phase == DTMF_TEST_FINISHED, "short bursts: stuck on key %d, phase %d", c.idx, c.phase);

    /* Dial tone still draining when the test starts, then a noisy line: neither
     * may raise the level a key has to reach past a real key. */
    phone_t noisy = { 0.3, 0, 0.12, 0 };
    reset(NULL);
    dtmf_test_init(&c);
    dtmf_detector_set_capture(&det, dtmf_test_on_window, dtmf_test_on_sample, &c);
    { static int16_t ft[FRAME];
      for (int m = 0; m < 15; m++) {
          for (int i = 0; i < FRAME; i++) { double tt = (double)(t_samp + i) / FS; ft[i] = clip(0.4 * sin(2 * M_PI * 350 * tt) + 0.4 * sin(2 * M_PI * 440 * tt)); }
          feed(ft);
      } }
    quiet(&good, 1200);
    CHECK(c.tone_peak_min < 2000, "draining dial tone: key level %d", c.tone_peak_min);
    dtmf_detector_set_capture(&det, NULL, NULL, NULL);
    run_test(&noisy, KEYS, NULL, &r);
    CHECK(r.tested == 12, "noisy line: tested %d", r.tested);

    /* Silence: nothing to measure. */
    reset(NULL);
    dtmf_test_init(&c);
    dtmf_detector_set_capture(&det, dtmf_test_on_window, dtmf_test_on_sample, &c);
    quiet(&good, 3000);
    dtmf_detector_set_capture(&det, NULL, NULL, NULL);
    dtmf_test_analyze(&c, &std, &r);
    CHECK(r.verdict == DTMF_VERDICT_NONE, "silence: verdict %d", r.verdict);

    /* The loosest limits a recommendation can reach still ignore noise and a
     * rough "voice" (harmonics of a wandering pitch). */
    const dtmf_tuning_t loosest = { 100, 100, 50, 90, 50, 25 };
    reset(&loosest);
    static int16_t f[FRAME];
    for (int m = 0; m < 1000; m++) { for (int i = 0; i < FRAME; i++) f[i] = clip(frand() * 0.4); feed(f); }
    CHECK(dialed_n == 0, "loosest limits: white noise dialed '%s'", dialed);
    reset(&loosest);
    double ph = 0;
    for (int m = 0; m < 1000; m++) {
        for (int i = 0; i < FRAME; i++) {
            double t = (double)(m * FRAME + i) / FS, pitch = 110 + 40 * sin(2 * M_PI * 0.7 * t) + 15 * sin(2 * M_PI * 3.1 * t);
            ph += 2 * M_PI * pitch / FS;
            double v = 0, env = 0.5 + 0.5 * sin(2 * M_PI * 3 * t);
            for (int h = 1; h <= 20; h++) v += sin(h * ph) / h;
            f[i] = clip(0.3 * env * v);
        }
        feed(f);
    }
    CHECK(dialed_n == 0, "loosest limits: voice dialed '%s'", dialed);

    if (!fails) printf("test_dtmf_test: all checks passed\n");
    return fails ? 1 : 0;
}

#include "dtmf_test.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* Windows of ~25.6 ms. */
#define NOISE_SETTLE    10   /* the dial tone that just stopped is still draining from the audio path */
#define NOISE_WINDOWS   30   /* the quiet line, before the first key, settling included */
#define MAX_TONE_PEAK 3000   /* a noisy line mustn't raise the "is it a key" level past real keys */
#define QUIET_WINDOWS    4   /* no tone for this long = the key was let go */
#define PRESS_TIMEOUT  780   /* ~20 s waiting for a key, then move on */
#define MIN_WINDOWS      2   /* shorter is a glitch: about the shortest press a phone can send */
#define TONE_CONC_PCT   25   /* as DTMF_MIN_CONC_PCT, and the lowest that limit can be tuned to */
#define MIN_TONE_PEAK  200

/* How far a recommendation may loosen each limit. Past these, speech and music
 * start to look like keys (firmware/test/host checks speech at these limits). */
#define CAP_TWIST_X10  100   /* 10x power, 10 dB */
#define CAP_OTHER_PCT   50   /* the other bins may reach half the winner's power, -3 dB */
#define CAP_ROW01_PCT   90
#define MARGIN_DB      1.5f
#define MARGIN_CONC    5.0f

/* A key is fine if most of its windows pass: the detector only needs three
 * overlapping windows (~38 ms) in a row. */
#define KEY_OK_PCT      60
/* Only a poorer key than this asks for looser limits. */
#define KEY_TUNE_PCT    80
#define CLIP_PEAK     0x7C   /* ~95% of full scale */

static const char KEYS[DTMF_TEST_KEYS + 1] = "123456789*0#";

char dtmf_test_key(int idx)
{
    return (idx >= 0 && idx < DTMF_TEST_KEYS) ? KEYS[idx] : 0;
}

void dtmf_test_init(dtmf_test_capture_t *c)
{
    memset(c, 0, sizeof(*c));
    c->phase = DTMF_TEST_NOISE;
    c->tone_peak_min = MIN_TONE_PEAK;
}

static uint8_t to_db2(int64_t v)
{
    float db2 = 20.0f * log10f((float)v + 1.0f);
    return db2 >= 255.0f ? 255 : (uint8_t)db2;
}

static bool is_tone(const dtmf_test_capture_t *c, const dtmf_window_t *w)
{
    if (w->peak < c->tone_peak_min) return false;
    int64_t r = 0, col = 0;
    for (int i = 0; i < 4; i++) {
        if (w->mags[i] > r) r = w->mags[i];
        if (w->mags[4 + i] > col) col = w->mags[4 + i];
    }
    return (r + col) * 200 >= (int64_t)TONE_CONC_PCT * (int64_t)w->block_n * w->energy;
}

static void store(dtmf_test_capture_t *c, const dtmf_window_t *w)
{
    int k = c->idx;
    if (c->n[k] >= DTMF_TEST_WINDOWS) return;
    dtmf_test_win_t *s = &c->win[k][c->n[k]++];
    for (int i = 0; i < 8; i++) s->bins[i] = to_db2(w->mags[i]);
    s->energy = to_db2(w->energy);
    int p = w->peak / 256;
    s->peak = p > 255 ? 255 : (uint8_t)p;
}

static void next_key(dtmf_test_capture_t *c)
{
    c->quiet_run = 0;
    c->wait_n = 0;
    if (c->idx + 1 >= DTMF_TEST_KEYS) {
        c->phase = DTMF_TEST_FINISHED;
    } else {
        c->idx = c->idx + 1;
        c->phase = DTMF_TEST_RELEASE;
    }
}

void dtmf_test_on_window(void *ctx, const dtmf_window_t *w)
{
    dtmf_test_capture_t *c = ctx;
    if (w->bank != 0 || c->phase == DTMF_TEST_FINISHED) return;

    if (c->skip) {
        c->skip = false;
        if (c->phase != DTMF_TEST_NOISE) {
            c->n[c->idx] = 0;
            c->snip_n[c->idx] = 0;
            next_key(c);
            return;
        }
    }
    bool tone = is_tone(c, w);

    switch (c->phase) {
    case DTMF_TEST_NOISE:
        if (c->noise_n >= NOISE_SETTLE && w->peak > c->noise_peak) c->noise_peak = w->peak;
        if (++c->noise_n >= NOISE_WINDOWS) {
            int level = c->noise_peak * 3;
            if (level > MAX_TONE_PEAK) level = MAX_TONE_PEAK;
            if (level > c->tone_peak_min) c->tone_peak_min = level;
            c->phase = DTMF_TEST_RELEASE;
        }
        break;
    case DTMF_TEST_RELEASE:
        c->quiet_run = tone ? 0 : c->quiet_run + 1;
        if (c->quiet_run >= QUIET_WINDOWS) {
            c->quiet_run = 0;
            c->wait_n = 0;
            c->phase = DTMF_TEST_PRESS;
        }
        break;
    case DTMF_TEST_PRESS:
        if (tone) {
            c->n[c->idx] = 0;
            c->quiet_run = 0;
            store(c, w);
            c->phase = DTMF_TEST_HOLD;
        } else if (++c->wait_n >= PRESS_TIMEOUT) {
            next_key(c);
        }
        break;
    case DTMF_TEST_HOLD:
        store(c, w);
        c->quiet_run = tone ? 0 : c->quiet_run + 1;
        if (c->quiet_run >= QUIET_WINDOWS) {
            c->n[c->idx] -= QUIET_WINDOWS; /* those are the silence after the key */
            if (c->n[c->idx] < MIN_WINDOWS) {
                c->n[c->idx] = 0;
                c->snip_n[c->idx] = 0;
                c->quiet_run = 0;
                c->phase = DTMF_TEST_PRESS;
            } else {
                next_key(c);
            }
        } else if (c->n[c->idx] >= DTMF_TEST_WINDOWS) {
            next_key(c);
        }
        break;
    default:
        break;
    }
}

void dtmf_test_on_sample(void *ctx, int16_t raw)
{
    dtmf_test_capture_t *c = ctx;
    if (c->phase != DTMF_TEST_HOLD) return;
    int k = c->idx;
    /* Skip the start of the press. A short press may never get this far, and
     * its report simply has no samples. */
    if (c->n[k] < 2 || c->quiet_run > 0 || c->snip_n[k] >= DTMF_TEST_SNIPPET) return;
    c->snip[k][c->snip_n[k]++] = raw;
}

void dtmf_test_on_digit(dtmf_test_capture_t *c, char digit)
{
    if (c->phase != DTMF_TEST_PRESS && c->phase != DTMF_TEST_HOLD) return;
    int k = c->idx;
    if (!c->heard[k]) c->heard[k] = digit;
    if (c->heard_n[k] < 255) c->heard_n[k]++;
}

/* ---- analysis ---- */

typedef struct {
    float col_over_row, row_over_col; /* dB */
    float row_other, row01, col_other;
    float conc;                       /* percent */
} limits_t;

static float ratio_db(float x) { return 10.0f * log10f(x); }

static limits_t limits_of(const dtmf_tuning_t *t)
{
    return (limits_t){
        .col_over_row = ratio_db(t->col_over_row_x10 / 10.0f),
        .row_over_col = ratio_db(t->row_over_col_x10 / 10.0f),
        .row_other    = ratio_db(t->row_other_pct / 100.0f),
        .row01        = ratio_db(t->row01_pct / 100.0f),
        .col_other    = ratio_db(t->col_other_pct / 100.0f),
        .conc         = (float)t->conc_pct,
    };
}

typedef struct {
    float twist;      /* column over row, dB */
    float row_other, row01, col_other;
    float conc;
} metrics_t;

static void key_pos(int k, int *r, int *col)
{
    float rf = 0, cf = 0;
    dtmf_key_freqs(KEYS[k], &rf, &cf);
    *r = rf < 730 ? 0 : rf < 810 ? 1 : rf < 900 ? 2 : 3;
    *col = cf < 1270 ? 0 : cf < 1400 ? 1 : cf < 1550 ? 2 : 3;
}

static metrics_t measure(const dtmf_test_win_t *w, int r, int col)
{
    metrics_t m = { .row_other = -99, .row01 = -99, .col_other = -99 };
    float rdb = w->bins[r] * 0.5f, cdb = w->bins[4 + col] * 0.5f;
    m.twist = cdb - rdb;
    for (int i = 0; i < 4; i++) {
        if (i != r) {
            float d = w->bins[i] * 0.5f - rdb;
            bool pair = (r == 0 && i == 1) || (r == 1 && i == 0);
            if (pair) { if (d > m.row01) m.row01 = d; }
            else      { if (d > m.row_other) m.row_other = d; }
        }
        if (i != col) {
            float d = w->bins[4 + i] * 0.5f - cdb;
            if (d > m.col_other) m.col_other = d;
        }
    }
    /* (r + c) * 200 / (N * energy), with 205-sample windows */
    float e = powf(10.0f, w->energy * 0.05f);
    float sum = powf(10.0f, rdb * 0.1f) + powf(10.0f, cdb * 0.1f);
    m.conc = sum * 200.0f / (205.0f * (e > 1.0f ? e : 1.0f));
    return m;
}

static bool passes(const metrics_t *m, const limits_t *l)
{
    if (m->twist > l->col_over_row || -m->twist > l->row_over_col) return false;
    if (m->row_other > l->row_other || m->row01 > l->row01 || m->col_other > l->col_other) return false;
    return m->conc >= l->conc;
}

static void sort_floats(float *a, int n)
{
    for (int i = 1; i < n; i++) {
        float v = a[i];
        int j = i - 1;
        while (j >= 0 && a[j] > v) { a[j + 1] = a[j]; j--; }
        a[j + 1] = v;
    }
}

/* The attack and the release are partly silence, so they say nothing about
 * the phone's tone. */
static void usable_range(int n, int *from, int *to)
{
    *from = n >= 5 ? 1 : 0;
    *to = n >= 5 ? n - 1 : n;
}

static uint8_t pass_pct(const dtmf_test_win_t *win, int n, int r, int col, const dtmf_tuning_t *t)
{
    int from, to, ok = 0;
    usable_range(n, &from, &to);
    if (to <= from) return 0;
    limits_t l = limits_of(t);
    for (int i = from; i < to; i++) {
        metrics_t m = measure(&win[i], r, col);
        if (passes(&m, &l)) ok++;
    }
    return (uint8_t)(ok * 100 / (to - from));
}

static uint8_t clamp_u8(float v, int lo, int hi)
{
    int i = (int)ceilf(v);
    return (uint8_t)(i < lo ? lo : i > hi ? hi : i);
}

void dtmf_test_analyze(const dtmf_test_capture_t *c, const dtmf_tuning_t *current, dtmf_test_result_t *r)
{
    const dtmf_tuning_t std = DTMF_TUNING_DEFAULT;
    memset(r, 0, sizeof(*r));
    r->current = *current;
    r->recommended = std;
    r->noise_peak = c->noise_peak;

    /* What each limit needs, worst key first. */
    float need_cor = -99, need_roc = -99, need_row = -99, need_01 = -99, need_col = -99, need_conc = 999;
    bool any_poor = false;

    for (int k = 0; k < DTMF_TEST_KEYS; k++) {
        dtmf_test_key_result_t *kr = &r->keys[k];
        int rr, cc, n = c->n[k], from, to;
        key_pos(k, &rr, &cc);
        kr->key = KEYS[k];
        kr->heard = c->heard[k];
        kr->heard_n = c->heard_n[k];
        kr->n = (uint8_t)n;
        usable_range(n, &from, &to);
        if (to <= from) continue;
        const dtmf_test_win_t *win = c->win[k];

        /* Which key was actually pressed: a wrong one mustn't steer the limits. */
        int row_votes[4] = {0}, col_votes[4] = {0}, clip = 0;
        for (int i = from; i < to; i++) {
            int br = 0, bc = 0;
            for (int j = 1; j < 4; j++) {
                if (win[i].bins[j] > win[i].bins[br]) br = j;
                if (win[i].bins[4 + j] > win[i].bins[4 + bc]) bc = j;
            }
            row_votes[br]++;
            col_votes[bc]++;
            if (win[i].peak >= CLIP_PEAK) clip++;
        }
        int br = 0, bc = 0;
        for (int j = 1; j < 4; j++) {
            if (row_votes[j] > row_votes[br]) br = j;
            if (col_votes[j] > col_votes[bc]) bc = j;
        }
        for (int j = 0; j < DTMF_TEST_KEYS; j++) {
            int jr, jc;
            key_pos(j, &jr, &jc);
            if (jr == br && jc == bc) kr->sent = KEYS[j];
        }
        kr->clip_pct = (uint8_t)(clip * 100 / (to - from));
        if (kr->clip_pct >= 20) r->clipped = true;

        float tw[DTMF_TEST_WINDOWS], ro[DTMF_TEST_WINDOWS], p01[DTMF_TEST_WINDOWS];
        float co[DTMF_TEST_WINDOWS], cn[DTMF_TEST_WINDOWS], pk[DTMF_TEST_WINDOWS];
        int m = 0;
        for (int i = from; i < to; i++, m++) {
            metrics_t mt = measure(&win[i], rr, cc);
            tw[m] = mt.twist; ro[m] = mt.row_other; p01[m] = mt.row01; co[m] = mt.col_other; cn[m] = mt.conc;
            pk[m] = win[i].peak;
        }
        sort_floats(pk, m);
        kr->peak = (uint8_t)pk[m / 2];
        sort_floats(tw, m);
        kr->twist_db10 = (int16_t)lroundf(tw[m / 2] * 10.0f);

        kr->pass_std = pass_pct(win, n, rr, cc, &std);
        kr->pass_cur = pass_pct(win, n, rr, cc, current);
        r->tested++;
        if (kr->sent != kr->key) continue; /* wrong key: it stays poor, and doesn't count below */

        if (kr->pass_std < KEY_TUNE_PCT) any_poor = true;
        /* What 80% of this key's windows need, one limit at a time. */
        int hi = (m * 8) / 10, lo = m - 1 - hi;
        if (hi >= m) hi = m - 1;
        if (lo < 0) lo = 0;
        sort_floats(ro, m); sort_floats(p01, m); sort_floats(co, m); sort_floats(cn, m);
        if (tw[hi] > need_cor)  need_cor = tw[hi];
        if (-tw[lo] > need_roc) need_roc = -tw[lo];
        if (ro[hi] > need_row)  need_row = ro[hi];
        if (p01[hi] > need_01)  need_01 = p01[hi];
        if (co[hi] > need_col)  need_col = co[hi];
        if (cn[lo] < need_conc) need_conc = cn[lo];
    }

    if (any_poor) {
        /* Only ever looser than the standard limits, and never past the caps. */
        dtmf_tuning_t t = std;
        t.col_over_row_x10 = clamp_u8(10.0f * powf(10.0f, (need_cor + MARGIN_DB) * 0.1f), std.col_over_row_x10, CAP_TWIST_X10);
        t.row_over_col_x10 = clamp_u8(10.0f * powf(10.0f, (need_roc + MARGIN_DB) * 0.1f), std.row_over_col_x10, CAP_TWIST_X10);
        t.row_other_pct = clamp_u8(100.0f * powf(10.0f, (need_row + MARGIN_DB) * 0.1f), std.row_other_pct, CAP_OTHER_PCT);
        t.row01_pct     = clamp_u8(100.0f * powf(10.0f, (need_01 + MARGIN_DB) * 0.1f), std.row01_pct, CAP_ROW01_PCT);
        t.col_other_pct = clamp_u8(100.0f * powf(10.0f, (need_col + MARGIN_DB) * 0.1f), std.col_other_pct, CAP_OTHER_PCT);
        float conc = need_conc - MARGIN_CONC;
        t.conc_pct = conc >= std.conc_pct ? std.conc_pct : conc <= TONE_CONC_PCT ? TONE_CONC_PCT : (uint8_t)conc;
        r->recommended = t;
    }

    int poor_std = 0;
    for (int k = 0; k < DTMF_TEST_KEYS; k++) {
        dtmf_test_key_result_t *kr = &r->keys[k];
        int rr, cc;
        key_pos(k, &rr, &cc);
        kr->pass_rec = pass_pct(c->win[k], c->n[k], rr, cc, &r->recommended);
        if (!kr->n) continue;
        if (kr->pass_std < KEY_OK_PCT) poor_std++;
        if (kr->pass_rec < KEY_OK_PCT) r->poor_rec++;
    }
    if (r->tested == 0)          r->verdict = DTMF_VERDICT_NONE;
    else if (!any_poor && poor_std == 0) r->verdict = DTMF_VERDICT_GOOD;
    else if (r->poor_rec == 0)   r->verdict = DTMF_VERDICT_TUNED;
    else                         r->verdict = DTMF_VERDICT_PARTIAL;
}

/* ---- report ---- */

static void outf(dtmf_test_out_t out, void *ctx, const char *fmt, ...)
{
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if (n >= (int)sizeof(line)) n = (int)sizeof(line) - 1;
    out(ctx, line, (size_t)n);
}

static void out_tuning(dtmf_test_out_t out, void *ctx, const char *name, const dtmf_tuning_t *t)
{
    outf(out, ctx, "%s: col_over_row_x10=%u row_over_col_x10=%u row_other_pct=%u row01_pct=%u col_other_pct=%u conc_pct=%u\n",
         name, t->col_over_row_x10, t->row_over_col_x10, t->row_other_pct, t->row01_pct, t->col_other_pct, t->conc_pct);
}

void dtmf_test_report(const dtmf_test_capture_t *c, const dtmf_test_result_t *r, const char *header,
                      dtmf_test_out_t out, void *ctx)
{
    static const char *const VERDICTS[] = { "good", "tuned", "partial", "none" };
    const dtmf_tuning_t std = DTMF_TUNING_DEFAULT;
    outf(out, ctx, "Analog Redial touch-tone test report, format 1\n");
    out(ctx, header, strlen(header));
    outf(out, ctx, "verdict: %s\nclipped: %s\ntested: %d of %d\nstill_poor: %d\nnoise_peak: %d\n",
         VERDICTS[r->verdict], r->clipped ? "yes" : "no", r->tested, DTMF_TEST_KEYS, r->poor_rec, r->noise_peak);
    out_tuning(out, ctx, "tuning_standard", &std);
    out_tuning(out, ctx, "tuning_current", &r->current);
    out_tuning(out, ctx, "tuning_recommended", &r->recommended);
    outf(out, ctx, "\nkey sent heard times windows pass_std pass_cur pass_rec clip_pct peak_x256 twist_db\n");
    for (int k = 0; k < DTMF_TEST_KEYS; k++) {
        const dtmf_test_key_result_t *kr = &r->keys[k];
        outf(out, ctx, "%c   %c    %c     %-5u %-7u %-8u %-8u %-8u %-8u %-9u %d.%d\n",
             kr->key, kr->sent ? kr->sent : '-', kr->heard ? kr->heard : '-', kr->heard_n, kr->n,
             kr->pass_std, kr->pass_cur, kr->pass_rec, kr->clip_pct, kr->peak,
             kr->twist_db10 / 10, (kr->twist_db10 < 0 ? -kr->twist_db10 : kr->twist_db10) % 10);
    }
    outf(out, ctx, "\nwindows: one line per ~25.6 ms, hex. 8 bins (r0-r3 c0-c3), energy, peak/256. Bins and energy are 20*log10.\n");
    for (int k = 0; k < DTMF_TEST_KEYS; k++) {
        outf(out, ctx, "key %c\n", KEYS[k]);
        for (int i = 0; i < c->n[k]; i++) {
            const dtmf_test_win_t *w = &c->win[k][i];
            outf(out, ctx, "%02x%02x%02x%02x %02x%02x%02x%02x %02x %02x\n", w->bins[0], w->bins[1], w->bins[2], w->bins[3],
                 w->bins[4], w->bins[5], w->bins[6], w->bins[7], w->energy, w->peak);
        }
    }
    outf(out, ctx, "\nsamples: raw 8 kHz signed 16-bit, hex, from the middle of each press\n");
    for (int k = 0; k < DTMF_TEST_KEYS; k++) {
        outf(out, ctx, "key %c, %d samples\n", KEYS[k], c->snip_n[k]);
        for (int i = 0; i < c->snip_n[k]; i += 20) {
            char line[108];
            int o = 0;
            for (int j = i; j < i + 20 && j < c->snip_n[k]; j++)
                o += snprintf(line + o, sizeof(line) - o, "%04x", (uint16_t)c->snip[k][j]);
            line[o++] = '\n';
            out(ctx, line, (size_t)o);
        }
    }
}

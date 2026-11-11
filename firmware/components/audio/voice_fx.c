#include "voice_fx.h"
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/*
 * All effects run on 8 kHz audio (audio.c decimates the 48 kHz streams to
 * 8 kHz around them; a phone call carries nothing above 4 kHz anyway). Other
 * rates still work, with times scaled, but the FFT robot is sized for 8 kHz.
 *
 * Pitch (chipmunk): a delay line read faster (or slower) than it's
 * written. When the read point runs out of room it jumps back (or forward)
 * by about a whole number of voice periods, the jump picked by
 * cross-correlation so the old and new read points are in step, then
 * crossfades. The earlier version jumped blindly every 15-30 ms, and the
 * out-of-phase crossfades were the "garbled" warble.
 *
 * Robot: short-time Fourier transform, every frame's phases set to zero,
 * resynthesized one hop apart (8 ms at the default 125 Hz): every frame becomes
 * the same short chirp, so the voice turns into a monotone buzz with its own
 * vowels and consonants.
 *
 * Dalek: ring modulation, the voice times a low sine. Tremolo: the voice's
 * volume swung by a sine. Both level-matched so the effect isn't quieter.
 *
 * Every effect's strength is a setting (VOICE_FX_PARAMS).
 */

/* Pitch shifter timing, in ms. */
#define PS_BUF_MS   64    /* delay line: the read point and its correlation look-back reach ~41 ms */
#define PS_DMIN_MS  4     /* pitch up: splice when the read point gets this close */
#define PS_DMAX_MS  40    /* pitch down: splice when it falls this far behind */
#define PS_JMIN_MS  10    /* splice jump search range: a few voice periods */
#define PS_JMAX_MS  22
#define PS_CORR_MS  10    /* correlation length */
#define PS_XF_MS    8     /* crossfade */
#define PS_AA_K     0.435f  /* pitch-up low-pass corner = this / ratio x the sample rate
                             * (2.3 kHz at 8 kHz for 1.5x): the shift puts it at 3.5 kHz,
                             * under the 4 kHz limit */

/* Robot: 256-point FFT; the hop (sample rate / buzz pitch) is set at run time. */
#define RB_N      256
#define RB_LOG2N  8

struct voice_fx_state {
    uint32_t   sr;
    voice_fx_t last;

    uint32_t ring_phase;                 /* Q32 turns: Dalek's and tremolo's sine */

    /* Pitch shifter. d_q16 is how far the read point is behind the newest
     * sample; during a crossfade d_old_q16 is the outgoing read point. */
    int16_t *buf;
    uint32_t mask, w;
    int32_t  d_q16, d_old_q16;
    uint32_t xf_left;
    uint32_t dmin, dmax, jmin, jmax, corr_n, xf_n;
    int16_t *xf_tab;                     /* fade-in gains, Q15 */
    /* Pitch up: 6th-order low-pass ahead of the delay line, so what the shift
     * pushes past the Nyquist frequency is gone instead of folding back. Three
     * biquads, Q28 coefficients b0 b1 b2 a1 a2, direct form I. */
    int32_t  aa_c[3][5];
    int32_t  aa_z[3][4];                 /* x1 x2 y1 y2 */
    int      aa_pct;                     /* chipmunk pitch the filter is designed for */

    /* Diagnostics: window since the last voice_fx_take_stats(). */
    uint32_t st_n, st_limited;
    int32_t  st_in_peak, st_out_peak;
    uint64_t st_in_sq, st_out_sq;

    /* Robot: last RB_N inputs, the overlap-add accumulator, position in the hop. */
    int16_t *rb_in;
    int32_t *rb_out;
    uint32_t rb_h, rb_hop;
    int32_t  rb_gain_q8;
};

static int16_t s_sine[256];
static bool s_tables_ready;

/* Robot tables and FFT work area, shared: every stream is processed on the
 * audio task, one after another (fxtest runs only with no call up). */
static int16_t s_hann[RB_N];             /* periodic Hann, Q15 */
static int16_t s_cos[RB_N / 2], s_sin[RB_N / 2];
static int16_t s_ph_c[RB_N / 2 + 1], s_ph_s[RB_N / 2 + 1];   /* robot's fixed phase, Q15 */
static uint8_t s_rev[RB_N];
static int32_t s_re[RB_N], s_im[RB_N];

static void init_tables(void)
{
    if (s_tables_ready) return;
    for (int i = 0; i < 256; i++) s_sine[i] = (int16_t)lrintf(32767.0f * sinf(i * 6.2831853f / 256));
    for (int i = 0; i < RB_N; i++) {
        s_hann[i] = (int16_t)lrintf(32767.0f * 0.5f * (1.0f - cosf(6.2831853f * i / RB_N)));
        unsigned r = 0;
        for (int b = 0; b < RB_LOG2N; b++) if (i & (1 << b)) r |= 1u << (RB_LOG2N - 1 - b);
        s_rev[i] = (uint8_t)r;
    }
    for (int i = 0; i < RB_N / 2; i++) {
        s_cos[i] = (int16_t)lrintf(32767.0f * cosf(6.2831853f * i / RB_N));
        s_sin[i] = (int16_t)lrintf(32767.0f * sinf(6.2831853f * i / RB_N));
    }
    s_tables_ready = true;
}

/* Robot's fixed phase. Quadratic: bin k is delayed spread * k / (N/2)
 * samples, so each frame is a short upward chirp instead of a click. With
 * the spread equal to the hop, one chirp runs into the next: a continuous
 * buzz at sample_rate / hop. Shared by every stream; rebuilt when the
 * robot's pitch setting changes. */
static uint32_t s_ph_spread;

static void robot_phase_table(uint32_t spread)
{
    if (spread == s_ph_spread) return;
    for (int k = 0; k <= RB_N / 2; k++) {
        float ph = -3.14159265f * (float)spread * (float)k * (float)k / (float)(RB_N / 2) / (float)RB_N;
        s_ph_c[k] = (int16_t)lrintf(32767.0f * cosf(ph));
        s_ph_s[k] = (int16_t)lrintf(32767.0f * sinf(ph));
    }
    s_ph_spread = spread;
}

/* ---- tuning (Settings -> Phone -> Voice effects) ------------------------ */

const voice_fx_param_info_t VOICE_FX_PARAMS[VFX_NPARAMS] = {
    [VFX_CHIP_PCT]   = { VOICE_FX_CHIPMUNK, "chip",    "Pitch (100% is your own voice, below it deeper)", "%", 70, 200, 5, 130 },
    [VFX_ROBOT_MIX]  = { VOICE_FX_ROBOT,    "rbmix",   "Amount (the rest is your own voice)", "%", 0, 100, 5, 100 },
    [VFX_ROBOT_HZ]   = { VOICE_FX_ROBOT,    "rbhz",    "Buzz pitch", " Hz", 70, 200, 5, 125 },
    [VFX_DALEK_HZ]   = { VOICE_FX_DALEK,    "dkhz",    "Buzz speed", " Hz", 15, 100, 5, 30 },
    [VFX_DALEK_MIX]  = { VOICE_FX_DALEK,    "dkmix",   "Amount (the rest is your own voice)", "%", 0, 100, 5, 100 },
    [VFX_TREM_HZ]    = { VOICE_FX_TREMOLO,  "trhz",    "Speed", " Hz", 2, 15, 1, 6 },
    [VFX_TREM_DEPTH] = { VOICE_FX_TREMOLO,  "trdepth", "Depth", "%", 10, 100, 5, 70 },
};

static volatile int16_t s_param[VFX_NPARAMS] = { 130, 100, 125, 30, 100, 6, 70 };

void voice_fx_set_param(voice_fx_param_t p, int v)
{
    if ((unsigned)p >= VFX_NPARAMS) return;
    const voice_fx_param_info_t *i = &VOICE_FX_PARAMS[p];
    s_param[p] = (int16_t)(v < i->min ? i->min : (v > i->max ? i->max : v));
}

int voice_fx_param(voice_fx_param_t p)
{
    return (unsigned)p < VFX_NPARAMS ? s_param[p] : 0;
}

bool voice_fx_valid(int fx)
{
    return fx == VOICE_FX_NONE || fx == VOICE_FX_DALEK || fx == VOICE_FX_CHIPMUNK ||
           fx == VOICE_FX_ROBOT || fx == VOICE_FX_TREMOLO;
}

static uint32_t pow2_at_least(uint32_t n)
{
    uint32_t s = 1;
    while (s < n) s <<= 1;
    return s;
}

/* Pitch-up low-pass, for a shift of pct/100: corner at PS_AA_K / ratio x the
 * sample rate, so what lands under the Nyquist frequency after the shift is
 * kept (0.29 x fs, 2.3 kHz at 8 kHz, for 1.5x). Butterworth sections (6th
 * order: Q 0.5176, 0.7071, 1.9319), bilinear transform. */
static void antialias_design(voice_fx_state_t *s, int pct)
{
    float frac = PS_AA_K * 100.0f / (float)pct;
    if (frac > 0.45f) frac = 0.45f;
    static const float q[3] = { 0.5176f, 0.7071f, 1.9319f };
    float wc = tanf(3.14159265f * frac);
    for (int k = 0; k < 3; k++) {
        float K = wc, norm = 1.0f / (1.0f + K / q[k] + K * K);
        float b0 = K * K * norm, a1 = 2.0f * (K * K - 1.0f) * norm, a2 = (1.0f - K / q[k] + K * K) * norm;
        const float c[5] = { b0, 2.0f * b0, b0, a1, a2 };
        for (int i = 0; i < 5; i++) s->aa_c[k][i] = (int32_t)lrintf(c[i] * 268435456.0f);
    }
    s->aa_pct = pct;
}

voice_fx_state_t *voice_fx_create(uint32_t sample_rate)
{
    init_tables();
    voice_fx_state_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->sr = sample_rate;
    uint32_t ms = sample_rate / 1000;
    uint32_t size = pow2_at_least(sample_rate * PS_BUF_MS / 1000);
    s->dmin = PS_DMIN_MS * ms;
    s->dmax = PS_DMAX_MS * ms;
    s->jmin = PS_JMIN_MS * ms;
    s->jmax = PS_JMAX_MS * ms;
    s->corr_n = PS_CORR_MS * ms;
    s->xf_n = PS_XF_MS * ms;
    s->buf = calloc(size, sizeof(int16_t));
    s->xf_tab = calloc(s->xf_n, sizeof(int16_t));
    s->rb_in = calloc(RB_N, sizeof(int16_t));
    s->rb_out = calloc(RB_N, sizeof(int32_t));
    if (!s->buf || !s->xf_tab || !s->rb_in || !s->rb_out) {
        voice_fx_destroy(s);
        return NULL;
    }
    s->mask = size - 1;
    for (uint32_t i = 0; i < s->xf_n; i++)
        s->xf_tab[i] = (int16_t)((32767u * (i + 1)) / (s->xf_n + 1));
    antialias_design(s, s_param[VFX_CHIP_PCT]);
    return s;
}

void voice_fx_destroy(voice_fx_state_t *s)
{
    if (!s) return;
    free(s->buf);
    free(s->xf_tab);
    free(s->rb_in);
    free(s->rb_out);
    free(s);
}

static void reset(voice_fx_state_t *s)
{
    memset(s->buf, 0, (s->mask + 1) * sizeof(int16_t));
    memset(s->rb_in, 0, RB_N * sizeof(int16_t));
    memset(s->rb_out, 0, RB_N * sizeof(int32_t));
    s->rb_h = 0;
    s->rb_hop = 0;
    s->w = 0;
    s->d_q16 = (int32_t)(((s->dmin + s->dmax) / 2) << 16);
    s->xf_left = 0;
    memset(s->aa_z, 0, sizeof(s->aa_z));
    s->ring_phase = 0;
}

static inline int16_t sat16(int32_t v)
{
    return v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v;
}

/* ---- pitch ------------------------------------------------------------- */

/* Reads the delay line `d` (Q16 samples) behind the newest sample (the one at
 * w - 1), linearly interpolated. */
static inline int32_t tap(const voice_fx_state_t *s, int32_t d)
{
    int64_t pos = ((int64_t)(s->w - 1) << 16) - d;
    uint32_t i = (uint32_t)(pos >> 16);
    int32_t frac = (int32_t)(pos & 0xFFFF);
    int32_t a = s->buf[i & s->mask], b = s->buf[(i + 1) & s->mask];
    return a + (int32_t)(((int64_t)(b - a) * frac) >> 16);
}

/* How far to jump: the offset in [jmin, jmax] whose recent past best matches
 * the recent past at the current read point (normalized cross-correlation),
 * so the crossfade joins two waveforms that are in step. */
static uint32_t best_jump(const voice_fx_state_t *s, bool back)
{
    uint32_t p = s->w - 1 - (uint32_t)(s->d_q16 >> 16);   /* current read sample */
    uint32_t L = s->corr_n, m = s->mask;
    const int16_t *b = s->buf;
    float best = -1e30f;
    uint32_t best_j = (s->jmin + s->jmax) / 2;
    for (uint32_t j = s->jmin; j <= s->jmax; j++) {
        uint32_t q = back ? p - j : p + j;
        int64_t xy = 0, yy = 0;
        for (uint32_t i = 1; i <= L; i++) {
            int32_t x = b[(p - i) & m] >> 2, y = b[(q - i) & m] >> 2;
            xy += x * y;
            yy += y * y;
        }
        float score = (float)xy / sqrtf((float)yy + 1.0f);
        if (score > best) { best = score; best_j = j; }
    }
    return best_j;
}

static inline int32_t antialias(voice_fx_state_t *s, int32_t v)
{
    for (int k = 0; k < 3; k++) {
        const int32_t *c = s->aa_c[k];
        int32_t *z = s->aa_z[k];
        int64_t acc = (int64_t)c[0] * v + (int64_t)c[1] * z[0] + (int64_t)c[2] * z[1]
                    - (int64_t)c[3] * z[2] - (int64_t)c[4] * z[3];
        int32_t y = (int32_t)(acc >> 28);
        z[1] = z[0]; z[0] = v;
        z[3] = z[2]; z[2] = y;
        v = y;
    }
    return v;
}

static void pitch(voice_fx_state_t *s, int32_t ratio_q16, int16_t *x, size_t n)
{
    int32_t step = 65536 - ratio_q16;            /* < 0: pitch up */
    for (size_t k = 0; k < n; k++) {
        s->buf[s->w & s->mask] = step < 0 ? sat16(antialias(s, x[k])) : x[k];
        s->w++;
        int32_t y;
        if (s->xf_left) {
            int32_t g = s->xf_tab[s->xf_n - s->xf_left];
            y = (int32_t)(((int64_t)tap(s, s->d_q16) * g +
                           (int64_t)tap(s, s->d_old_q16) * (32768 - g)) >> 15);
            s->d_old_q16 += step;
            s->xf_left--;
        } else {
            y = tap(s, s->d_q16);
        }
        x[k] = sat16(y);
        s->d_q16 += step;
        if (!s->xf_left) {
            uint32_t d = (uint32_t)(s->d_q16 >> 16);
            if (step < 0 && d <= s->dmin) {
                uint32_t j = best_jump(s, true);
                s->d_old_q16 = s->d_q16;
                s->d_q16 += (int32_t)(j << 16);
                s->xf_left = s->xf_n;
            } else if (step > 0 && d >= s->dmax) {
                uint32_t j = best_jump(s, false);
                s->d_old_q16 = s->d_q16;
                s->d_q16 -= (int32_t)(j << 16);
                s->xf_left = s->xf_n;
            }
        }
    }
}

/* ---- robot ------------------------------------------------------------- */

/* In-place radix-2 FFT on s_re/s_im. The forward one halves at every stage
 * (output scaled by 1/N) so Q15 input can't overflow; the inverse doesn't
 * (its input is that 1/N spectrum, so it comes back at the input's scale,
 * at most 2^24). inverse also flips the twiddles' sign. */
static void fft(bool inverse)
{
    for (int i = 0; i < RB_N; i++) {
        int r = s_rev[i];
        if (r > i) {
            int32_t t = s_re[i]; s_re[i] = s_re[r]; s_re[r] = t;
            t = s_im[i]; s_im[i] = s_im[r]; s_im[r] = t;
        }
    }
    for (int len = 2; len <= RB_N; len <<= 1) {
        int half = len >> 1, tstep = RB_N / len;
        for (int i = 0; i < RB_N; i += len) {
            for (int j = 0; j < half; j++) {
                int32_t c = s_cos[j * tstep], sn = inverse ? s_sin[j * tstep] : -s_sin[j * tstep];
                int a = i + j, b = a + half;
                int32_t tr = (int32_t)(((int64_t)s_re[b] * c - (int64_t)s_im[b] * sn) >> 15);
                int32_t ti = (int32_t)(((int64_t)s_re[b] * sn + (int64_t)s_im[b] * c) >> 15);
                int32_t ur = s_re[a], ui = s_im[a];
                int sh = inverse ? 0 : 1;
                s_re[a] = (ur + tr) >> sh; s_im[a] = (ui + ti) >> sh;
                s_re[b] = (ur - tr) >> sh; s_im[b] = (ui - ti) >> sh;
            }
        }
    }
}

static uint32_t isqrt32(uint32_t v)
{
    uint32_t r = 0, bit = 1u << 30;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; }
        else r >>= 1;
        bit >>= 2;
    }
    return r;
}

/* Output level, Q8: Hann^2 overlap at hop N/4 sums to 1.5, and a
 * zero-phase frame is peakier than the voice it came from. Matched on
 * speech (RMS in ~ RMS out); the limiter keeps the pulse peaks in check. */
#define RB_GAIN_Q8 190    /* at hop 64 (scaled by hop / 64): ~2 dB under the voice it replaces */

static void robot_frame(voice_fx_state_t *s)
{
    /* Block floating point: scale the frame up to use the FFT's full 16 bits
     * (it halves at every stage), and back down after. A voice at -30 dBFS
     * otherwise comes out of the transform as a few bits of grain. */
    int32_t peak = 0;
    for (int i = 0; i < RB_N; i++) {
        int32_t v = ((int32_t)s->rb_in[i] * s_hann[i]) >> 15;
        s_re[i] = v;
        s_im[i] = 0;
        if (v < 0) v = -v;
        if (v > peak) peak = v;
    }
    if (!peak) return;
    int sh = 0;
    while (sh < 10 && (peak << (sh + 1)) <= 16383) sh++;
    for (int i = 0; i < RB_N; i++) s_re[i] <<= sh;
    fft(false);
    /* Keep each bin's magnitude, give it the fixed phase; conjugate-symmetric
     * so the inverse is real. */
    for (int k = 0; k <= RB_N / 2; k++) {
        int32_t re = s_re[k], im = s_im[k];
        int32_t m = (int32_t)isqrt32((uint32_t)(re * re) + (uint32_t)(im * im));
        int32_t yr = (m * s_ph_c[k]) >> 15, yi = (m * s_ph_s[k]) >> 15;
        s_re[k] = yr;
        s_im[k] = (k && k < RB_N / 2) ? yi : 0;
        if (k && k < RB_N / 2) { s_re[RB_N - k] = yr; s_im[RB_N - k] = -yi; }
    }
    fft(true);
    /* The chirp starts at index 0 and runs RB_SPREAD samples: put its middle
     * at the middle of the window, window again and overlap-add. */
    const int rot = RB_N / 2 - (int)s->rb_hop / 2;
    for (int i = 0; i < RB_N; i++) {
        int32_t v = s_re[(i - rot) & (RB_N - 1)] >> sh;
        s->rb_out[i] += (int32_t)((((int64_t)v * s_hann[i]) >> 15) * s->rb_gain_q8 >> 8);
    }
}

/* The buzz is sample_rate / hop; `mix` percent robot, the rest the plain
 * voice delayed by the robot's own latency (RB_N + hop / 4, measured) so the
 * two line up. */
static void robot(voice_fx_state_t *s, int16_t *x, size_t n)
{
    uint32_t hz = (uint32_t)s_param[VFX_ROBOT_HZ];
    uint32_t hop = (s->sr + hz / 2) / hz;
    if (hop < 16) hop = 16;
    if (hop > RB_N / 2) hop = RB_N / 2;
    if (hop != s->rb_hop) {
        memset(s->rb_in, 0, RB_N * sizeof(int16_t));
        memset(s->rb_out, 0, RB_N * sizeof(int32_t));
        s->rb_h = 0;
        s->rb_hop = hop;
        s->rb_gain_q8 = (int32_t)((RB_GAIN_Q8 * hop + 32) / 64);
    }
    robot_phase_table(hop);
    /* Equal-power blend: the robot and the plain voice don't add in phase. */
    const int32_t mix = s_param[VFX_ROBOT_MIX];
    const int32_t wq = (int32_t)lrintf(32767.0f * sinf(1.5707963f * (float)mix / 100.0f));
    const int32_t dq = (int32_t)lrintf(32767.0f * cosf(1.5707963f * (float)mix / 100.0f));
    for (size_t k = 0; k < n; k++) {
        s->buf[s->w & s->mask] = x[k];
        int32_t dry = s->buf[(s->w - RB_N - hop / 4) & s->mask];
        s->w++;
        s->rb_in[RB_N - hop + s->rb_h] = x[k];
        int32_t wet = s->rb_out[s->rb_h];
        x[k] = sat16(mix >= 100 ? wet : (int32_t)(((int64_t)wet * wq + (int64_t)dry * dq) >> 15));
        if (++s->rb_h == hop) {
            s->rb_h = 0;
            memmove(s->rb_out, s->rb_out + hop, (RB_N - hop) * sizeof(int32_t));
            memset(s->rb_out + RB_N - hop, 0, hop * sizeof(int32_t));
            robot_frame(s);
            memmove(s->rb_in, s->rb_in + hop, (RB_N - hop) * sizeof(int16_t));
        }
    }
}

/* ---- simple effects ---------------------------------------------------- */

/* Ring modulation: the voice times a sine at `hz`, blended `mix`% with the
 * plain voice: gain = dry + wet sin. Scaled to the same average power at
 * every mix, 1 dB under the plain voice (fully wet: a sine at 1.25x, where
 * multiplying by a sine alone would halve the power). */
static void dalek(voice_fx_state_t *s, int16_t *x, size_t n)
{
    uint32_t step = (uint32_t)(((uint64_t)(uint32_t)s_param[VFX_DALEK_HZ] << 32) / s->sr);
    float m = (float)s_param[VFX_DALEK_MIX] / 100.0f, wet = m, dry = 1.0f - m;
    float r = 0.884f / sqrtf(dry * dry + wet * wet / 2.0f);
    int32_t wet_q15 = (int32_t)lrintf(32768.0f * wet * r), dry_q15 = (int32_t)lrintf(32768.0f * dry * r);
    for (size_t k = 0; k < n; k++) {
        int32_t g = dry_q15 + ((wet_q15 * s_sine[s->ring_phase >> 24]) >> 15);   /* Q15 */
        x[k] = sat16((int32_t)(((int64_t)x[k] * g) >> 15));
        s->ring_phase += step;
    }
}

/* Volume swung between 1 and 1 - depth, `hz` times a second, then scaled so
 * the average power matches the plain voice: gain = (a - b sin) / rms with
 * a = 1 - depth / 2, b = depth / 2, rms = sqrt(a^2 + b^2 / 2). */
static void tremolo(voice_fx_state_t *s, int16_t *x, size_t n)
{
    uint32_t step = (uint32_t)(((uint64_t)(uint32_t)s_param[VFX_TREM_HZ] << 32) / s->sr);
    float d = (float)s_param[VFX_TREM_DEPTH] / 100.0f, a = 1.0f - d / 2.0f, b = d / 2.0f;
    float r = 1.0f / sqrtf(a * a + b * b / 2.0f);
    int32_t a_q15 = (int32_t)lrintf(32768.0f * a * r), b_q15 = (int32_t)lrintf(32768.0f * b * r);
    for (size_t k = 0; k < n; k++) {
        int32_t g = a_q15 - ((b_q15 * s_sine[s->ring_phase >> 24]) >> 15);
        x[k] = sat16((int32_t)(((int64_t)x[k] * g) >> 15));
        s->ring_phase += step;
    }
}

/* Last stage of every effect: bend anything above 20000 over to a slope of
 * 1/4 (so 32767 comes out at ~23200). The effects are level-matched to plain
 * speech, but the phone line's audio input starts distorting well before the
 * codec's full scale, so a rare loud peak shouldn't get through at full size. */
static volatile int s_gain_pct = 100;

void voice_fx_set_gain_pct(int pct) { s_gain_pct = pct < 10 ? 10 : (pct > 300 ? 300 : pct); }
int voice_fx_gain_pct(void) { return s_gain_pct; }

static uint32_t limit(int16_t *x, size_t n)
{
    uint32_t hit = 0;
    for (size_t k = 0; k < n; k++) {
        int32_t v = x[k], a = v < 0 ? -v : v;
        if (a > 20000) {
            a = 20000 + ((a - 20000) >> 2);
            x[k] = (int16_t)(v < 0 ? -a : a);
            hit++;
        }
    }
    return hit;
}

static void measure(const int16_t *x, size_t n, int32_t *peak, uint64_t *sq)
{
    for (size_t k = 0; k < n; k++) {
        int32_t a = x[k] < 0 ? -x[k] : x[k];
        if (a > *peak) *peak = a;
        *sq += (uint64_t)(a * a);
    }
}

void voice_fx_process(voice_fx_state_t *s, voice_fx_t fx, int16_t *x, size_t n)
{
    if (!s || fx == VOICE_FX_NONE) return;
    if (fx != s->last) {
        reset(s);
        s->last = fx;
    }
    measure(x, n, &s->st_in_peak, &s->st_in_sq);
    switch (fx) {
    case VOICE_FX_DALEK:
        dalek(s, x, n);
        break;
    case VOICE_FX_CHIPMUNK: {
        int pct = s_param[VFX_CHIP_PCT];
        if (pct != s->aa_pct) antialias_design(s, pct);
        pitch(s, (int32_t)((pct * 65536 + 50) / 100), x, n);
        for (size_t k = 0; k < n; k++) x[k] = (int16_t)((x[k] * 26028) >> 15);   /* -2 dB */
        break;
    }
    case VOICE_FX_ROBOT:
        robot(s, x, n);
        break;
    case VOICE_FX_TREMOLO:
        tremolo(s, x, n);
        break;
    default:
        break;
    }
    int g = s_gain_pct;
    if (g != 100) {
        for (size_t k = 0; k < n; k++) x[k] = sat16((x[k] * g) / 100);
    }
    s->st_limited += limit(x, n);
    measure(x, n, &s->st_out_peak, &s->st_out_sq);
    s->st_n += n;
}

void voice_fx_take_stats(voice_fx_state_t *s, voice_fx_stats_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!s) return;
    out->fx = s->last;
    out->samples = s->st_n;
    out->in_peak = s->st_in_peak;
    out->out_peak = s->st_out_peak;
    out->limited = s->st_limited;
    if (s->st_n) {
        out->in_rms = (uint32_t)sqrt((double)s->st_in_sq / s->st_n);
        out->out_rms = (uint32_t)sqrt((double)s->st_out_sq / s->st_n);
    }
    s->st_n = s->st_limited = 0;
    s->st_in_peak = s->st_out_peak = 0;
    s->st_in_sq = s->st_out_sq = 0;
}

const char *voice_fx_name(voice_fx_t fx)
{
    switch (fx) {
    case VOICE_FX_DALEK:    return "Dalek";
    case VOICE_FX_CHIPMUNK: return "chipmunk";
    case VOICE_FX_ROBOT:    return "robot";
    case VOICE_FX_TREMOLO:  return "tremolo";
    default:                return "normal";
    }
}

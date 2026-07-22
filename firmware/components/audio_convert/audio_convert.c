#include "audio_convert.h"
#include <string.h>
#include <math.h>

static uint32_t rd_u32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t rd_u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

bool audio_wav_parse(const uint8_t *buf, size_t len, audio_wav_info_t *out)
{
    if (len < 12 || memcmp(buf, "RIFF", 4) != 0 || memcmp(buf + 8, "WAVE", 4) != 0) return false;
    memset(out, 0, sizeof(*out));
    size_t off = 12;
    bool have_fmt = false, have_data = false;
    while (off + 8 <= len) {
        uint32_t sz = rd_u32(buf + off + 4);
        size_t body = off + 8;
        if (memcmp(buf + off, "fmt ", 4) == 0 && body + 16 <= len) {
            out->audio_format    = rd_u16(buf + body + 0);
            out->channels        = rd_u16(buf + body + 2);
            out->sample_rate     = rd_u32(buf + body + 4);
            out->bits_per_sample = rd_u16(buf + body + 14);
            have_fmt = true;
        } else if (memcmp(buf + off, "data", 4) == 0) {
            out->data_offset = (uint32_t)body;
            out->data_bytes = sz;
            have_data = true;
        }
        off = body + sz + (sz & 1); /* chunks are word-aligned */
        if (have_fmt && have_data) break;
    }
    return have_fmt && have_data;
}

void audio_resampler_init(audio_resampler_t *r, uint32_t in_rate, uint32_t out_rate)
{
    r->ratio = (double)in_rate / (double)out_rate;
    r->phase = 0.0;
    r->prev = 0;
    r->primed = false;
}

size_t audio_resampler_max_out(const audio_resampler_t *r, size_t n_in)
{
    return (size_t)((double)n_in / r->ratio) + 4;
}

size_t audio_resampler_push(audio_resampler_t *r, const int16_t *in, size_t n_in,
                              int16_t *out, size_t out_cap)
{
    size_t out_n = 0;
    for (size_t i = 0; i < n_in; i++) {
        int16_t cur = in[i];
        if (!r->primed) {
            r->prev = cur;
            r->primed = true;
            continue;
        }
        while (r->phase < 1.0 && out_n < out_cap) {
            double interp = r->prev + r->phase * (double)(cur - r->prev);
            out[out_n++] = (int16_t)interp;
            r->phase += r->ratio;
        }
        r->phase -= 1.0;
        r->prev = cur;
    }
    return out_n;
}

void audio_convert_init(audio_convert_ctx_t *ctx, const audio_convert_fmt_t *fmt)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->fmt = *fmt;
    /* One-pole RC low-pass: alpha = dt/(RC+dt), RC = 1/(2*pi*fc). Telephony
     * voice band (~3.4kHz) -- always applied, not just when decimating:
     * it's also just what phone-quality audio is supposed to sound like. */
    const double fc = 3400.0;
    double dt = 1.0 / (double)fmt->in_rate;
    double rc = 1.0 / (2.0 * M_PI * fc);
    ctx->lpf_alpha = dt / (rc + dt);
    ctx->lpf_prev = 0.0;
    audio_resampler_init(&ctx->rs, fmt->in_rate, AUDIO_CONVERT_OUT_RATE);
}

static int32_t rd_le(const uint8_t *p, int nbytes)
{
    int32_t v = 0;
    for (int i = 0; i < nbytes; i++) v |= ((int32_t)p[i]) << (8 * i);
    return v;
}

/* Reads one channel sample from a frame and normalizes it to int16 range,
 * regardless of source bit depth/float-ness. */
static int32_t sample_as_int16(const audio_convert_fmt_t *fmt, const uint8_t *sp)
{
    if (fmt->in_float && fmt->in_bits == 32) {
        float f;
        memcpy(&f, sp, 4);
        double d = (double)f * 32767.0;
        if (d > 32767.0) d = 32767.0;
        if (d < -32768.0) d = -32768.0;
        return (int32_t)d;
    }
    switch (fmt->in_bits) {
        case 8:
            return ((int32_t)sp[0] - 128) * 256; /* unsigned 8-bit -> signed 16-bit range */
        case 16:
            return (int16_t)rd_le(sp, 2);
        case 24: {
            int32_t v = rd_le(sp, 3);
            if (v & 0x800000) v |= (int32_t)0xFF000000; /* sign-extend 24 -> 32 */
            return v >> 8; /* 24-bit range -> 16-bit range */
        }
        default: { /* 32-bit int */
            int32_t v = rd_le(sp, 4);
            return v >> 16; /* 32-bit range -> 16-bit range */
        }
    }
}

/* Downmix all channels of one frame to a single normalized int16 sample. */
static int16_t normalize_and_downmix_frame(const audio_convert_fmt_t *fmt, const uint8_t *frame)
{
    int bytes_per_sample = fmt->in_bits / 8;
    int32_t sum = 0;
    for (int ch = 0; ch < fmt->in_channels; ch++) {
        sum += sample_as_int16(fmt, frame + ch * bytes_per_sample);
    }
    return (int16_t)(sum / fmt->in_channels);
}

static int16_t lpf_step(audio_convert_ctx_t *ctx, int16_t sample)
{
    ctx->lpf_prev += ctx->lpf_alpha * ((double)sample - ctx->lpf_prev);
    return (int16_t)ctx->lpf_prev;
}

static size_t process_frame(audio_convert_ctx_t *ctx, const uint8_t *frame,
                              int16_t *out, size_t out_cap)
{
    int16_t mono = normalize_and_downmix_frame(&ctx->fmt, frame);
    int16_t filtered = lpf_step(ctx, mono);
    return audio_resampler_push(&ctx->rs, &filtered, 1, out, out_cap);
}

size_t audio_convert_push(audio_convert_ctx_t *ctx, const void *in_bytes, size_t in_len,
                           int16_t *out, size_t out_cap_samples)
{
    size_t frame_bytes = (size_t)ctx->fmt.in_channels * (ctx->fmt.in_bits / 8);
    const uint8_t *p = (const uint8_t *)in_bytes;
    size_t remaining = in_len;
    size_t out_n = 0;

    /* Complete a partial frame left over from the previous call, if any. */
    if (ctx->leftover_len > 0) {
        size_t need = frame_bytes - ctx->leftover_len;
        size_t take = (remaining < need) ? remaining : need;
        memcpy(ctx->leftover + ctx->leftover_len, p, take);
        ctx->leftover_len += take;
        p += take;
        remaining -= take;
        if (ctx->leftover_len < frame_bytes) {
            return 0; /* still waiting on more bytes to complete even one frame */
        }
        out_n += process_frame(ctx, ctx->leftover, out + out_n, out_cap_samples - out_n);
        ctx->leftover_len = 0;
    }

    size_t n_frames = remaining / frame_bytes;
    for (size_t i = 0; i < n_frames && out_n < out_cap_samples; i++) {
        out_n += process_frame(ctx, p + i * frame_bytes, out + out_n, out_cap_samples - out_n);
    }
    size_t used = n_frames * frame_bytes;
    p += used;
    remaining -= used;

    if (remaining > 0) { /* stash the trailing partial frame for next call */
        memcpy(ctx->leftover, p, remaining);
        ctx->leftover_len = remaining;
    }
    return out_n;
}

size_t audio_convert_max_out(const audio_convert_ctx_t *ctx, size_t in_len)
{
    size_t frame_bytes = (size_t)ctx->fmt.in_channels * (ctx->fmt.in_bits / 8);
    size_t n_frames = (in_len + ctx->leftover_len) / frame_bytes + 1;
    return audio_resampler_max_out(&ctx->rs, n_frames);
}

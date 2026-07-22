/*
 * audio_convert.h — WAV header parsing, sample-rate resampling, and
 * upload-time format conversion (arbitrary WAV PCM -> 8kHz mono 16-bit PCM).
 *
 * Split out of `player` so both playback (which only ever needed the
 * upsample-only subset) and the web UI's audio uploader (which needs the
 * full bit-depth/downmix/anti-alias-then-decimate pipeline) share one
 * implementation instead of two copies drifting apart.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t audio_format;   /* 1 = PCM int, 3 = IEEE float (standard WAV fmt tags) */
    uint16_t channels;
    uint32_t sample_rate;
    uint16_t bits_per_sample;
    uint32_t data_offset;
    uint32_t data_bytes;
} audio_wav_info_t;

/* Tolerant RIFF/WAVE chunk walker -- finds `fmt `/`data`, skips unknown
 * chunks in between (e.g. macOS afconvert's `FLLR` filler). `buf`/`len`
 * should cover at least the file's header (a few KB is normally enough --
 * see HEADER_SCAN_BYTES-style callers). Returns false if it isn't a WAV or
 * the header wasn't fully within `len`. */
bool audio_wav_parse(const uint8_t *buf, size_t len, audio_wav_info_t *out);

/* Streaming linear-interpolation resampler -- state persists across
 * pushes so chunk boundaries don't click. No anti-aliasing filter: fine
 * alone for upsampling (e.g. an 8kHz clip -> the device's 48kHz engine
 * rate) or mild rate mismatches, NOT for heavy decimation -- see
 * audio_convert_ctx_t below, which adds a low-pass stage before this for
 * that case. */
typedef struct {
    double  ratio;  /* in_rate / out_rate; <1 upsamples, >1 downsamples */
    double  phase;
    int16_t prev;
    bool    primed;
} audio_resampler_t;

void   audio_resampler_init(audio_resampler_t *r, uint32_t in_rate, uint32_t out_rate);
size_t audio_resampler_max_out(const audio_resampler_t *r, size_t n_in);
size_t audio_resampler_push(audio_resampler_t *r, const int16_t *in, size_t n_in,
                             int16_t *out, size_t out_cap);

/* ---- Upload-time conversion: arbitrary WAV PCM -> 8kHz mono 16-bit PCM ----
 * Streaming/chunked throughout -- never buffers a whole file (this hardware
 * has failed a single ~330KB malloc before; nothing here should ever need
 * more than a few KB at a time). Per input frame: normalize to a common
 * 16-bit-range sample regardless of source bit depth (8-bit unsigned /
 * 16-bit / 24-bit / 32-bit int / 32-bit IEEE float) -> downmix all channels
 * (average) -> one-pole anti-alias low-pass (telephony band, ~3.4kHz
 * cutoff) -> feed the existing resampler to land on exactly 8000 Hz. */
#define AUDIO_CONVERT_OUT_RATE 8000
#define AUDIO_CONVERT_MAX_FRAME_BYTES 32 /* generous: 8 channels x 32-bit */

typedef struct {
    uint32_t in_rate;
    uint16_t in_channels;
    uint16_t in_bits;   /* 8, 16, 24, or 32 */
    bool     in_float;  /* true only for 32-bit IEEE float (WAV fmt tag 3) */
} audio_convert_fmt_t;

typedef struct {
    audio_convert_fmt_t fmt;
    double            lpf_alpha; /* one-pole low-pass coefficient, derived from in_rate */
    double            lpf_prev;  /* filter state, persists across push() calls */
    audio_resampler_t rs;
    uint8_t           leftover[AUDIO_CONVERT_MAX_FRAME_BYTES]; /* partial frame straddling two pushes */
    size_t            leftover_len;
} audio_convert_ctx_t;

void audio_convert_init(audio_convert_ctx_t *ctx, const audio_convert_fmt_t *fmt);

/* Feeds raw interleaved input bytes in the source format; writes converted
 * 8kHz mono int16 samples to `out` (capacity `out_cap_samples` samples),
 * returns how many were written. May legitimately write fewer than
 * `audio_convert_max_out()` on any given call (a partial frame straddling
 * this call and the next is buffered internally) -- just keep feeding
 * sequential chunks of the same stream. */
size_t audio_convert_push(audio_convert_ctx_t *ctx, const void *in_bytes, size_t in_len,
                           int16_t *out, size_t out_cap_samples);

/* Upper bound on samples audio_convert_push() could emit for `in_len` bytes
 * of input, for sizing the caller's `out` buffer. */
size_t audio_convert_max_out(const audio_convert_ctx_t *ctx, size_t in_len);

#ifdef __cplusplus
}
#endif

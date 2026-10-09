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

#include "sdkconfig.h"
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
 * the full amount on any given call (a partial frame straddling
 * this call and the next is buffered internally) -- just keep feeding
 * sequential chunks of the same stream. */
size_t audio_convert_push(audio_convert_ctx_t *ctx, const void *in_bytes, size_t in_len,
                           int16_t *out, size_t out_cap_samples);

/* ---- MP3 decode support ----
 * Declared/usable when either CONFIG_AUDIO_CONVERT_MP3 (webui.c's
 * browser-upload decode, "trial") or CONFIG_PLAYER_MP3 (player.c's
 * playback decode -- see that component's Kconfig for why this is a
 * separate option with a very different RAM-cost profile) is on. Vendors
 * minimp3 (CC0, third_party/minimp3/) in a separate translation unit
 * (audio_convert_mp3.c) added to the build only when needed, so the
 * flash-size cost can be measured by toggling both off and rebuilding.
 * Pure decode logic here, no HTTP/multipart/storage awareness -- callers
 * own buffering compressed bytes and feeding them in. */
#if defined(CONFIG_AUDIO_CONVERT_MP3) || defined(CONFIG_PLAYER_MP3)
#define AUDIO_CONVERT_MP3_MAX_SAMPLES (1152 * 2) /* minimp3's MINIMP3_MAX_SAMPLES_PER_FRAME */

typedef struct audio_mp3_decoder audio_mp3_decoder_t; /* opaque */

/* Heap-allocated (the decoder's internal state is a few KB -- too big for
 * a stack local given this project's history with large stack arrays
 * under the httpd task's 8KB stack). NULL on allocation failure. */
audio_mp3_decoder_t *audio_mp3_decoder_create(void);
void                 audio_mp3_decoder_destroy(audio_mp3_decoder_t *dec);

/* True if `buf` (the first handful of bytes of an upload) looks like an
 * MP3 -- an ID3v2 tag, or a raw frame sync. Checked before falling back
 * to WAV parsing. */
bool audio_convert_mp3_magic(const uint8_t *buf, size_t len);

/* Decodes one frame from `in` (up to `in_len` bytes buffered -- caller's
 * job to keep enough buffered; a real frame is at most a couple KB).
 * *consumed is always <= in_len: 0 means not enough bytes were buffered
 * for even one frame, caller must top up input and retry with the same
 * (uncommitted) bytes still at the front. A non-zero *consumed with 0
 * returned samples means a skipped ID3/garbage region -- just advance and
 * keep calling. `out` must hold AUDIO_CONVERT_MP3_MAX_SAMPLES samples.
 * Returns samples-per-channel decoded (interleaved in `out` by
 * *out_channels if stereo). */
int audio_mp3_decode_frame(audio_mp3_decoder_t *dec, const uint8_t *in, size_t in_len,
                            int16_t *out, int *out_channels, uint32_t *out_rate,
                            size_t *consumed);
#endif /* CONFIG_AUDIO_CONVERT_MP3 || CONFIG_PLAYER_MP3 */

#ifdef __cplusplus
}
#endif

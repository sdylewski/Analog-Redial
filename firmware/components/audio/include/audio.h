/*
 * audio.h — I2S engine + per-line audio routing for the WM8731.
 *
 * The codec runs as an I2S slave at a fixed rate (default 48 kHz, 16-bit,
 * stereo). LEFT channel = Line 0, RIGHT channel = Line 1.
 *
 * Each line has ONE transmit source at a time (what the caller hears) and a
 * receive tap (what the caller says). Sources:
 *   - silence
 *   - a call-progress tone (dial/ringback/busy...)
 *   - a DTMF digit burst
 *   - a bridge from the other line (local inter-line call)
 *   - an external PCM stream (VoIP / file playback) via a stream buffer
 *
 * VoIP seam:
 *   audio_ext_write(line, ...)   feed downlink PCM (engine rate) toward phone
 *   audio_rx_read(line, ...)     pull uplink PCM (engine rate) from phone mic
 * Resampling to 8 kHz for G.711 is done in the `voip` component.
 */
#pragma once

#include "esp_err.h"
#include "tones.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_NUM_LINES 2

typedef struct {
    uint32_t sample_rate;   /* default/recommended 48000 (keeps codec MCLK=12.288MHz) */
    size_t   frame_samples; /* per-channel samples per I2S transfer; 0 => sample_rate/100 */
} audio_config_t;

#define AUDIO_DEFAULT_CONFIG() (audio_config_t){ .sample_rate = 48000, .frame_samples = 0 }

/* Optional per-line receive tap, called from the audio task with mono PCM. */
typedef void (*audio_rx_tap_fn)(int line, const int16_t *mono, size_t n, void *ctx);

esp_err_t audio_init(const audio_config_t *cfg);
uint32_t  audio_sample_rate(void);
size_t    audio_frame_samples(void);

/* ---- transmit source selection (mutually exclusive per line) ---- */
void audio_tx_silence(int line);
void audio_tx_tone(int line, call_progress_t tone);
void audio_tx_dtmf(int line, char key, uint32_t ms);     /* one digit burst then silence */
/* Arbitrary dual-tone burst (f2=0 for single tone), then silence. Bring-up/test. */
void audio_tx_burst(int line, float f1, float f2, float amp, uint32_t ms);
void audio_tx_bridge(int line, int from_line, bool enable); /* hear `from_line` */
void audio_tx_external(int line, bool enable);           /* play from audio_ext_write() */

/* ---- external PCM (VoIP / file) ---- */
/* Push engine-rate mono PCM to a line's external buffer. Returns bytes queued. */
size_t audio_ext_write(int line, const int16_t *mono, size_t n);

/* ---- receive ---- */
void   audio_set_rx_tap(int line, audio_rx_tap_fn fn, void *ctx);
/* Pull engine-rate mono PCM captured from a line's mic (uplink). BACKED BY A
 * SINGLE-READER STREAM BUFFER: exactly one consumer may block on this per
 * line at a time (this is what the VoIP RTP tx task uses while a call is
 * up). Calling this concurrently from a second task (e.g. a console 'record'
 * command) on a line that's already mid-call WILL crash -- FreeRTOS asserts
 * on a second task blocking to receive on the same stream buffer. For a
 * diagnostic capture that needs to run *alongside* whatever's already
 * consuming this line (the whole point of most echo/audio debugging), use
 * audio_add_diag_tap() instead -- a synchronous callback, not a second
 * stream-buffer reader, so it can't collide with the RTP task. */
size_t audio_rx_read(int line, int16_t *mono, size_t n, uint32_t timeout_ms);

/* Second, independent RX tap slot reserved for diagnostics (console 'record',
 * echo debugging tools, etc.) -- separate from audio_set_rx_tap()'s slot,
 * which telephony.c owns permanently for DTMF detection. Coexists safely
 * with an active call: both taps and the rx stream buffer are fed from the
 * same synchronous point in the audio task, so a diag tap never competes
 * with the RTP task's audio_rx_read(). Callback runs on the audio task --
 * keep it fast, no blocking/malloc/logging. Only one diag tap per line;
 * audio_add_diag_tap() replaces whatever was previously registered. */
void audio_add_diag_tap(int line, audio_rx_tap_fn fn, void *ctx);
void audio_remove_diag_tap(int line);

/* Simple RMS level (0..32767) of the most recent RX frame for a line. */
uint16_t audio_rx_level(int line);

/* ---- echo-cancelled uplink (8 kHz) ----
 * Only produced while the canceller is enabled for the line (see aec.h). The
 * engine already has both signals the canceller needs -- the frame about to
 * be played and the frame just captured -- aligned in one audio_task pass, so
 * cancellation happens there and the result is published here at 8 kHz, ready
 * for G.711 without the caller re-decimating.
 *
 * The 48 kHz audio_rx_read() path is deliberately left carrying the RAW mic
 * signal: recording and echoprobe have to keep measuring the hardware, not
 * the canceller's output. */
#define AUDIO_AEC_BLOCK 80 /* 10 ms at 8 kHz -- one engine frame decimated 6:1 */
size_t audio_aec_read(int line, int16_t *mono, size_t n, uint32_t timeout_ms);

/* Test-only digital loopback: feed the synthesized TX back into the RX
 * processing chain (level meter / DTMF detector / rx read) so automated
 * tests can confirm a played tone/DTMF/clip is actually produced and
 * detectable without a physical handset. DSP-domain only -- does not
 * exercise the analog codec path. */
void audio_set_test_loopback(bool on);
bool audio_test_loopback(void);

/* ---- Echo probe (local earpiece->mic path measurement) ----
 * Plays a short burst out a line's own earpiece and listens on that same
 * line's own mic, reporting where the burst comes back (bulk delay) and how
 * long it takes to decay (tail) -- the two numbers that size an echo
 * canceller (see firmware/components/aec/). This, not a live call's
 * round-trip, is what actually needs measuring: an AEC only ever models its
 * own device's earpiece->mic path, never the far end's. Run off-hook and
 * NOT in a call, so the only thing the mic can hear is this board's own
 * earpiece. */
typedef struct {
    bool     leak_detected; /* peak cleared 4x the ambient noise floor */
    uint32_t noise_rms;
    int32_t  peak;
    double   delay_ms;      /* time from burst start to the return peak */
    double   tail_ms;       /* time from the peak until it decays back down */
} audio_echo_probe_t;

/* `cap_out`/`cap_out_n` are optional (pass NULL/0 to skip) -- if given, the
 * raw capture is also copied there (e.g. for plotting the envelope), up to
 * cap_out_n samples. Returns false only on a hard failure (no audio
 * captured at all, alloc/stream-buffer failure) -- a clean "no leak" result
 * still returns true, check out->leak_detected. */
bool audio_echo_probe(int line, uint32_t burst_hz, uint32_t burst_ms,
                       uint32_t listen_ms, int16_t *cap_out, size_t cap_out_n,
                       audio_echo_probe_t *out);

#ifdef __cplusplus
}
#endif

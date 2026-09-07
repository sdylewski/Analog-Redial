/*
 * recorder.h — captures a phone line's mic (uplink) audio to a mono 16-bit
 * 8 kHz WAV in the storage layer.
 *
 * The mirror image of the `player` component: one background task per active
 * recording, built on the `audio` component's RX seam (audio_rx_read()). The
 * engine runs at 48 kHz; this decimates to 8 kHz (~6x smaller, still full
 * phone bandwidth) so a few seconds of speech fits the small flash partition.
 * player.c resamples 8 kHz back up on playback, so recordings play at the
 * right pitch.
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Fires from the recorder task when capture ends (max time, silence, or
 * recorder_stop()). `ok` is true if any audio was actually written. Keep it
 * short -- like player's done_cb, just enqueue to your own FSM. */
typedef void (*recorder_done_cb_t)(int line, bool ok, void *ctx);

/*
 * Start capturing `line` to `storage_name` (mono 16-bit 8 kHz WAV). Returns
 * immediately; recording runs in the background until the FIRST of:
 *   - max_ms elapsed,
 *   - ~silence_ms of continuous quiet after the first ~1 s (0 = disable), or
 *   - recorder_stop(line) (e.g. the caller hung up).
 *
 *   ESP_OK                  -- started
 *   ESP_ERR_INVALID_ARG     -- bad line
 *   ESP_ERR_INVALID_STATE   -- already recording that line
 *   ESP_ERR_NO_MEM / ESP_FAIL -- couldn't start the task
 */
esp_err_t recorder_start(int line, const char *storage_name, uint32_t max_ms,
                         uint32_t silence_ms, recorder_done_cb_t done_cb, void *ctx);

/* Same as recorder_start(), but captures the DOWNLINK side (decoded VoIP RTP
 * for a real call, via audio_add_extwrite_tap() -- see that function's doc
 * comment in audio.h for why not audio_add_downlink_tap()) instead of the
 * local mic. For the answering machine recording a caller's message on an
 * auto-answered VoIP call: the caller's voice arrives over the network, not
 * this line's own mic, which would just capture room silence. Not for
 * local-peer calls -- there the caller's voice really is on their own
 * line's mic, so recorder_start() on THAT line is what's used instead. See
 * archive/ANSWERING_MACHINE.md. */
esp_err_t recorder_start_downlink(int line, const char *storage_name, uint32_t max_ms,
                                  uint32_t silence_ms, recorder_done_cb_t done_cb, void *ctx);

/* Ask the recording on `line` to finish and flush (done_cb still fires). Safe
 * to call when nothing is recording. */
void recorder_stop(int line);

bool recorder_is_active(int line);

#ifdef __cplusplus
}
#endif

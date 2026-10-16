/*
 * player.h — plays a mono 16-bit PCM WAV file from the storage layer out a
 * phone line's speaker.
 *
 * Built entirely on the `audio` component's existing external-source seam
 * (audio_tx_external()/audio_ext_write()) -- the same path the VoIP RTP
 * receiver uses -- so it doesn't touch audio.c at all. One background task
 * per active playback; exits on completion or player_stop().
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*player_done_cb_t)(int line, void *ctx);

/*
 * Plays `storage_name` (must already exist in the storage layer, see
 * storage.h) out `line`'s speaker. The WAV must be mono 16-bit PCM at the
 * device's sample rate (audio_sample_rate()) -- no resampling is done; a
 * mismatched rate still plays, just at the wrong pitch/speed (logged as a
 * warning).
 *
 * Returns immediately:
 *   ESP_OK                  -- playback started in the background
 *   ESP_ERR_NOT_FOUND       -- no such file in storage
 *   ESP_ERR_INVALID_STATE   -- `line` is already playing something
 *   ESP_ERR_INVALID_ARG     -- bad line index
 *   ESP_ERR_NO_MEM / ESP_FAIL -- couldn't start the playback task
 *
 * `done_cb` (optional) fires once playback finishes normally, from the
 * playback task's context -- keep it short. Not called if player_stop()
 * cuts playback short.
 */
esp_err_t player_play(int line, const char *storage_name, player_done_cb_t done_cb, void *ctx);

/* Like player_play(), for a clip that is followed by another started right
 * away (reading digits aloud): it ends as soon as its audio is queued, without
 * the short wait and the silencing that let a final clip play out. The caller
 * must play the last clip with player_play(), or silence the line itself.
 * (A WAV; an MP3 plays as with player_play().) */
esp_err_t player_play_chained(int line, const char *storage_name, player_done_cb_t done_cb, void *ctx);

/* Stops playback on `line` if active (e.g. on hangup mid-clip). done_cb is
 * NOT invoked. Safe to call even if nothing is playing. */
void player_stop(int line);

/* player_stop(), then waits up to `wait_ms` for the playback task to finish, so
 * a new player_play() on the line right after won't see it busy. */
void player_stop_and_wait(int line, uint32_t wait_ms);


#ifdef __cplusplus
}
#endif

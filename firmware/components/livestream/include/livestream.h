/*
 * livestream.h -- a phone line's call audio, live, over the network, for
 * recording a video with OBS (or listening with VLC, ffmpeg, a browser).
 *
 * Off by default, and off again after a restart. Turned on (Settings ->
 * Diagnostics), the board listens on port LIVESTREAM_PORT for
 *
 *   GET /live.wav?key=<key>[&line=1][&stereo=1]
 *
 * and answers with an endless 16 kHz, 16-bit WAV: by default mono, the
 * line's own mic mixed at half level with what its earpiece hears (both voices of a
 * call, whatever the other end is: the other line, VoIP, another board);
 * with stereo=1, left = what the phone sends, right = what the earpiece
 * hears, as in a call recording (callcap.h). Between calls it's silence,
 * dial tone and so on, in real time, so a player never runs dry.
 *
 * One listener at a time: a new connection replaces the old one (OBS
 * reconnecting after a scene change). The key is random, kept in NVS so the
 * link stays the same across restarts, and changed with livestream_new_key().
 *
 * The audio comes from the line's diagnostic and downlink taps (audio.h),
 * which a call recording also uses: while one is recording that line, the
 * stream is silence.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LIVESTREAM_PORT 8081
#define LIVESTREAM_KEY_LEN 16   /* hex characters */

/* Starts the listening task on the first call with on=true. */
esp_err_t livestream_set_enabled(bool on);
bool      livestream_enabled(void);

/* The current key ("" before NVS is readable). */
void      livestream_key(char *out, size_t len);
/* A new random key: the old link stops working, and a listener is dropped. */
void      livestream_new_key(void);

typedef struct {
    bool     streaming;          /* a listener is connected */
    char     client[16];         /* its IPv4 address */
    int      line;
    bool     stereo;
    uint32_t secs;               /* how long it has been connected */
    uint32_t dropped_ms;         /* audio lost because the network was slow */
} livestream_status_t;

livestream_status_t livestream_status(void);

#ifdef __cplusplus
}
#endif

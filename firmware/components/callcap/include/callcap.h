/*
 * callcap.h -- record everything one call sends and hears, as a stereo WAV:
 * "call record YYMMDD HHMMSS.wav", to the microSD card when one is mounted,
 * otherwise to internal flash (so a two-line board, which has no card, can
 * record a demo too).
 *
 * One-shot test mode for making demos and bug reports. Arm it (Settings ->
 * Diagnostics), then lift a handset: recording starts at pickup and stops at
 * hang-up, and the mode switches itself off again.
 *
 *   left channel   what the phone sends: your voice, and the DTMF tones of
 *                  touch-tone dialing
 *   right channel  what the earpiece hears: dial tone, ringback, busy, clips,
 *                  and the other party's voice
 *
 * Not on it, because they aren't audio on the line: the bell ringing (the
 * SLIC makes ring voltage, not sound; record that with a microphone), and
 * the click timing of rotary pulses unless the SLIC passes them through.
 *
 * To the card: 16 kHz, 16-bit (about 64 KB/s, 3.8 MB a minute), capped at 20
 * minutes. To internal flash: 8 kHz (32 KB/s, 1.9 MB a minute; the line has
 * nothing above 3.4 kHz anyway), using only the free space, so nothing is
 * evicted for it: a minute or two. The file isn't a recording the flash
 * evicts either (only rec_* and voicemail_* are): delete it when done.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CALLCAP_IDLE = 0,
    CALLCAP_ARMED,       /* waiting for the next pickup */
    CALLCAP_RECORDING,
    CALLCAP_SAVING,      /* handset is down; finishing the file */
} callcap_state_t;

/* force_local records to internal flash even with a card mounted.
 * ESP_ERR_NO_MEM: internal flash has under 20 s free (and no card, or
 * force_local). ESP_ERR_INVALID_STATE: already armed or recording. */
esp_err_t callcap_arm(bool force_local);
/* Seconds of recording internal flash has room for now. */
unsigned callcap_local_secs_free(void);
/* Armed: disarms. Recording: stops and saves what was captured so far. */
void callcap_cancel(void);

/* Called by telephony on every real hook change (not rotary pulses). Cheap
 * and non-blocking; the file work happens on the capture task. */
void callcap_hook(int line, bool offhook);

callcap_state_t callcap_state(void);
unsigned callcap_elapsed_secs(void);       /* while recording */
/* The most recent finished recording this boot ("" if none). */
const char *callcap_last_name(void);
unsigned    callcap_last_secs(void);
unsigned    callcap_last_dropped_ms(void); /* audio lost to slow writes */

#ifdef __cplusplus
}
#endif

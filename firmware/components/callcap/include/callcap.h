/*
 * callcap.h -- record everything one call sends and hears, to the microSD
 * card, as a stereo WAV: "call record YYMMDD HHMMSS.wav".
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
 * 16 kHz, 16-bit (about 64 KB/s, 3.8 MB a minute), capped at 20 minutes. The
 * card is required: the internal flash is only 4 MB.
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

/* ESP_ERR_NOT_FOUND: no microSD card mounted. ESP_ERR_INVALID_STATE: already
 * armed or recording. */
esp_err_t callcap_arm(void);
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
unsigned    callcap_last_dropped_ms(void); /* audio lost to a slow card */

#ifdef __cplusplus
}
#endif

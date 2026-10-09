/*
 * rtp.h — RTP/G.711 media session, bridged to a phone line's audio.
 *
 * One session per line while a call is up:
 *   uplink   (phone -> net): audio_rx_read() 48k -> decimate 8k -> µ-law -> RTP
 *   downlink (net -> phone): RTP -> µ-law -> upsample 48k -> audio_ext_write()
 *
 * The SIP layer (voip.c) picks the local port, advertises it in SDP, learns
 * the remote ip:port from the answer, then calls rtp_start().
 */
#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Fixed local RTP port per line, advertised in our SDP offer/answer. */
uint16_t rtp_local_port(int line);

/* Bind rtp_local_port(line) and stream to remote_ip:remote_port. Spawns the
 * tx/rx tasks. Idempotent-ish: call rtp_stop() first if already running. */
esp_err_t rtp_start(int line, const char *remote_ip, uint16_t remote_port);

/* Tear down the session on `line` (call ended). Safe if not running. */
void rtp_stop(int line);


#ifdef __cplusplus
}
#endif

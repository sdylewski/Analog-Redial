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

/* Call-quality counters for one line: live while a call is up, otherwise the
 * last call's (all zero before the first). Loss/late are from RTP sequence
 * numbers; jitter is the RFC 3550 estimate; max_gap_ms is the longest wait
 * between two arriving packets (20 is perfect; 100+ is an audible hole). */
typedef struct {
    bool     active;
    uint32_t duration_s;
    uint32_t rx_pkts, rx_lost, rx_late;
    uint32_t rx_gaps60;        /* gaps between packets over 60 ms */
    uint32_t rx_max_gap_ms;
    uint32_t jitter_us;
    uint32_t dl_drops;         /* packets the playback buffer could not take in full */
    uint32_t tx_pkts, tx_underruns, tx_send_fail;
    int      rssi_start, rssi_end;   /* dBm; 0 = unknown */
    uint32_t heap_min_free;    /* lowest free heap since boot, at the last update */
} rtp_stats_t;
void rtp_stats_get(int line, rtp_stats_t *out);

/* Tear down the session on `line` (call ended). Safe if not running. */
void rtp_stop(int line);


#ifdef __cplusplus
}
#endif

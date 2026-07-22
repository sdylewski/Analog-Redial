/*
 * voip.h — SIP/RTP call backend interface (the VoIP "seam").
 *
 * This interface is defined NOW so the telephony state machine can be written
 * against it, even though the SIP/RTP implementation is the final milestone.
 * The current implementation (voip.c) is a stub that reports "not registered"
 * and returns ESP_ERR_NOT_SUPPORTED for calls, but exercises the event path so
 * the FSM wiring can be tested.
 *
 * Audio plumbing (when implemented):
 *   downlink (network -> phone): resample G.711 8k -> engine rate, audio_ext_write()
 *   uplink   (phone -> network): audio_rx_read(), resample engine rate -> 8k, encode
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    VOIP_EV_REGISTERED = 0,
    VOIP_EV_REGISTER_FAILED,
    VOIP_EV_INCOMING,   /* a call is ringing on .line; remote in .peer */
    VOIP_EV_ANSWERED,   /* media is up */
    VOIP_EV_CLOSED,     /* remote/again hung up */
} voip_event_type_t;

typedef struct {
    int               line;
    voip_event_type_t type;
    char              peer[64];
} voip_event_t;

typedef void (*voip_event_cb)(const voip_event_t *ev, void *ctx);

typedef struct {
    const char *user;       /* SIP user/extension, e.g. "1000" */
    const char *password;
    const char *domain;     /* registrar / PBX, e.g. "feynman.local" */
    const char *outbound;   /* optional proxy */
    int         line;       /* which phone line this account maps to */
} voip_account_t;

/* Initialize the VoIP subsystem (Wi-Fi must already be connected for the real
 * implementation; the stub ignores this). */
esp_err_t voip_init(voip_event_cb cb, void *ctx);

/* Register an account for a line. */
esp_err_t voip_register(const voip_account_t *acct);

bool      voip_is_registered(int line);

/* Place an outbound call. number is the dialed string (e.g. "5551234"); the
 * implementation maps it to a SIP URI. */
esp_err_t voip_dial(int line, const char *number);

/* Answer the currently-ringing inbound call on a line. */
esp_err_t voip_answer(int line);

/* Hang up / cancel any call on a line. */
esp_err_t voip_hangup(int line);

/* True if the implementation is a stub (no SIP stack compiled in). */
bool      voip_is_stub(void);

/* Save SIP config to NVS: server host/IP + per-line SIP user/password. Pass
 * NULL for any field to leave it unchanged. Takes effect on reboot (the SIP
 * task binds + registers at startup). */
esp_err_t voip_set_config(const char *server, const char *user0, const char *pass0,
                          const char *user1, const char *pass1);

/* Configured SIP server (host or IP), for status display. */
const char *voip_server(void);

#ifdef __cplusplus
}
#endif

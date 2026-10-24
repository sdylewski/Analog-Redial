/* homecall.h -- calls between Analog Redial phones on the same network, no
 * server.
 *
 * Each line can have a "home number" (3-11 digits). Boards advertise their
 * numbers over mDNS (_redial-phone._udp, TXT n0/n1) and look for each other
 * every minute; dialling another board's number places a direct SIP call to
 * it (voip_dial_local), with the same audio path as VoIP calls through a
 * server. Trust is the local network itself: no keys, no server.
 *
 * On first start-up, once on Wi-Fi, the board picks a free 4-digit number for
 * each line (leading 1 unless Bluetooth dial-out owns it), so every board is
 * callable from the start. The same four digits are the default VoIP-server
 * extension (dialled as 555 + the digits from other phones on that server).
 * Boards are advertised even with no number, and shown as "not set up".
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HOMECALL_NUM_MAX   12   /* digits + NUL */
#define HOMECALL_PEERS_MAX 8

typedef struct {
    char     name[32];                     /* the other board's name */
    char     ip[16];
    char     number[2][HOMECALL_NUM_MAX];  /* its lines' numbers ("" = none) */
    uint32_t age_s;                        /* since it last answered */
    bool     older_firmware;               /* found by its web page, not as a phone: can't be called until updated */
} homecall_peer_t;

/* Loads this board's numbers and starts advertising/looking. Call after Wi-Fi
 * and VoIP are initialised. */
void homecall_init(void);

const char *homecall_number(int line);

/* True while this board's numbers were picked by the board itself on first
 * start-up (cleared as soon as someone sets one by hand). */
bool homecall_number_is_automatic(void);

/* Saves a line's number ("" clears it). Looks for other boards first, so a
 * number another board already uses is refused. msg says what happened. */
esp_err_t homecall_set_number(int line, const char *number, char *msg, size_t msg_len);

/* Looks for other boards now (about 3 s). */
void homecall_refresh(void);

size_t homecall_peers(homecall_peer_t *out, size_t max);
/* The address of the board that answers to `number`. */
bool homecall_lookup(const char *number, char *ip, size_t ip_len, char *name, size_t name_len);

#ifdef __cplusplus
}
#endif

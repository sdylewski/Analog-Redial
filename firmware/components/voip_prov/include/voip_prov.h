#pragma once

/* Client for the Analog Redial VoIP provisioner (server/voip-provisioner/):
 * claims a phone number on a provisioner-managed Asterisk and saves the SIP
 * credentials it hands back into the normal VoIP config.
 *
 * Every request is signed with HMAC-SHA256 -- first with the enrollment key
 * the user enters, then with the per-board token the provisioner issues on
 * the first successful registration -- so neither secret crosses the
 * network. The URL may be http:// (LAN) or https:// (internet; certificates
 * checked against the built-in root bundle). */

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VOIP_PROV_MSG_LEN 160

/* Saved provisioner base URL, e.g. "http://pbx.local:8077" ("" if unset). */
const char *voip_prov_url(void);
esp_err_t   voip_prov_set_url(const char *url);

/* Enrollment key, "<id>.<secret>" as printed by the provisioner's add-key.
 * Never readable back -- only whether one is saved. */
bool        voip_prov_has_key(void);
esp_err_t   voip_prov_set_key(const char *key);

/* True once the provisioner has issued this board its own token. */
bool        voip_prov_enrolled(void);

/* The range of numbers this board's key may register, as last reported by
 * the provisioner. False if it hasn't been heard yet. */
bool        voip_prov_number_range(int *min, int *max);

/* Asks the provisioner for the key's number range (saved for
 * voip_prov_number_range()). Blocks for one or two round trips. */
esp_err_t   voip_prov_refresh(char *msg, size_t msg_len);

/* Looks for a provisioner advertising _redial-prov._tcp on the LAN and
 * writes its URL into url. Blocks up to ~3 s. */
esp_err_t   voip_prov_discover(char *url, size_t url_len, char *name, size_t name_len);

/* Each of these blocks for one or two HTTP round trips (a few seconds at
 * most) and writes a one-line, user-readable outcome into msg either way. */
esp_err_t   voip_prov_check(const char *number, bool *available, char *msg, size_t msg_len);

/* Claims number for line (0 or 1) and saves the returned server + SIP
 * credentials with voip_set_config(). Like that call, the new account is
 * used from the next reboot. */
esp_err_t   voip_prov_register(int line, const char *number, char *msg, size_t msg_len);

/* Gives line's number back and clears that line's SIP credentials. */
esp_err_t   voip_prov_release(int line, char *msg, size_t msg_len);

/* Whether the provisioner offers text-to-speech: -1 not known yet (nothing
 * asked since boot), 0 no, 1 yes. Learned from every request's info call. */
int         voip_prov_tts_state(void);
/* Has the server speak `text` (already expanded, e.g. {{boardname}}) and saves
 * the WAV it returns into local storage as `name`, streamed in small chunks.
 * msg says what happened, for people. */
esp_err_t   voip_prov_tts(const char *text, const char *name, char *msg, size_t msg_len);
/* The voice the server last reported ("" if not known yet or no TTS). */
const char *voip_prov_tts_voice(void);
/* The tag the server writes into a WAV it renders (a LIST/INFO ICMT chunk):
 * "redial-tts voice=<voice> text=<16 hex digits of SHA-256(text)>", text with its
 * whitespace collapsed. A stored file whose tag matches needs no new render. */
void        voip_prov_tts_tag(const char *voice, const char *text, char *out, size_t out_len);

#ifdef __cplusplus
}
#endif

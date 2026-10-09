/*
 * webauth.h — optional web UI password.
 *
 * Off by default. With a password set, the web UI requires signing in to
 * change anything (every POST) or to fetch pages that carry secrets; pages
 * themselves stay readable. The password is stored only as a salted
 * PBKDF2-SHA256 hash in NVS. Sign-ins are random session tokens held in
 * RAM, so a reboot signs everyone out.
 *
 * Forgotten password: the BOOT button's 3 s rescue mode lets a client on the
 * board's own Wi-Fi network in without it (the web UI decides that), the
 * 10 s hold calls webauth_clear(), and so does the console's
 * `webpass clear`.
 *
 * Not thread-safe across tasks except where noted: everything but
 * webauth_clear() is called from the web server's single task.
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WEBAUTH_PASSWORD_MAX   64   /* characters */
#define WEBAUTH_TOKEN_LEN      32   /* hex characters, without the NUL */
#define WEBAUTH_SESSION_SECS   (30 * 24 * 3600)

void webauth_init(void);

bool webauth_enabled(void);

/* Sets a new password (1..WEBAUTH_PASSWORD_MAX chars), signing out every
 * existing session. */
esp_err_t webauth_set_password(const char *password);

/* Removes the password (login off) and signs everyone out. Safe from any
 * task (the BOOT button, the console). */
esp_err_t webauth_clear(void);

typedef enum {
    WEBAUTH_OK,
    WEBAUTH_WRONG,
    WEBAUTH_LOCKED,   /* too many wrong tries: refused without checking */
} webauth_result_t;

/* Checks a password. After 5 wrong tries in a row, further tries are
 * refused for 30 s, doubling per failure up to 10 min, so guessing over
 * the network is slow. *retry_secs (if non-NULL) is set when LOCKED. */
webauth_result_t webauth_check(const char *password, int *retry_secs);

/* Starts a session: writes a WEBAUTH_TOKEN_LEN-char token + NUL to out.
 * Only a few are kept; starting one past that drops the oldest. */
void webauth_session_new(char out[WEBAUTH_TOKEN_LEN + 1]);
bool webauth_session_valid(const char *token);
void webauth_session_end(const char *token);

#ifdef __cplusplus
}
#endif

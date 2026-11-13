/*
 * payphone.h -- the experimental Payphone features (Settings -> Payphone):
 * settings, the coin counter and the coin log. telephony.c does the calling
 * side (coin required for dial tone, messages after a coin) and feeds each
 * line's audio to the coin tone detector (coin_tone.h).
 *
 * Coins come from the line's coin tones, from the web UI's "simulate a coin"
 * buttons, and from the coin board's switches (coinboard.h).
 *
 * Collect and return: telephony.c keeps the coins dropped since the handset
 * was lifted (the phone's hopper holds them) and calls payphone_relay():
 * collect when a call it placed is answered, return when the handset goes
 * down without one. The coin board, if one is set up, fires its relay.
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* How the firmware collects / returns coins (Settings -> Payphone).
 * See PayPhone/payphone-types.md for which phones need which method. */
typedef enum {
    PAYPHONE_RELAY_NONE  = 0,   /* just log (no coin control hardware) */
    PAYPHONE_RELAY_TONES = 1,   /* green box tones (2200+2600 Hz) down the line */
    PAYPHONE_RELAY_BOARD = 2,   /* coin board fires the relay electrically */
    PAYPHONE_RELAY_BOTH  = 3,   /* tones AND board */
} payphone_relay_method_t;

/* Spoken prompts (prompts.h), made from text like the operator message. */
#define PAYPHONE_DEPOSIT_CLIP "coin_deposit.wav"   /* "Please deposit..." until there's credit */
#define PAYPHONE_THANKS_CLIP  "coin_thanks.wav"    /* after a coin, unless that coin has its own clip */

#define PAYPHONE_CLIP_MAX 32   /* matches STORAGE_MAX_NAME */
#define PAYPHONE_COINS    3    /* 5, 10, 25 cents */

typedef struct {
    bool     enabled;        /* the whole feature; off = the board behaves as before */
    bool     tone_detect;    /* listen for coin tones on the line */
    bool     coin_required;  /* no calls until the credit reaches price_cents */
    bool     codes_free;     /* the board's own dial codes work without a coin */
    bool     coin_msg;       /* play a message after each coin */
    uint16_t price_cents;    /* what a call costs */
    uint8_t  repeat_s;       /* seconds between "please deposit" messages */
    uint8_t  relay_method;   /* payphone_relay_method_t */
    char     coin_clip[PAYPHONE_COINS][PAYPHONE_CLIP_MAX]; /* per coin; "" = PAYPHONE_THANKS_CLIP */
} payphone_cfg_t;

typedef enum {
    PAYPHONE_SRC_TONE = 0,   /* coin tones on the line */
    PAYPHONE_SRC_SIM,        /* the web UI's simulate buttons */
    PAYPHONE_SRC_BOARD,      /* the coin board's switches */
} payphone_src_t;

typedef struct {
    uint32_t count[PAYPHONE_COINS];  /* nickels, dimes, quarters */
    uint32_t cents;
} payphone_totals_t;

#define PAYPHONE_LOG_N   40
#define PAYPHONE_MSG_LEN 64

typedef struct {
    time_t  when;            /* wall clock; < 1700000000 = before the clock was set */
    int64_t up_ms;           /* uptime, for entries made before the clock was set */
    int8_t  line;
    char    msg[PAYPHONE_MSG_LEN];
} payphone_log_t;

void payphone_init(void);

/* A copy of the settings; cheap (RAM), safe from any task. */
payphone_cfg_t payphone_cfg(void);
esp_err_t payphone_cfg_set(const payphone_cfg_t *cfg);
bool payphone_enabled(void);
bool payphone_tones_on(void);        /* enabled and tone_detect */

/* 5 -> 0, 10 -> 1, 25 -> 2; -1 for any other value. */
int payphone_coin_index(int cents);
const char *payphone_src_name(payphone_src_t src);

/* A coin seen: counts it, logs it. */
void payphone_coin_seen(int line, int cents, payphone_src_t src, int credit_after);
void payphone_logf(int line, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* Newest first. */
size_t payphone_log_list(payphone_log_t *out, size_t max);
void payphone_log_clear(void);

payphone_totals_t payphone_totals(void);
void payphone_totals_reset(void);

/* ---- collect and return ---- */

typedef enum {
    PAYPHONE_COLLECT = 0,   /* into the coin box */
    PAYPHONE_RETURN  = 1,   /* back to the caller */
} payphone_relay_t;

/* Does the work, from telephony's task: keep it short (queue it). */
typedef void (*payphone_relay_fn)(int line, payphone_relay_t what);
void payphone_set_relay_handler(payphone_relay_fn fn);

/* Called by telephony.c: logs it ("25 c collected: call answered") and passes
 * it to the coin board, if one has registered. */
void payphone_relay(int line, payphone_relay_t what, int cents, const char *why);

/* The line the coin board's switches count coins on (-1: none). Coin tones
 * are ignored on that line, since the switches see the same coins. */
void payphone_set_board_line(int line);
int  payphone_board_line(void);
/* Coin tones are listened for on `line`: payphone_tones_on(), and not the
 * coin board's line. */
bool payphone_tones_on_line(int line);

#ifdef __cplusplus
}
#endif

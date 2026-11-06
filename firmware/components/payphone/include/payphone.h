/*
 * payphone.h -- the experimental Payphone features (Settings -> Payphone):
 * settings, the coin counter and the coin log. telephony.c does the calling
 * side (coin required for dial tone, messages after a coin) and feeds each
 * line's audio to the coin tone detector (coin_tone.h).
 *
 * Coins come from the line's coin tones, from the web UI's "simulate a coin"
 * buttons, and later from the coin board (PayPhone/ in the repo).
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
    char     coin_clip[PAYPHONE_COINS][PAYPHONE_CLIP_MAX]; /* per coin; "" = PAYPHONE_THANKS_CLIP */
} payphone_cfg_t;

typedef enum {
    PAYPHONE_SRC_TONE = 0,   /* coin tones on the line */
    PAYPHONE_SRC_SIM,        /* the web UI's simulate buttons */
    PAYPHONE_SRC_BOARD,      /* the coin board's switches (not built yet) */
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

#ifdef __cplusplus
}
#endif

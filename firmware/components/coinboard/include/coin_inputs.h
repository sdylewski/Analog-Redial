/*
 * coin_inputs.h -- counts coins from the coin board's switch inputs (U1
 * P0-P3), polled every few ms over I2C. No ESP-IDF code, so
 * firmware/test/host/test_coin_inputs.c runs it against bouncing switches.
 *
 * A coin is one closure: the input reads closed on two polls in a row, at
 * least COIN_IN_CLOSE_MS apart (a single-poll spike is noise). After a coin the input has
 * to read open for COIN_IN_REARM_MS before it can count again, so contact
 * bounce on the way in or out never counts twice. An input closed for
 * COIN_IN_STUCK_MS is reported once as stuck (a jammed coin, a shorted
 * cable) and counts nothing until it opens. An input already closed when
 * counting starts isn't a coin either.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define COIN_IN_N         4
#define COIN_IN_CLOSE_MS  2      /* closed this long, over two polls or more */
#define COIN_IN_REARM_MS  30     /* open this long before the next coin */
#define COIN_IN_STUCK_MS  2000

typedef struct {
    int64_t closed_since;   /* -1 while open */
    int64_t open_since;     /* -1 while closed */
    bool    armed;
    bool    stuck;
} coin_in_ch_t;

typedef struct {
    coin_in_ch_t ch[COIN_IN_N];
} coin_in_t;

void coin_in_init(coin_in_t *c);

/* One poll at now_ms. `closed`: bit i set when input i reads closed (low on
 * the chip). Returns the inputs that just counted a coin; *stuck_now (if
 * non-NULL) gets the inputs that just became stuck. */
uint8_t coin_in_push(coin_in_t *c, uint8_t closed, int64_t now_ms, uint8_t *stuck_now);

/* The inputs stuck closed now. */
uint8_t coin_in_stuck(const coin_in_t *c);

#ifdef __cplusplus
}
#endif

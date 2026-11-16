/*
 * payphone_types.h -- the Payphone tab's "Phone type": a lookup table from
 * what the phone is (its housing, and what is inside it) to the settings it
 * needs, so a builder picks a phone instead of setting each value by hand.
 *
 * Two choices, in this order:
 *   1. Housing: the case and its maker (a Quadrum is only a housing; the same
 *      case held many different boards).
 *   2. Inside: what handles the coins, which decides everything else: the
 *      phone's own smart board, its coin switches and relay wired to the coin
 *      board, a coin-line totalizer that sends coin tones, or no coin parts.
 * Each housing lists the insides it is found with, the usual one first.
 *
 * The table is plain data with no ESP-IDF calls (host-tested by
 * firmware/test/host/test_payphone_types.c). Where the categories come from:
 * PayPhone/README.md#payphone-types and https://telesfor.org/payphones/.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PP_HOUSING_NONE = 0,     /* not chosen yet: every setting shows */
    PP_HOUSING_QUADRUM,      /* Quadrum / GTE 120B style, "smart" phone housings */
    PP_HOUSING_WESTERN,      /* Western Electric / AT&T single-slot (1C, 1D, 2C) */
    PP_HOUSING_3SLOT,        /* Western Electric 3-slot, before 1965 */
    PP_HOUSING_MILLENNIUM,   /* Nortel Millennium */
    PP_HOUSING_OTHER,        /* other, or not sure */
    PP_HOUSING_N
} pp_housing_t;

typedef enum {
    PP_INSIDE_NONE = 0,      /* not chosen yet */
    PP_INSIDE_COINBOARD,     /* coin switches and coin relay wired to the coin board */
    PP_INSIDE_PHONEONLY,     /* no coin board: the phone's own coin parts are left alone; used as a phone */
    PP_INSIDE_SMART,         /* its own smart board, working and programmed */
    PP_INSIDE_COINLINE,      /* coin-line totalizer: sends coin tones, relay worked from the line */
    PP_INSIDE_COINLESS,      /* coinless conversion: no coin parts in use */
    PP_INSIDE_MANUAL,        /* not sure: show every setting, change nothing */
    PP_INSIDE_N
} pp_inside_t;

/* Settings cards on the Payphone tab's Settings pane. A phone type hides the
 * ones that don't apply to it ("Show all settings" brings them back). */
#define PP_CARD_REQUIRED  (1u << 0)   /* Coin required */
#define PP_CARD_MESSAGE   (1u << 1)   /* Message after a coin */
#define PP_CARD_TONES     (1u << 2)   /* Coin tones */
#define PP_CARD_SOUNDS    (1u << 3)   /* Coin sounds */
#define PP_CARD_COINS     (1u << 4)   /* What happens to the coins */
#define PP_CARD_BOARD     (1u << 5)   /* Coin board */
#define PP_CARD_ALL       0x3Fu

typedef struct {
    const char *name;        /* "Quadrum / GTE 120B style" */
    const char *made;        /* "1980s-2000s" */
    const char *about;       /* one or two sentences: how to recognise it */
    const char *dial;        /* how it dials */
    const char *ring;        /* how it rings */
    uint8_t     insides[PP_INSIDE_N];   /* pp_inside_t found in it, usual first; 0 ends the list */
} pp_housing_info_t;

typedef struct {
    const char *name;        /* "Coin switches and relay on the coin board" */
    const char *about;       /* what that means and what the board does */
    const char *coins;       /* how coins are seen */
    const char *relay;       /* how the coin relay is worked */
    bool        coinboard;   /* needs the coin board */
    bool        board_optional;  /* ... or can use it, but works without */
    bool        tested;      /* known to work on at least one real phone; the rest show as "untested" */
    /* Applied by "Use these settings" (-1: leave as it is). */
    int8_t      tone_detect, coin_required;
    uint8_t     cards;       /* PP_CARD_* shown */
} pp_inside_info_t;

/* A phone that has been measured: settings beyond what the inside implies. */
typedef struct {
    pp_housing_t housing;
    pp_inside_t  inside;
    const char  *phone;      /* "GTE / Quadrum 120B style, HD550002C relay" */
    int8_t       swap;       /* the coin board's Swap: 1 on, 0 off, -1 not settled (left as it is) */
    uint16_t     pulse_ms;   /* the coin board's FIRE pulse */
    const char  *hv;         /* the shunts to fit: "J14 + J7 (87 V)" */
    const char  *notes;      /* anything else, shown under the settings */
} pp_known_t;

const pp_housing_info_t *pp_housing_info(int housing);   /* NULL if out of range or NONE */
const pp_inside_info_t  *pp_inside_info(int inside);     /* NULL if out of range or NONE */
/* true if `inside` is listed for `housing` (MANUAL always is). */
bool pp_inside_fits(int housing, int inside);
/* The usual inside for a housing (its first), PP_INSIDE_MANUAL if none. */
pp_inside_t pp_inside_default(int housing);
/* The measured settings for this pair, or NULL. */
const pp_known_t *pp_known(int housing, int inside);
/* The cards to show: PP_CARD_ALL until both are chosen. */
uint8_t pp_cards(int housing, int inside);

#ifdef __cplusplus
}
#endif

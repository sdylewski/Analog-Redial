/* payphone_types.c -- see payphone_types.h. Plain data: no ESP-IDF calls. */
#include "payphone_types.h"
#include <stddef.h>

#define CARDS_COINBOARD (PP_CARD_REQUIRED | PP_CARD_MESSAGE | PP_CARD_SOUNDS | PP_CARD_COINS | PP_CARD_BOARD)

static const pp_housing_info_t HOUSINGS[PP_HOUSING_N] = {
    [PP_HOUSING_QUADRUM] = {
        .name  = "Quadrum / GTE 120B style",
        .made  = "1980s-2000s",
        .about = "The usual housing for privately owned \"smart\" payphones (Quadrum, also sold as Sentinel; "
                 "the GTE 120B, mostly Frontier). The two differ in where the coin return lever sits. It is only "
                 "the case: the board inside may be an Elcotel, Protel, Ernest, Intellicall or another make.",
        .dial  = "Keypad (DTMF). Wired to Analog Redial, the keypad's tones are heard like any phone's.",
        .ring  = "Whatever ringer is fitted: many payphones have none, and a smart board may be set not to "
                 "ring. Try the ring test on the Diagnostics tab.",
        .insides = { PP_INSIDE_COINBOARD, PP_INSIDE_PHONEONLY, PP_INSIDE_SMART, PP_INSIDE_COINLESS,
                     PP_INSIDE_MANUAL },
    },
    [PP_HOUSING_WESTERN] = {
        .name  = "Western Electric / AT&T single-slot (1C, 1D, 2C)",
        .made  = "1965-1990s",
        .about = "The telephone companies' coin-line phones, one slot for every coin. The board is named by "
                 "three characters, e.g. 1D2: 1 wall-mounted, 2 flush-mounted; A coin first, C coin first or "
                 "dial tone first, D dial tone first; last digit 1 rotary, 2 Touch-Tone. Many were later "
                 "refitted with smart boards (Protel 4000 or 8000, Elcotel 5501).",
        .dial  = "Rotary (a board ending in 1) or Touch-Tone (ending in 2): both work, nothing to set.",
        .ring  = "Its own bell, rung by the line like any phone's, if it hasn't been taken out.",
        .insides = { PP_INSIDE_COINLINE, PP_INSIDE_PHONEONLY, PP_INSIDE_SMART, PP_INSIDE_COINBOARD,
                     PP_INSIDE_COINLESS, PP_INSIDE_MANUAL },
    },
    [PP_HOUSING_3SLOT] = {
        .name  = "Western Electric 3-slot",
        .made  = "1920s-1960s",
        .about = "Separate slots for nickels, dimes and quarters. Coins strike bells and a gong that the "
                 "operator listened to, rather than sending tones, so Analog Redial can't count them.",
        .dial  = "Rotary, or none on the earliest (manual) phones.",
        .ring  = "Its own bell, if fitted.",
        .insides = { PP_INSIDE_COINLESS, PP_INSIDE_MANUAL },
    },
    [PP_HOUSING_MILLENNIUM] = {
        .name  = "Nortel Millennium",
        .made  = "1990s-2000s",
        .about = "Nortel's smart payphone with a display and card reader, used mostly by the big telephone "
                 "companies in Canada and the US. Its board does everything and needs its own programming.",
        .dial  = "Keypad; its board dials out by itself.",
        .ring  = "Set by its board's programming.",
        .insides = { PP_INSIDE_SMART, PP_INSIDE_PHONEONLY, PP_INSIDE_COINLESS, PP_INSIDE_MANUAL },
    },
    [PP_HOUSING_OTHER] = {
        .name  = "Other, or not sure",
        .made  = "",
        .about = "Any other payphone. Pick what's inside if you know it; otherwise every setting shows.",
        .dial  = "Rotary and keypad phones both work.",
        .ring  = "Its own ringer, if fitted. Try the ring test on the Diagnostics tab.",
        .insides = { PP_INSIDE_MANUAL, PP_INSIDE_COINBOARD, PP_INSIDE_PHONEONLY, PP_INSIDE_SMART,
                     PP_INSIDE_COINLINE, PP_INSIDE_COINLESS },
    },
};

static const pp_inside_info_t INSIDES[PP_INSIDE_N] = {
    [PP_INSIDE_COINBOARD] = {
        .name  = "Coin switches and coin relay wired to the coin board",
        .about = "The phone's own board is removed or bypassed. Its coin switches go to the coin board's J3 and "
                 "its coin relay to J2: Analog Redial counts the coins, asks for them before a call, and collects "
                 "or returns them.",
        .coins = "the phone's coin switches, read by the coin board",
        .relay = "fired by the coin board, at 19-119 V set by its shunts",
        .coinboard = true,
        .tested = true,   /* on the GTE / Quadrum phone */
        .tone_detect = 0, .coin_required = 1,
        .cards = CARDS_COINBOARD,
    },
    [PP_INSIDE_PHONEONLY] = {
        .name  = "No coin board: use it as a phone",
        .about = "No coin board: the phone's own coin mechanism is left as it is, and Analog Redial can't collect or "
                 "return coins. It works as a phone. Some phones send coin tones when a coin drops: with them it "
                 "can ask for a coin before a call and play a message after each. Listening is on, and harmless "
                 "if the phone sends none: drop a coin and watch the Log pane. Coin required stays off until "
                 "the log shows the tones counted.",
        .coins = "coin tones on the line, if the phone sends them; otherwise not counted",
        .relay = "none: coins are not collected or returned",
        .tone_detect = 1, .coin_required = 0,
        .cards = PP_CARD_REQUIRED | PP_CARD_MESSAGE | PP_CARD_TONES | PP_CARD_SOUNDS,
    },
    [PP_INSIDE_SMART] = {
        .name  = "Its own smart board, working and programmed",
        .about = "A smart (COCOT) board, such as Elcotel, Protel, Intellicall, Ernest or AT&T Private Payphone, "
                 "counts the coins, works the relay and dials out by itself: Analog Redial only gives it a phone "
                 "line, and nothing on this tab applies. Most need programming with their own software and a "
                 "modem, and some need a battery as well as the line. Not yet tried with Analog Redial.",
        .coins = "counted by the phone's own board; Analog Redial doesn't see them",
        .relay = "worked by the phone's own board",
        .tone_detect = 0, .coin_required = 0,
        .cards = 0,
    },
    [PP_INSIDE_COINLINE] = {
        .name  = "Coin-line totalizer (sends coin tones)",
        .about = "The original telephone-company board: a totalizer adds up the coins and sends coin tones "
                 "(1700 and 2200 Hz) down the line, which Analog Redial hears. On a real coin line the exchange "
                 "collected or returned the coins with a high voltage on the line; Analog Redial can't, but the "
                 "coin relay's own wires could go to the coin board's J2 instead (not yet tried). A coin-first "
                 "board also waits for a ground start before dial tone, which Analog Redial doesn't give yet.",
        .coins = "coin tones on the line",
        .relay = "the coin board, if the relay's wires are moved to it; otherwise coins stay in the hopper",
        .board_optional = true,
        .tone_detect = 1, .coin_required = 1,
        .cards = PP_CARD_ALL,
    },
    [PP_INSIDE_COINLESS] = {
        .name  = "No coin parts in use (coinless)",
        .about = "The coin mechanism is gone or not wired: the phone works like any other, and nothing on this "
                 "tab applies.",
        .coins = "none",
        .relay = "none",
        .tone_detect = 0, .coin_required = 0,
        .cards = 0,
    },
    [PP_INSIDE_MANUAL] = {
        .name  = "Not sure: set everything by hand",
        .about = "Every setting shows, and choosing this changes none of them.",
        .coins = "as set below",
        .relay = "as set below",
        .board_optional = true,
        .tone_detect = -1, .coin_required = -1,
        .cards = PP_CARD_ALL,
    },
};

static const pp_known_t KNOWN[] = {
    {
        .housing = PP_HOUSING_QUADRUM, .inside = PP_INSIDE_COINBOARD,
        .phone = "GTE / Quadrum 120B style with an HD550002C coin relay (the project's first phone)",
        .swap = 0, .pulse_ms = 100, .hv = "J14 + J7 (87 V)",
        .notes = "In the phone the relay needs 87 V (it moves at 60 V on the bench, without the levers). After a "
                 "fire its lever stays down, and the relay can't fire again, until the next coin pushes it up; a "
                 "lever that won't come back needs cleaning and oiling. Swap is off for this phone. Check the "
                 "direction on the Bring-up pane: this is one phone, and yours may be wired the other way.",
    },
};

const pp_housing_info_t *pp_housing_info(int housing)
{
    return housing > PP_HOUSING_NONE && housing < PP_HOUSING_N ? &HOUSINGS[housing] : NULL;
}

const pp_inside_info_t *pp_inside_info(int inside)
{
    return inside > PP_INSIDE_NONE && inside < PP_INSIDE_N ? &INSIDES[inside] : NULL;
}

bool pp_inside_fits(int housing, int inside)
{
    if (inside == PP_INSIDE_MANUAL) return true;
    const pp_housing_info_t *h = pp_housing_info(housing);
    if (!h || !pp_inside_info(inside)) return false;
    for (int i = 0; i < PP_INSIDE_N && h->insides[i]; i++)
        if (h->insides[i] == inside) return true;
    return false;
}

pp_inside_t pp_inside_default(int housing)
{
    const pp_housing_info_t *h = pp_housing_info(housing);
    return h && h->insides[0] ? (pp_inside_t)h->insides[0] : PP_INSIDE_MANUAL;
}

const pp_known_t *pp_known(int housing, int inside)
{
    for (size_t i = 0; i < sizeof(KNOWN) / sizeof(KNOWN[0]); i++)
        if ((int)KNOWN[i].housing == housing && (int)KNOWN[i].inside == inside) return &KNOWN[i];
    return NULL;
}

uint8_t pp_cards(int housing, int inside)
{
    const pp_inside_info_t *in = pp_inside_info(inside);
    if (!pp_housing_info(housing) || !in) return PP_CARD_ALL;
    return in->cards;
}

/* Host test for components/payphone/payphone_types.c: the phone type table is
 * complete and consistent (every housing lists real insides, the usual one
 * first; every inside that needs the coin board shows its card; the measured
 * phones point at a pair that exists). */
#include "payphone_types.h"
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

int main(void)
{
    CHECK(!pp_housing_info(PP_HOUSING_NONE) && !pp_housing_info(PP_HOUSING_N) && !pp_housing_info(-1), "out-of-range housing");
    CHECK(!pp_inside_info(PP_INSIDE_NONE) && !pp_inside_info(PP_INSIDE_N), "out-of-range inside");
    CHECK(pp_cards(0, 0) == PP_CARD_ALL, "nothing chosen shows every card");

    for (int h = 1; h < PP_HOUSING_N; h++) {
        const pp_housing_info_t *hi = pp_housing_info(h);
        CHECK(hi && hi->name && hi->about && hi->dial && hi->ring && hi->made, "housing %d has every text", h);
        CHECK(hi->insides[0] != 0, "housing %d lists an inside", h);
        CHECK(pp_inside_default(h) == hi->insides[0], "housing %d default is its first", h);
        bool manual = false;
        for (int k = 0; k < PP_INSIDE_N && hi->insides[k]; k++) {
            CHECK(pp_inside_info(hi->insides[k]) != NULL, "housing %d inside %d exists", h, hi->insides[k]);
            CHECK(pp_inside_fits(h, hi->insides[k]), "housing %d fits its own inside %d", h, hi->insides[k]);
            for (int j = 0; j < k; j++) CHECK(hi->insides[j] != hi->insides[k], "housing %d lists %d twice", h, hi->insides[k]);
            manual |= hi->insides[k] == PP_INSIDE_MANUAL;
        }
        CHECK(manual, "housing %d offers 'not sure'", h);
        CHECK(pp_inside_fits(h, PP_INSIDE_MANUAL), "manual always fits");
        CHECK(strlen(hi->about) < 600 && strlen(hi->dial) < 600 && strlen(hi->ring) < 600, "housing %d text fits the page's buffers", h);
    }
    CHECK(!pp_inside_fits(PP_HOUSING_3SLOT, PP_INSIDE_COINBOARD), "3-slot doesn't list the coin board");
    CHECK(!pp_inside_fits(PP_HOUSING_NONE, PP_INSIDE_COINBOARD), "no housing fits nothing");

    for (int i = 1; i < PP_INSIDE_N; i++) {
        const pp_inside_info_t *in = pp_inside_info(i);
        CHECK(in && in->name && in->about && in->coins && in->relay, "inside %d has every text", i);
        CHECK(strlen(in->about) < 600 && strlen(in->name) < 150, "inside %d text fits the page's buffers", i);
        if (in->coinboard) CHECK(in->cards & PP_CARD_BOARD, "inside %d needs the board and shows its card", i);
        CHECK(!(in->coinboard && in->board_optional), "inside %d: needed or optional, not both", i);
        CHECK(in->tone_detect >= -1 && in->tone_detect <= 1 && in->coin_required >= -1 && in->coin_required <= 1, "inside %d values", i);
    }
    CHECK(pp_cards(PP_HOUSING_QUADRUM, PP_INSIDE_COINBOARD) == pp_inside_info(PP_INSIDE_COINBOARD)->cards, "cards follow the inside");
    CHECK(!(pp_cards(PP_HOUSING_QUADRUM, PP_INSIDE_COINBOARD) & PP_CARD_TONES), "coin board phones hide coin tones");
    CHECK(pp_cards(PP_HOUSING_QUADRUM, PP_INSIDE_SMART) == 0, "a smart board phone hides everything");
    CHECK(pp_inside_info(PP_INSIDE_PHONEONLY) && !pp_inside_info(PP_INSIDE_PHONEONLY)->coinboard &&
          !pp_inside_info(PP_INSIDE_PHONEONLY)->tested && (pp_cards(PP_HOUSING_QUADRUM, PP_INSIDE_PHONEONLY) & PP_CARD_TONES) &&
          !(pp_cards(PP_HOUSING_QUADRUM, PP_INSIDE_PHONEONLY) & PP_CARD_BOARD), "phone-only: no board, untested, hears tones");
    CHECK(pp_inside_fits(PP_HOUSING_WESTERN, PP_INSIDE_PHONEONLY) && !pp_inside_fits(PP_HOUSING_3SLOT, PP_INSIDE_PHONEONLY), "phone-only fits the right housings");
    CHECK(pp_inside_info(PP_INSIDE_COINBOARD)->tested, "the coin board is tested");

    const pp_known_t *k = pp_known(PP_HOUSING_QUADRUM, PP_INSIDE_COINBOARD);
    CHECK(k && k->swap == 0 && k->pulse_ms == 100 && k->hv && k->notes, "the first phone is known");
    CHECK(k && pp_inside_fits(k->housing, k->inside), "the known phone's pair exists");
    CHECK(!pp_known(PP_HOUSING_WESTERN, PP_INSIDE_COINLINE), "no measurements for unmeasured phones");

    printf("test_payphone_types: %s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}

/* coin_inputs.c -- see coin_inputs.h. */
#include "coin_inputs.h"
#include <string.h>

void coin_in_init(coin_in_t *c)
{
    memset(c, 0, sizeof(*c));
    for (int i = 0; i < COIN_IN_N; i++) {
        c->ch[i].closed_since = -1;
        c->ch[i].open_since = -1;
    }
}

uint8_t coin_in_push(coin_in_t *c, uint8_t closed, int64_t now, uint8_t *stuck_now)
{
    uint8_t coins = 0, stuck = 0;
    for (int i = 0; i < COIN_IN_N; i++) {
        coin_in_ch_t *ch = &c->ch[i];
        if (closed & (1u << i)) {
            ch->open_since = -1;
            if (ch->closed_since < 0) {
                ch->closed_since = now;   /* the first closed poll: wait for a second */
                continue;
            }
            if (ch->armed && now - ch->closed_since >= COIN_IN_CLOSE_MS) {
                coins |= (uint8_t)(1u << i);
                ch->armed = false;
            }
            if (!ch->stuck && now - ch->closed_since >= COIN_IN_STUCK_MS) {
                ch->stuck = true;
                stuck |= (uint8_t)(1u << i);
            }
        } else {
            ch->closed_since = -1;
            if (ch->open_since < 0) ch->open_since = now;
            if (now - ch->open_since >= COIN_IN_REARM_MS) {
                ch->armed = true;
                ch->stuck = false;
            }
        }
    }
    if (stuck_now) *stuck_now = stuck;
    return coins;
}

uint8_t coin_in_stuck(const coin_in_t *c)
{
    uint8_t m = 0;
    for (int i = 0; i < COIN_IN_N; i++)
        if (c->ch[i].stuck) m |= (uint8_t)(1u << i);
    return m;
}

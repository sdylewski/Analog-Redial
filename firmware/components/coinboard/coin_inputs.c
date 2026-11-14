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
            ch->last_closed = now;
            if (ch->closed_since < 0) {
                ch->closed_since = now;   /* the first closed poll: wait for a second */
                continue;
            }
            if (ch->armed && now - ch->closed_since >= COIN_IN_CLOSE_MS) {
                coins |= (uint8_t)(1u << i);
                ch->armed = false;
                ch->counted++;
            }
            if (!ch->stuck && now - ch->closed_since >= COIN_IN_STUCK_MS) {
                ch->stuck = true;
                stuck |= (uint8_t)(1u << i);
            }
        } else {
            if (ch->closed_since >= 0) {
                int32_t w = (int32_t)(ch->last_closed - ch->closed_since);
                if (w > ch->max_ms) ch->max_ms = w;
                if (ch->armed) ch->too_short++;      /* it ended before it counted */
            }
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

void coin_in_stats(const coin_in_t *c, uint32_t counted[COIN_IN_N], uint32_t too_short[COIN_IN_N],
                   int32_t max_ms[COIN_IN_N])
{
    for (int i = 0; i < COIN_IN_N; i++) {
        counted[i] = c->ch[i].counted;
        too_short[i] = c->ch[i].too_short;
        max_ms[i] = c->ch[i].max_ms;
    }
}

void coin_in_clear_stats(coin_in_t *c)
{
    for (int i = 0; i < COIN_IN_N; i++) {
        c->ch[i].counted = 0;
        c->ch[i].too_short = 0;
        c->ch[i].max_ms = 0;
    }
}

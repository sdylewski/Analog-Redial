#include "dialplan.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "dialplan";

/* Clip ids — assets live in flash later (see FEATURES.md milestone 3). */
enum {
    CLIP_6 = 6, CLIP_7 = 7, CLIP_8 = 8, CLIP_9 = 9,
    CLIP_BANANA_0 = 430, CLIP_BANANA_1 = 431, CLIP_BANANA_2 = 432, CLIP_BANANA_3 = 433,
};

typedef struct {
    const char            *code;
    dialplan_action_kind_t kind;
    int                    arg;
    int                    arg2;
    bool                   play_only;   /* blocked in CALL_ONLY mode */
    const char            *desc;
    bool                   implemented; /* see dialplan_entry_info_t */
} entry_t;

/* Authoritative table (see ../pi_rotary_phone/docs/DIAL_CODES.md).
 * `implemented` reflects telephony.c's handle_action() as of this writing --
 * anything still just logging "(... TODO)" there and playing a confirm tone
 * is marked false. Flip it when you wire the real behavior in. */
static const entry_t TABLE[] = {
    /* global modes */
    { "009", ACT_SET_MODE, MODE_PLAY,            0, false,
      "Play mode (allow fun clips / record codes)", true },
    { "008", ACT_SET_MODE, MODE_CALL_ONLY,       0, false,
      "Call-only mode (blocks play-only codes)", true },
    { "301", ACT_SET_MODE, MODE_ENGLISH,         0, false,
      "Language: English (no prompt audio exists yet to be affected by it)", false },
    { "302", ACT_SET_MODE, MODE_ITALIAN,         0, false,
      "Language: Italian (same caveat as English)", false },
    { "003", ACT_SET_MODE, MODE_TOGGLE_READBACK, 0, false,
      "Toggle spoken-digit readback while dialing", true },

    /* operator help -- plays storage/operator.wav via `player` if present,
     * falls back to a confirm tone if it isn't (see telephony.c) */
    { "0",   ACT_OPERATOR_HELP, 0, 0, false,
      "Operator help prompt", true },

    /* single-digit fun clips (play mode) -- plays storage/clip_<N>.wav via
     * `player` if present, falls back to a confirm tone if it isn't */
    { "6",   ACT_PLAY_CLIP, CLIP_6, 0, true, "Play clip 6", true },
    { "7",   ACT_PLAY_CLIP, CLIP_7, 0, true, "Play clip 7", true },
    { "8",   ACT_PLAY_CLIP, CLIP_8, 0, true, "Play clip 8", true },
    { "9",   ACT_PLAY_CLIP, CLIP_9, 0, true, "Play clip 9", true },

    /* banana line -- same clip-playback mechanism as above */
    { "430", ACT_PLAY_CLIP, CLIP_BANANA_0, 0, true, "Banana line, clip 0", true },
    { "431", ACT_PLAY_CLIP, CLIP_BANANA_1, 0, true, "Banana line, clip 1", true },
    { "432", ACT_PLAY_CLIP, CLIP_BANANA_2, 0, true, "Banana line, clip 2", true },
    { "433", ACT_PLAY_CLIP, CLIP_BANANA_3, 0, true, "Banana line, clip 3", true },

    /* record + playback on same handset */
    { "123", ACT_RECORD_PLAYBACK, FX_NORMAL, 0, true, "Record + play back (normal voice)", false },
    { "124", ACT_RECORD_PLAYBACK, FX_HIGH,   0, true, "Record + play back (pitched up)", false },
    { "125", ACT_RECORD_PLAYBACK, FX_LOW,    0, true, "Record + play back (pitched down)", false },
    { "128", ACT_RECORD_PLAYBACK, FX_DALEK,  0, true, "Record + play back (Dalek effect)", false },

    /* record to archive */
    { "111", ACT_RECORD_ARCHIVE, 0, 0, true,
      "Record a message to storage (storage layer exists, capture not wired in yet)", false },

    /* call the other phone */
    { "369", ACT_CALL_LINE, 1, FX_NORMAL, false, "Call line 1 (from line 0)", true },
    { "842", ACT_CALL_LINE, 0, FX_NORMAL, false, "Call line 0 (from line 1)", true },
    { "223", ACT_CALL_LINE, -1, FX_NORMAL, false, "Call the other line", true },
    { "224", ACT_CALL_LINE, -1, FX_HIGH,   false, "Call the other line (pitched up voice)", true },
    { "225", ACT_CALL_LINE, -1, FX_LOW,    false, "Call the other line (pitched down voice)", true },
    { "226", ACT_CALL_LINE, -1, FX_DALEK,  false, "Call the other line (Dalek voice)", true },

    /* internet radio */
    { "201", ACT_RADIO, 1, 0, true, "Internet radio station 1", false },
    { "202", ACT_RADIO, 2, 0, true, "Internet radio station 2", false },
    { "203", ACT_RADIO, 3, 0, true, "Internet radio station 3", false },
    { "204", ACT_RADIO, 4, 0, true, "Internet radio station 4", false },
    { "205", ACT_RADIO, 5, 0, true, "Internet radio station 5", false },
    { "206", ACT_RADIO, 6, 0, true, "Internet radio station 6", false },
    { "207", ACT_RADIO, 7, 0, true, "Internet radio station 7", false },
    { "208", ACT_RADIO, 8, 0, true, "Internet radio station 8", false },
    { "209", ACT_RADIO, 9, 0, true, "Internet radio station 9", false },
};
#define TABLE_N (sizeof(TABLE) / sizeof(TABLE[0]))

static dialplan_modes_t s_modes;

void dialplan_init(void)
{
    memset(&s_modes, 0, sizeof(s_modes));
    ESP_LOGI(TAG, "loaded %u codes", (unsigned)TABLE_N);
}

dialplan_modes_t *dialplan_modes(void) { return &s_modes; }
uint32_t dialplan_interdigit_ms(void) { return 900; } /* matches Pi pulse completion feel */

void dialplan_collector_reset(dialplan_collector_t *c)
{
    c->buf[0] = '\0';
    c->len = 0;
}

void dialplan_apply_mode(int line, dialplan_mode_change_t change)
{
    switch (change) {
        case MODE_PLAY:
            s_modes.call_only = false;
            s_modes.italian[0] = s_modes.italian[1] = false;
            break;
        case MODE_CALL_ONLY:
            s_modes.call_only = true;
            s_modes.italian[0] = s_modes.italian[1] = true; /* Pi forces IT in call-only */
            break;
        case MODE_ENGLISH:
            if (line >= 0 && line < 2) s_modes.italian[line] = false;
            break;
        case MODE_ITALIAN:
            if (line >= 0 && line < 2) s_modes.italian[line] = true;
            break;
        case MODE_TOGGLE_READBACK:
            s_modes.readback_digits = !s_modes.readback_digits;
            break;
    }
    ESP_LOGI(TAG, "mode change %d (line %d): call_only=%d it=[%d,%d] readback=%d",
             change, line, s_modes.call_only, s_modes.italian[0], s_modes.italian[1],
             s_modes.readback_digits);
}

static const entry_t *exact_match(const char *buf)
{
    for (size_t i = 0; i < TABLE_N; i++) {
        if (strcmp(TABLE[i].code, buf) == 0) return &TABLE[i];
    }
    return NULL;
}

static bool has_longer_prefix(const char *buf)
{
    size_t blen = strlen(buf);
    for (size_t i = 0; i < TABLE_N; i++) {
        if (strlen(TABLE[i].code) > blen &&
            strncmp(TABLE[i].code, buf, blen) == 0) {
            return true;
        }
    }
    return false;
}

static dialplan_action_t make(dialplan_action_kind_t kind, int arg, int arg2)
{
    dialplan_action_t a = { .kind = kind, .arg = arg, .arg2 = arg2 };
    return a;
}

/* Resolve target line for "other phone" entries. */
static int resolve_target(const entry_t *e, int line)
{
    if (e->arg >= 0) return e->arg;     /* explicit (369/842) */
    return (line == 0) ? 1 : 0;          /* generic 22x: the other line */
}

static dialplan_action_t fire(const entry_t *e, int line)
{
    if (e->play_only && s_modes.call_only) {
        return make(ACT_WRONG_NUMBER, 0, 0);
    }
    switch (e->kind) {
        case ACT_CALL_LINE:
            return make(ACT_CALL_LINE, resolve_target(e, line), e->arg2);
        default:
            return make(e->kind, e->arg, e->arg2);
    }
}

dialplan_action_t dialplan_feed(dialplan_collector_t *c, int line, char digit)
{
    if (digit < '0' || digit > '9') return make(ACT_NONE, 0, 0);
    if (c->len >= sizeof(c->buf) - 1) dialplan_collector_reset(c);
    c->buf[c->len++] = digit;
    c->buf[c->len] = '\0';

    /* VoIP: 555 + 4 digits = 7-digit dial. */
    if (c->len >= 3 && strncmp(c->buf, "555", 3) == 0) {
        if (c->len >= 7) {
            dialplan_action_t a = make(ACT_VOIP_DIAL, 0, 0);
            strncpy(a.number, c->buf, sizeof(a.number) - 1);
            return a;
        }
        return make(ACT_NONE, 0, 0); /* keep collecting */
    }

    const entry_t *e = exact_match(c->buf);
    if (e) {
        if (has_longer_prefix(c->buf)) {
            return make(ACT_NONE, 0, 0); /* ambiguous: wait for timeout */
        }
        return fire(e, line);
    }

    /* no exact match: is the buffer still a viable prefix? */
    if (has_longer_prefix(c->buf) || (c->len < 3 && c->buf[0] == '5')) {
        return make(ACT_NONE, 0, 0);
    }

    if (c->len >= 3) {
        return make(ACT_WRONG_NUMBER, 0, 0);
    }
    return make(ACT_NONE, 0, 0);
}

dialplan_action_t dialplan_timeout(dialplan_collector_t *c, int line)
{
    if (c->len == 0) return make(ACT_NONE, 0, 0);

    /* 555 partial that never reached 7 digits -> wrong number */
    if (strncmp(c->buf, "555", 3) == 0 && c->len < 7) {
        return make(ACT_WRONG_NUMBER, 0, 0);
    }

    /* Ambiguous shorter code (e.g. "8" while "842" exists): the timeout
     * resolves it in favor of the shorter exact match. */
    const entry_t *e = exact_match(c->buf);
    if (e) return fire(e, line);

    /* Still a viable prefix of a longer code (slow rotary inter-digit):
     * keep waiting rather than declaring a wrong number. */
    if (has_longer_prefix(c->buf)) {
        return make(ACT_NONE, 0, 0);
    }

    return make(ACT_WRONG_NUMBER, 0, 0);
}

size_t dialplan_list(dialplan_entry_info_t *out, size_t max)
{
    size_t n = (TABLE_N < max) ? TABLE_N : max;
    for (size_t i = 0; i < n; i++) {
        out[i].code = TABLE[i].code;
        out[i].desc = TABLE[i].desc;
        out[i].implemented = TABLE[i].implemented;
    }
    return n;
}

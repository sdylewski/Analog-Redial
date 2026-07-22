#include "dialplan.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>

static const char *TAG = "dialplan";
#define MODES_NVS_NS  "phone"
#define MODES_NVS_KEY "modes"

/* Clip ids double as the dial code (and the "clip_<id>.wav" filename
 * player_play() looks for) -- assets live in flash later (see FEATURES.md
 * milestone 3). Pi wav basenames noted for whoever records the assets. */
enum {
    CLIP_RING_OF_FIRE = 401, /* ringfire.wav (EN) / thats_amore.wav (IT) */
    CLIP_BURP         = 402, /* burp.wav */
    CLIP_CROCODILE    = 403, /* crocodile_rock.wav (EN) / crocfa.wav (IT) */
    CLIP_MARTIANS     = 404, /* martians.wav */
    CLIP_BANANA_PHONE = 411, /* bananaphone.wav */
    CLIP_BANANA_BOAT  = 412, /* bananaboat.wav */
    CLIP_BANANA_SPLIT = 413, /* bananasplit.wav */
    CLIP_LIME_COCONUT = 414, /* limeincoconut.wav */
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

/* Authoritative table (see ../pi_rotary_phone/docs/DIAL_CODES.md, matched
 * against the live phone_config.json on the Pi -- interphone dial codes are
 * symmetric 321-324 on both lines there, not the asymmetric 369/842 this
 * table used to guess at).
 * `implemented` reflects telephony.c's handle_action() as of this writing --
 * anything still just logging "(... TODO)" there and playing a confirm tone
 * is marked false. Flip it when you wire the real behavior in. */
static const entry_t TABLE[] = {
    /* operator help -- plays storage/operator.wav via `player` if present,
     * falls back to a confirm tone if it isn't (see telephony.c) */
    { "0",   ACT_OPERATOR_HELP, 0, 0, false,
      "Operator help prompt", true },

    /* 2xx -- leave a message / record & play back (play mode) */
    { "211", ACT_RECORD_ARCHIVE, 0, 0, true,
      "Leave a message (record to storage, download on web UI)", true },
    { "212", ACT_RECORD_PLAYBACK, FX_NORMAL, 0, true, "Record + play back (normal voice)", true },
    { "213", ACT_RECORD_PLAYBACK, FX_HIGH,   0, true, "Record + play back (chipmunk)", true },
    { "214", ACT_RECORD_PLAYBACK, FX_LOW,    0, true, "Record + play back (low pitch)", true },
    { "215", ACT_RECORD_PLAYBACK, FX_DALEK,  0, true, "Record + play back (robot, pitch TODO)", true },

#if CONFIG_BOARD_NUM_LINES > 1
    /* 3xx -- call the other phone line (symmetric on both lines). Only present
     * on a 2-line board; a single-line build has no other line to ring, so
     * these codes are omitted entirely (dialing 3xx -> wrong number) and the
     * web UI's inter-phone card/tab drop out with them. */
    { "321", ACT_CALL_LINE, -1, FX_NORMAL, false, "Call other phone (normal voice)", true },
    { "322", ACT_CALL_LINE, -1, FX_HIGH,   false, "Call other phone (chipmunk)", true },
    { "323", ACT_CALL_LINE, -1, FX_LOW,    false, "Call other phone (low pitch)", true },
    { "324", ACT_CALL_LINE, -1, FX_DALEK,  false, "Call other phone (robot)", true },
#endif

    /* 4xx -- silly audio clips (play mode) -- plays storage/clip_<N>.wav via
     * `player` if present, falls back to a confirm tone if it isn't */
    { "401", ACT_PLAY_CLIP, CLIP_RING_OF_FIRE, 0, true, "Ring of Fire / That's Amore", true },
    { "402", ACT_PLAY_CLIP, CLIP_BURP,         0, true, "Burp", true },
    { "403", ACT_PLAY_CLIP, CLIP_CROCODILE,    0, true, "Crocodile Rock / Crocfa", true },
    { "404", ACT_PLAY_CLIP, CLIP_MARTIANS,     0, true, "Martians", true },
    { "411", ACT_PLAY_CLIP, CLIP_BANANA_PHONE, 0, true, "Banana Phone", true },
    { "412", ACT_PLAY_CLIP, CLIP_BANANA_BOAT,  0, true, "Banana Boat", true },
    { "413", ACT_PLAY_CLIP, CLIP_BANANA_SPLIT, 0, true, "Banana Split", true },
    { "414", ACT_PLAY_CLIP, CLIP_LIME_COCONUT, 0, true, "Lime in the Coconut", true },

    /* 8xx -- internet radio (play mode); only 811/812 are configured on the
     * Pi today, 813-819 are documented gaps, not real codes yet */
    { "811", ACT_RADIO, 1, 0, true, "KXSF 102.5 SF alt-rock (KFOG successor)", false },
    { "812", ACT_RADIO, 2, 0, true, "KQED NPR 88.5", false },

    /* 9xx -- settings. No 901/902: language is set once at configuration
     * time (board_language_set), not switched in use -- see dialplan.h. */
    { "921", ACT_SET_MODE, MODE_READBACK_OFF, 0, false,
      "Digit readback off, default", true },
    { "922", ACT_SET_MODE, MODE_READBACK_ON,  0, false,
      "Digit readback on", true },
    { "931", ACT_SET_MODE, MODE_PLAY,         0, false,
      "Play mode, default (allow fun clips / record codes)", true },
    { "932", ACT_SET_MODE, MODE_CALL_ONLY,    0, false,
      "Call-only mode (blocks play-only codes)", true },
};
#define TABLE_N (sizeof(TABLE) / sizeof(TABLE[0]))

static dialplan_modes_t s_modes;

/* nvs_flash_init() is safe to call from more than one component -- the first
 * call wins, later ones are no-ops -- so this doesn't depend on whether
 * wifi_init() (the other caller) has run yet. dialplan_init() actually runs
 * before wifi_init() in app_main.c's boot order, so this can't assume it. */
static void modes_nvs_ready(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

/* s_modes was already zeroed by the caller -- on first boot (nothing saved
 * yet) or a corrupt/short blob, it's left at those defaults (play mode,
 * English, readback off), matching the Pi's own defaults. */
static void modes_load(void)
{
    nvs_handle_t h;
    if (nvs_open(MODES_NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    dialplan_modes_t loaded;
    size_t len = sizeof(loaded);
    if (nvs_get_blob(h, MODES_NVS_KEY, &loaded, &len) == ESP_OK && len == sizeof(loaded)) {
        s_modes = loaded;
    }
    nvs_close(h);
}

static void modes_save(void)
{
    nvs_handle_t h;
    if (nvs_open(MODES_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_blob(h, MODES_NVS_KEY, &s_modes, sizeof(s_modes)) == ESP_OK) {
        nvs_commit(h);
    }
    nvs_close(h);
}

void dialplan_init(void)
{
    memset(&s_modes, 0, sizeof(s_modes));
    modes_nvs_ready();
    modes_load();
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
            break;
        case MODE_CALL_ONLY:
            s_modes.call_only = true;
            break;
        case MODE_READBACK_OFF:
            s_modes.readback_digits = false;
            break;
        case MODE_READBACK_ON:
            s_modes.readback_digits = true;
            break;
    }
    modes_save();
    ESP_LOGI(TAG, "mode change %d (line %d): call_only=%d readback=%d",
             change, line, s_modes.call_only, s_modes.readback_digits);
}

static const entry_t *exact_match(const char *buf)
{
    for (size_t i = 0; i < TABLE_N; i++) {
        if (strcmp(TABLE[i].code, buf) == 0) return &TABLE[i];
    }
    return NULL;
}

bool dialplan_set_mode_by_code(const char *code, int line)
{
    const entry_t *e = exact_match(code);
    if (!e || e->kind != ACT_SET_MODE) return false;
    dialplan_apply_mode(line, (dialplan_mode_change_t)e->arg);
    return true;
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
    if (e->arg >= 0) return e->arg;      /* explicit target line */
    return (line == 0) ? 1 : 0;          /* -1: whichever line isn't dialing */
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

    /* Ambiguous shorter code (e.g. "0" while a longer code shared its prefix):
     * the timeout resolves it in favor of the shorter exact match. */
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

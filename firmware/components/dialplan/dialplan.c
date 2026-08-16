#include "dialplan.h"
#include "sdkconfig.h"
#include "board.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>

static const char *TAG = "dialplan";
#define MODES_NVS_NS  "phone"
#define MODES_NVS_KEY "modes"
#define CLIPS_NVS_NS       "dialclips"
#define CLIPS_NVS_KEY      "entries"
#define CLIPS_NVS_SEEDED   "seeded"

typedef struct {
    const char            *code;
    dialplan_action_kind_t kind;
    int                    arg;
    int                    arg2;
    const char            *desc;
    bool                   implemented; /* see dialplan_entry_info_t */
} entry_t;

/* Authoritative table (see ../pi_rotary_phone/docs/DIAL_CODES.md, matched
 * against the live phone_config.json on the Pi -- interphone dial codes are
 * symmetric 321-324 on both lines there, not the asymmetric 369/842 this
 * table used to guess at).
 * `implemented` reflects telephony.c's handle_action() as of this writing --
 * anything still just logging "(... TODO)" there and playing a confirm tone
 * is marked false. Flip it when you wire the real behavior in.
 * The "silly audio" clips (401-414) used to live here too, but they're now a
 * mutable code<->filename table (see dialplan_clip_entry_t / CLIP_DEFAULTS
 * below) so their dial codes and audio files can each be changed at runtime
 * from Settings -> Dial Codes without a rebuild. */
static const entry_t TABLE[] = {
    /* operator help -- plays storage/operator.wav via `player` if present,
     * falls back to a confirm tone if it isn't (see telephony.c). Code and
     * filename are both fixed -- replace the audio from Settings ->
     * Operator, not the Dial Codes tab. */
    { "0",   ACT_OPERATOR_HELP, 0, 0,
      "Operator help prompt", true },

    /* 2xx -- leave a message. 212-215 ("record + call back") lived here too
     * until it was archived -- see FEATURES.md "Archived features" and the
     * archive/record-and-callback git tag to revive it. */
    { "211", ACT_RECORD_ARCHIVE, 0, 0,
      "Leave a message (record to storage, download on web UI)", true },

    /* 3xx -- call the other phone line (symmetric on both lines). Always
     * compiled in now (board line count is runtime, board.h), but
     * entry_available() below hides these from lookup/listing on a board
     * currently configured for 1 line -- dialing 3xx -> wrong number, and
     * the web UI's inter-phone card/tab drop out with them, same observable
     * behavior as when this used to be #if CONFIG_BOARD_NUM_LINES > 1. */
    { "321", ACT_CALL_LINE, -1, FX_NORMAL, "Call other phone (normal voice)", true },
    { "322", ACT_CALL_LINE, -1, FX_HIGH,   "Call other phone (chipmunk)", true },
    { "323", ACT_CALL_LINE, -1, FX_LOW,    "Call other phone (low pitch)", true },
    { "324", ACT_CALL_LINE, -1, FX_DALEK,  "Call other phone (robot)", true },

    /* 8xx -- internet radio; only 811/812 are configured on the Pi today,
     * 813-819 are documented gaps, not real codes yet */
    { "811", ACT_RADIO, 1, 0, "KXSF 102.5 SF alt-rock (KFOG successor)", false },
    { "812", ACT_RADIO, 2, 0, "KQED NPR 88.5", false },

    /* 9xx -- settings. No 901/902: language is set once at configuration
     * time (board_language_set), not switched in use -- see dialplan.h. No
     * 931/932 either: Play/Call-only mode was removed as redundant (Play
     * mode already allowed everything Call-only did, just without the
     * restriction) -- the phone just always behaves like Play mode did. */
    { "921", ACT_SET_MODE, MODE_READBACK_OFF, 0,
      "Digit readback off, default", true },
    { "922", ACT_SET_MODE, MODE_READBACK_ON,  0,
      "Digit readback on", true },
};
#define TABLE_N (sizeof(TABLE) / sizeof(TABLE[0]))

/* One-time seed data for the mutable clip table (see dialplan_clip_entry_t
 * in dialplan.h) -- codes and filenames match what this project has always
 * shipped/documented, but from first boot on, both live in NVS and are
 * freely editable; this array is never consulted again after the first
 * seed. Builder-editable defaults live in dialplan_defaults.h, not here --
 * edit that file to change what a fresh build seeds. */
#include "dialplan_defaults.h"

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
 * yet) or a corrupt/short blob, it's left at those defaults (readback off),
 * matching the Pi's own defaults. */
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

/* Clip dial codes (Settings -> Dial Codes): a fixed-size table, empty slot =
 * code[0]=='\0'. Small and simple enough that a single NVS blob for the
 * whole array beats a per-entry key scheme. */
static dialplan_clip_entry_t s_clips[DIALPLAN_MAX_CLIP_ENTRIES];

static void clips_save(void)
{
    nvs_handle_t h;
    if (nvs_open(CLIPS_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_blob(h, CLIPS_NVS_KEY, s_clips, sizeof(s_clips)) == ESP_OK) {
        nvs_commit(h);
    }
    nvs_close(h);
}

/* Seeds the 8 builtin clips exactly once, gated on an explicit "seeded"
 * flag (not "is the table empty", since builtins are never deletable and
 * so should never legitimately reach zero -- relying on emptiness alone
 * would be an indirect trigger for something that deserves an explicit
 * one). The flag is written LAST, after the blob commit, so a power loss
 * mid-seed just looks like "not seeded yet" and safely re-seeds once more
 * next boot (idempotent: seeding assigns fixed code+filename pairs, it
 * doesn't append). */
static void clips_load(void)
{
    memset(s_clips, 0, sizeof(s_clips));
    nvs_handle_t h;
    if (nvs_open(CLIPS_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;

    size_t len = sizeof(s_clips);
    nvs_get_blob(h, CLIPS_NVS_KEY, s_clips, &len); /* short/missing -> stays zeroed */

    uint8_t seeded = 0;
    esp_err_t serr = nvs_get_u8(h, CLIPS_NVS_SEEDED, &seeded);
    if (serr != ESP_OK) {
        for (size_t i = 0; i < CLIP_DEFAULTS_N && i < DIALPLAN_MAX_CLIP_ENTRIES; i++) {
            strncpy(s_clips[i].code, CLIP_DEFAULTS[i].code, sizeof(s_clips[i].code) - 1);
            strncpy(s_clips[i].filename, CLIP_DEFAULTS[i].filename, sizeof(s_clips[i].filename) - 1);
            strncpy(s_clips[i].desc, CLIP_DEFAULTS[i].desc, sizeof(s_clips[i].desc) - 1);
            s_clips[i].builtin = true;
        }
        nvs_set_blob(h, CLIPS_NVS_KEY, s_clips, sizeof(s_clips));
        nvs_set_u8(h, CLIPS_NVS_SEEDED, 1);
        nvs_commit(h);
        ESP_LOGI(TAG, "seeded %u builtin clip codes", (unsigned)CLIP_DEFAULTS_N);
    }
    nvs_close(h);
}

void dialplan_init(void)
{
    memset(&s_modes, 0, sizeof(s_modes));
    modes_nvs_ready();
    modes_load();
    clips_load();
    ESP_LOGI(TAG, "loaded %u fixed codes", (unsigned)TABLE_N);
}

dialplan_modes_t *dialplan_modes(void) { return &s_modes; }
/* Not to be confused with ag1170's own (much shorter, ~300ms) interdigit_ms,
 * which decides when a train of rotary pulses adds up to one finished digit.
 * This one decides when the *whole number* is considered abandoned/complete
 * -- e.g. a partial 555-xxx (< 7 digits) that goes quiet this long is
 * declared a wrong number. 900ms was too tight for a human pausing between
 * digits (easy to trip while reading off an extension); real POTS interdigit
 * timeouts are several seconds, so this gives a caller room to pause. */
uint32_t dialplan_interdigit_ms(void) { return 15000; }

void dialplan_collector_reset(dialplan_collector_t *c)
{
    c->buf[0] = '\0';
    c->len = 0;
}

void dialplan_apply_mode(int line, dialplan_mode_change_t change)
{
    switch (change) {
        case MODE_READBACK_OFF:
            s_modes.readback_digits = false;
            break;
        case MODE_READBACK_ON:
            s_modes.readback_digits = true;
            break;
    }
    modes_save();
    ESP_LOGI(TAG, "mode change %d (line %d): readback=%d",
             change, line, s_modes.readback_digits);
}

/* ACT_CALL_LINE entries (321-324) stay in TABLE unconditionally (board line
 * count is a runtime setting now, board.h) but only "exist" for dialing/
 * listing purposes when a second line is actually configured -- matches
 * the observable behavior from when these were compiled out entirely on a
 * 1-line build. Everything else in TABLE is always available; only
 * ACT_CALL_LINE depends on board config. code_conflicts_except() below
 * deliberately does NOT use this gate -- 321-324 stay reserved against
 * custom clip codes even while hidden, so switching a board from 1 to 2
 * lines at runtime (no rebuild needed now) can't have silently let a custom
 * code claim them in the meantime. */
static bool entry_available(const entry_t *e)
{
    if (e->kind == ACT_CALL_LINE) return board_hw_config_get().num_lines > 1;
    return true;
}

static const entry_t *exact_match(const char *buf)
{
    for (size_t i = 0; i < TABLE_N; i++) {
        if (strcmp(TABLE[i].code, buf) == 0 && entry_available(&TABLE[i])) return &TABLE[i];
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
            strncmp(TABLE[i].code, buf, blen) == 0 &&
            entry_available(&TABLE[i])) {
            return true;
        }
    }
    return false;
}

/* ---- clip entries: same exact/prefix matching shape as the fixed table
 * above, kept as separate small helpers rather than merging into one loop
 * since the two live in different arrays with different element types. */
static const dialplan_clip_entry_t *clip_exact_match(const char *buf)
{
    for (int i = 0; i < DIALPLAN_MAX_CLIP_ENTRIES; i++) {
        if (s_clips[i].code[0] && strcmp(s_clips[i].code, buf) == 0) return &s_clips[i];
    }
    return NULL;
}

static bool clip_has_longer_prefix(const char *buf)
{
    size_t blen = strlen(buf);
    for (int i = 0; i < DIALPLAN_MAX_CLIP_ENTRIES; i++) {
        if (s_clips[i].code[0] && strlen(s_clips[i].code) > blen &&
            strncmp(s_clips[i].code, buf, blen) == 0) {
            return true;
        }
    }
    return false;
}

static dialplan_action_t fire_clip(const dialplan_clip_entry_t *cl)
{
    dialplan_action_t a = { .kind = ACT_PLAY_CLIP };
    strncpy(a.filename, cl->filename, sizeof(a.filename) - 1);
    return a;
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

    bool longer = has_longer_prefix(c->buf) || clip_has_longer_prefix(c->buf);

    const entry_t *e = exact_match(c->buf);
    if (e) {
        if (longer) return make(ACT_NONE, 0, 0); /* ambiguous: wait for timeout */
        return fire(e, line);
    }

    const dialplan_clip_entry_t *cl = clip_exact_match(c->buf);
    if (cl) {
        if (longer) return make(ACT_NONE, 0, 0);
        return fire_clip(cl);
    }

    /* no exact match: is the buffer still a viable prefix? */
    if (longer || (c->len < 3 && c->buf[0] == '5')) {
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

    const dialplan_clip_entry_t *cl = clip_exact_match(c->buf);
    if (cl) return fire_clip(cl);

    /* Still a viable prefix of a longer code (slow rotary inter-digit):
     * keep waiting rather than declaring a wrong number. */
    if (has_longer_prefix(c->buf) || clip_has_longer_prefix(c->buf)) {
        return make(ACT_NONE, 0, 0);
    }

    return make(ACT_WRONG_NUMBER, 0, 0);
}

size_t dialplan_list(dialplan_entry_info_t *out, size_t max)
{
    size_t n = 0;
    for (size_t i = 0; i < TABLE_N && n < max; i++) {
        if (!entry_available(&TABLE[i])) continue; /* e.g. 321-324 on a 1-line board */
        out[n].code = TABLE[i].code;
        out[n].desc = TABLE[i].desc;
        out[n].implemented = TABLE[i].implemented;
        n++;
    }
    return n;
}

/* True if `code` exactly matches, or is a prefix of / has as a prefix, any
 * existing code -- the fixed table, another clip entry (other than
 * `except_idx`, if >= 0 -- used when recoding an entry so it doesn't
 * conflict with itself), or the 555-xxxx VoIP prefix specifically (NOT every
 * code starting with '5': dialplan_feed() only special-cases "5"/"55"/
 * "555..." itself, so e.g. "501" is fine and must not be rejected here).
 * Comparing only the first min(lengths) characters catches both "equal" and
 * "one is a prefix of the other". */
static bool code_conflicts_except(const char *code, int except_idx)
{
    size_t clen = strlen(code);
    size_t vlen = (clen < 3) ? clen : 3;
    if (strncmp(code, "555", vlen) == 0) return true; /* shares a prefix with 555-xxxx */

    for (size_t i = 0; i < TABLE_N; i++) {
        size_t tlen = strlen(TABLE[i].code);
        size_t minlen = (clen < tlen) ? clen : tlen;
        if (strncmp(TABLE[i].code, code, minlen) == 0) return true;
    }
    for (int i = 0; i < DIALPLAN_MAX_CLIP_ENTRIES; i++) {
        if (i == except_idx || !s_clips[i].code[0]) continue;
        size_t ulen = strlen(s_clips[i].code);
        size_t minlen = (clen < ulen) ? clen : ulen;
        if (strncmp(s_clips[i].code, code, minlen) == 0) return true;
    }
    return false;
}

static bool valid_code(const char *code)
{
    size_t len = strlen(code);
    if (len < 2 || len > 6) return false;
    for (size_t i = 0; i < len; i++) {
        if (code[i] < '0' || code[i] > '9') return false;
    }
    return true;
}

static int clip_find_idx(const char *code)
{
    for (int i = 0; i < DIALPLAN_MAX_CLIP_ENTRIES; i++) {
        if (s_clips[i].code[0] && strcmp(s_clips[i].code, code) == 0) return i;
    }
    return -1;
}

esp_err_t dialplan_clip_add(const char *code, const char *filename, const char *desc)
{
    if (!code || !filename || !filename[0]) return ESP_ERR_INVALID_ARG;
    if (!valid_code(code)) return ESP_ERR_INVALID_ARG;
    if (code_conflicts_except(code, -1)) return ESP_ERR_INVALID_STATE;

    int slot = -1;
    for (int i = 0; i < DIALPLAN_MAX_CLIP_ENTRIES; i++) {
        if (!s_clips[i].code[0]) { slot = i; break; }
    }
    if (slot < 0) return ESP_ERR_NO_MEM;

    strncpy(s_clips[slot].code, code, sizeof(s_clips[slot].code) - 1);
    strncpy(s_clips[slot].filename, filename, sizeof(s_clips[slot].filename) - 1);
    if (desc) strncpy(s_clips[slot].desc, desc, sizeof(s_clips[slot].desc) - 1);
    s_clips[slot].builtin = false;
    clips_save();
    ESP_LOGI(TAG, "clip code added: %s -> %s", code, filename);
    return ESP_OK;
}

esp_err_t dialplan_clip_set_code(const char *old_code, const char *new_code)
{
    int idx = clip_find_idx(old_code);
    if (idx < 0) return ESP_ERR_NOT_FOUND;
    if (!valid_code(new_code)) return ESP_ERR_INVALID_ARG;
    if (strcmp(old_code, new_code) != 0 && code_conflicts_except(new_code, idx)) {
        return ESP_ERR_INVALID_STATE;
    }
    strncpy(s_clips[idx].code, new_code, sizeof(s_clips[idx].code) - 1);
    s_clips[idx].code[sizeof(s_clips[idx].code) - 1] = '\0';
    clips_save();
    ESP_LOGI(TAG, "clip code recoded: %s -> %s (%s)", old_code, new_code, s_clips[idx].filename);
    return ESP_OK;
}

esp_err_t dialplan_clip_set_filename(const char *code, const char *filename)
{
    if (!filename || !filename[0]) return ESP_ERR_INVALID_ARG;
    int idx = clip_find_idx(code);
    if (idx < 0) return ESP_ERR_NOT_FOUND;
    strncpy(s_clips[idx].filename, filename, sizeof(s_clips[idx].filename) - 1);
    s_clips[idx].filename[sizeof(s_clips[idx].filename) - 1] = '\0';
    clips_save();
    ESP_LOGI(TAG, "clip code %s now plays %s", code, filename);
    return ESP_OK;
}

esp_err_t dialplan_clip_remove(const char *code)
{
    int idx = clip_find_idx(code);
    if (idx < 0) return ESP_ERR_NOT_FOUND;
    if (s_clips[idx].builtin) return ESP_ERR_INVALID_STATE;
    ESP_LOGI(TAG, "clip code removed: %s (was -> %s)", code, s_clips[idx].filename);
    memset(&s_clips[idx], 0, sizeof(s_clips[idx]));
    clips_save();
    return ESP_OK;
}

esp_err_t dialplan_clip_get_filename(const char *code, char *out, size_t out_len)
{
    int idx = clip_find_idx(code);
    if (idx < 0) return ESP_ERR_NOT_FOUND;
    strncpy(out, s_clips[idx].filename, out_len - 1);
    out[out_len - 1] = '\0';
    return ESP_OK;
}

size_t dialplan_list_clip_entries(dialplan_clip_entry_t *out, size_t max)
{
    size_t n = 0;
    for (int i = 0; i < DIALPLAN_MAX_CLIP_ENTRIES && n < max; i++) {
        if (s_clips[i].code[0]) out[n++] = s_clips[i];
    }
    return n;
}

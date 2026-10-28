#include "dialplan.h"
#include "sdkconfig.h"
#include "board.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>

static const char *TAG = "dialplan";
#define MODES_NVS_NS  "phone"
#define MODES_NVS_KEY "modes"
#define CLIPS_NVS_NS       "dialclips"
/* One small blob per slot ("c00".."c23"), not one table-sized blob: NVS wants
 * a whole free stretch of a page for a blob that size, and on a board whose
 * settings area has filled up the write quietly failed, losing the table.
 * "entries" is the old single-blob table (code[8]), converted at first boot. */
#define CLIPS_NVS_KEY_V1   "entries"
#define CLIPS_NVS_SEEDED   "seeded"
#define CLIPS_NVS_TABLE_SAVED "tbl"     /* table has been written at least once */
#define CLIPS_NVS_SEEDED_N "seeded_n"   /* how many CLIP_DEFAULTS entries this board has had */
/* Entries CLIP_DEFAULTS had before the counter above existed: a board seeded
 * back then has exactly these, and gets any later ones appended at boot. */
#define CLIP_DEFAULTS_LEGACY_N 1
#define PINS_NVS_NS        "dialpins"
#define PINS_NVS_KEY_V1    "entries"    /* old single blob; slots are now "p0".."p7" */

/* The saved layouts from before codes could be 10 digits (code[8]). Field
 * order and types must stay exactly as they were, since they're read back
 * byte for byte. */
typedef struct { char code[8]; char filename[32]; char desc[64]; bool builtin; } clip_entry_v1_t;
typedef struct { char code[8]; uint8_t gpio; uint16_t secs; char desc[40]; } pin_entry_v1_t;

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

    /* 2xx -- leave a message / answering machine. 212-215 ("record
     * greeting" and "record + call back") lived here too until archived --
     * see docs/features.md "Archived features" and the
     * archive/record-and-callback git tag to revive 213-215. The
     * answering-machine greeting recorder (originally 212, part of that
     * same archived range) moved to 999 so it can't be dialed by accident
     * in place of 211 -- see archive/ANSWERING_MACHINE.md. */
    { "211", ACT_RECORD_ARCHIVE, 0, 0,
      "Leave a message (record to storage, download on web UI)", true },

    /* 3xx -- call the other phone line (symmetric on both lines). Always
     * compiled in now (board line count is runtime, board.h), but
     * entry_available() below hides these from lookup/listing on a board
     * currently configured for 1 line -- dialing 3xx -> wrong number, and
     * the web UI's inter-phone card/tab drop out with them, same observable
     * behavior as when this used to be #if CONFIG_BOARD_NUM_LINES > 1. */
    { "314", ACT_PI_DIGITS, 0, 0, "Read pi aloud (first 100 decimals)", true },
    /* 846 spells TIM on the keypad. */
    { "846", ACT_TIMER, 0, 0, "Timer: say how many minutes (1-99, 0 cancels); the phone rings when it's up", true },
    /* 256 spells ALM. */
    { "256", ACT_ALARM, 0, 0, "Alarm: dial the time as four digits on a 24 hour clock (0730 = 7:30 am; 99 cancels); the phone rings then", true },
    { "321", ACT_CALL_LINE, -1, FX_NORMAL, "Call other phone (normal voice)", true },
    { "322", ACT_CALL_LINE, -1, FX_HIGH,   "Call other phone (chipmunk)", true },
    { "323", ACT_CALL_LINE, -1, FX_LOW,    "Call other phone (Vader)", true },
    { "324", ACT_CALL_LINE, -1, FX_DALEK,  "Call other phone (Dalek)", true },

    /* 8xx holds the mode/greeting codes that used to be 9xx (921/922/999,
     * moved 2026-10-01). The whole 9xx block is reserved: "9" is the prefix
     * for Bluetooth dial-out, PBX-style ("9 for an outside line"), and a code
     * starting with 9 would be indistinguishable from the start of a phone
     * number. */
    /* 9xx -- settings. No 901/902: language is set once at configuration
     * time (board_language_set), not switched in use -- see dialplan.h. No
     * 931/932 either: Play/Call-only mode was removed as redundant (Play
     * mode already allowed everything Call-only did, just without the
     * restriction) -- the phone just always behaves like Play mode did. */
    { "821", ACT_SET_MODE, MODE_READBACK_OFF, 0,
      "Digit readback off, default", true },
    { "822", ACT_SET_MODE, MODE_READBACK_ON,  0,
      "Digit readback on", true },
    /* Was 212 (see the 2xx block's comment above) -- moved out here so it
     * can't be dialed by accident in place of 211 (Leave a message), the
     * actual mistake this was renumbered to avoid. */
    { "899", ACT_RECORD_GREETING, 0, 0,
      "Record this line's answering-machine greeting", true },
};
#define TABLE_N (sizeof(TABLE) / sizeof(TABLE[0]))

/* What each TABLE entry answers to right now: its default code unless it was
 * changed from Settings -> Dial Codes (NVS "dialfixed", key "f" + default). */
#define FIXED_NVS_NS "dialfixed"
static char s_fixed_code[TABLE_N][DIALPLAN_CODE_MAX_DIGITS + 2];
static const char *fcode(size_t i) { return s_fixed_code[i]; }

static void fixed_load(void)
{
    nvs_handle_t h;
    bool open = nvs_open(FIXED_NVS_NS, NVS_READONLY, &h) == ESP_OK;
    for (size_t i = 0; i < TABLE_N; i++) {
        snprintf(s_fixed_code[i], sizeof(s_fixed_code[i]), "%s", TABLE[i].code);
        if (!open) continue;
        char key[16], val[sizeof(s_fixed_code[i])];
        size_t len = sizeof(val);
        snprintf(key, sizeof(key), "f%s", TABLE[i].code);
        if (nvs_get_str(h, key, val, &len) == ESP_OK && val[0]) {
            snprintf(s_fixed_code[i], sizeof(s_fixed_code[i]), "%s", val);
        }
    }
    if (open) nvs_close(h);
}

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

/* Writes every slot (empty ones are erased). Returns the first error, after
 * trying them all, so one failure doesn't cost the rest. */
static esp_err_t clips_write(nvs_handle_t h)
{
    esp_err_t err = ESP_OK;
    char key[8];
    for (int i = 0; i < DIALPLAN_MAX_CLIP_ENTRIES; i++) {
        snprintf(key, sizeof(key), "c%02d", i);
        if (s_clips[i].code[0]) {
            esp_err_t e = nvs_set_blob(h, key, &s_clips[i], sizeof(s_clips[i]));
            if (e != ESP_OK) {
                ESP_LOGE(TAG, "saving dial code slot %d failed: %s", i, esp_err_to_name(e));
                if (err == ESP_OK) err = e;
            }
        } else {
            nvs_erase_key(h, key);
        }
    }
    if (err == ESP_OK) nvs_set_u8(h, CLIPS_NVS_TABLE_SAVED, 1);   /* an empty table from here on is deliberate */
    esp_err_t c = nvs_commit(h);
    return err != ESP_OK ? err : c;
}

static esp_err_t clips_save(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(CLIPS_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "can't open the dial code store: %s", esp_err_to_name(err));
        return err;
    }
    err = clips_write(h);
    nvs_close(h);
    return err;
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

    int found = 0;
    char key[8];
    for (int i = 0; i < DIALPLAN_MAX_CLIP_ENTRIES; i++) {
        snprintf(key, sizeof(key), "c%02d", i);
        size_t len = sizeof(s_clips[i]);
        if (nvs_get_blob(h, key, &s_clips[i], &len) == ESP_OK && len == sizeof(s_clips[i])) found++;
        else memset(&s_clips[i], 0, sizeof(s_clips[i]));
    }

    /* Saved by firmware from before the per-slot layout and 10-digit codes:
     * one blob of narrower entries. The old copy is deleted only once the new
     * one is safely written, so a failed conversion can simply retry. */
    size_t oldlen = 0;
    if (!found && nvs_get_blob(h, CLIPS_NVS_KEY_V1, NULL, &oldlen) == ESP_OK && oldlen > 0) {
        clip_entry_v1_t *old = (oldlen % sizeof(clip_entry_v1_t) == 0) ? malloc(oldlen) : NULL;
        if (old && nvs_get_blob(h, CLIPS_NVS_KEY_V1, old, &oldlen) == ESP_OK) {
            size_t n = oldlen / sizeof(clip_entry_v1_t);
            for (size_t i = 0; i < n && i < DIALPLAN_MAX_CLIP_ENTRIES; i++) {
                memcpy(s_clips[i].code, old[i].code, sizeof(old[i].code));
                memcpy(s_clips[i].filename, old[i].filename, sizeof(old[i].filename));
                memcpy(s_clips[i].desc, old[i].desc, sizeof(old[i].desc));
                s_clips[i].builtin = old[i].builtin;
                if (s_clips[i].code[0]) found++;
            }
            if (clips_write(h) == ESP_OK) {
                nvs_erase_key(h, CLIPS_NVS_KEY_V1);
                nvs_commit(h);
                ESP_LOGI(TAG, "converted %d saved dial codes to the new layout", found);
            } else {
                ESP_LOGE(TAG, "couldn't save the converted dial codes; the old copy is kept "
                              "and will be tried again next boot");
            }
        } else {
            ESP_LOGW(TAG, "saved dial codes (%u bytes) are from an incompatible firmware",
                     (unsigned)oldlen);
        }
        free(old);
    }
    for (int i = 0; i < DIALPLAN_MAX_CLIP_ENTRIES; i++) {
        s_clips[i].code[sizeof(s_clips[i].code) - 1] = '\0';
        s_clips[i].filename[sizeof(s_clips[i].filename) - 1] = '\0';
        s_clips[i].desc[sizeof(s_clips[i].desc) - 1] = '\0';
    }

    /* First boot, or an empty table that was never saved empty on purpose
     * (a lost table): put the defaults in. */
    uint8_t seeded = 0, saved_once = 0;
    esp_err_t serr = nvs_get_u8(h, CLIPS_NVS_SEEDED, &seeded);
    nvs_get_u8(h, CLIPS_NVS_TABLE_SAVED, &saved_once);
    if (serr != ESP_OK || (found == 0 && !saved_once)) {
        for (size_t i = 0; i < CLIP_DEFAULTS_N && i < DIALPLAN_MAX_CLIP_ENTRIES; i++) {
            strncpy(s_clips[i].code, CLIP_DEFAULTS[i].code, sizeof(s_clips[i].code) - 1);
            strncpy(s_clips[i].filename, CLIP_DEFAULTS[i].filename, sizeof(s_clips[i].filename) - 1);
            strncpy(s_clips[i].desc, CLIP_DEFAULTS[i].desc, sizeof(s_clips[i].desc) - 1);
            s_clips[i].builtin = true;
        }
        if (clips_write(h) == ESP_OK) {
            nvs_set_u8(h, CLIPS_NVS_SEEDED, 1);
            nvs_set_u8(h, CLIPS_NVS_SEEDED_N, (uint8_t)CLIP_DEFAULTS_N);
            nvs_commit(h);
        }
        ESP_LOGI(TAG, "%s %u builtin clip codes", serr != ESP_OK ? "seeded" : "restored",
                 (unsigned)CLIP_DEFAULTS_N);
    }
    nvs_close(h);
}

/* Pin dial codes: a small fixed table, empty slot = code[0] == '\0'. */
static dialplan_pin_entry_t s_pin_actions[DIALPLAN_MAX_PIN_ACTIONS];

static esp_err_t pins_write(nvs_handle_t h)
{
    esp_err_t err = ESP_OK;
    char key[8];
    for (int i = 0; i < DIALPLAN_MAX_PIN_ACTIONS; i++) {
        snprintf(key, sizeof(key), "p%d", i);
        if (s_pin_actions[i].code[0]) {
            esp_err_t e = nvs_set_blob(h, key, &s_pin_actions[i], sizeof(s_pin_actions[i]));
            if (e != ESP_OK) {
                ESP_LOGE(TAG, "saving pin code slot %d failed: %s", i, esp_err_to_name(e));
                if (err == ESP_OK) err = e;
            }
        } else {
            nvs_erase_key(h, key);
        }
    }
    esp_err_t c = nvs_commit(h);
    return err != ESP_OK ? err : c;
}

static esp_err_t pins_save(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(PINS_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = pins_write(h);
    nvs_close(h);
    return err;
}

static void pins_load(void)
{
    memset(s_pin_actions, 0, sizeof(s_pin_actions));
    nvs_handle_t h;
    if (nvs_open(PINS_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    int found = 0;
    char key[8];
    for (int i = 0; i < DIALPLAN_MAX_PIN_ACTIONS; i++) {
        snprintf(key, sizeof(key), "p%d", i);
        size_t len = sizeof(s_pin_actions[i]);
        if (nvs_get_blob(h, key, &s_pin_actions[i], &len) == ESP_OK && len == sizeof(s_pin_actions[i])) found++;
        else memset(&s_pin_actions[i], 0, sizeof(s_pin_actions[i]));
    }
    /* The old single blob (4 or 8 entries of the narrower layout). */
    size_t oldlen = 0;
    if (!found && nvs_get_blob(h, PINS_NVS_KEY_V1, NULL, &oldlen) == ESP_OK && oldlen > 0 &&
        oldlen % sizeof(pin_entry_v1_t) == 0 && oldlen <= DIALPLAN_MAX_PIN_ACTIONS * sizeof(pin_entry_v1_t)) {
        pin_entry_v1_t old[DIALPLAN_MAX_PIN_ACTIONS];
        if (nvs_get_blob(h, PINS_NVS_KEY_V1, old, &oldlen) == ESP_OK) {
            for (size_t i = 0; i < oldlen / sizeof(old[0]); i++) {
                memcpy(s_pin_actions[i].code, old[i].code, sizeof(old[i].code));
                s_pin_actions[i].gpio = old[i].gpio;
                s_pin_actions[i].secs = old[i].secs;
                memcpy(s_pin_actions[i].desc, old[i].desc, sizeof(old[i].desc));
            }
            if (pins_write(h) == ESP_OK) {
                nvs_erase_key(h, PINS_NVS_KEY_V1);
                nvs_commit(h);
                ESP_LOGI(TAG, "converted saved pin codes to the new layout");
            } else {
                ESP_LOGE(TAG, "couldn't save the converted pin codes; the old copy is kept");
            }
        }
    }
    nvs_close(h);
    for (int i = 0; i < DIALPLAN_MAX_PIN_ACTIONS; i++) {
        s_pin_actions[i].code[sizeof(s_pin_actions[i].code) - 1] = '\0';
        s_pin_actions[i].desc[sizeof(s_pin_actions[i].desc) - 1] = '\0';
        if (s_pin_actions[i].secs > 3600) memset(&s_pin_actions[i], 0, sizeof(s_pin_actions[i]));
    }
}

static void clips_add_new_defaults(void);

void dialplan_init(void)
{
    memset(&s_modes, 0, sizeof(s_modes));
    modes_nvs_ready();
    modes_load();
    fixed_load();
    clips_load();
    pins_load();
    clips_add_new_defaults();
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

bool dialplan_pins_active(void)
{
    board_hw_config_t cfg = board_hw_config_get();
    return cfg.num_lines == 1 && !cfg.has_microsd;
}

/* A pin code is live while the pins are free; a Home Assistant code always. */
static bool pin_entry_live(const dialplan_pin_entry_t *p, bool pins_active)
{
    return p->code[0] && (p->gpio == DIALPLAN_PIN_NONE || pins_active);
}

static const dialplan_pin_entry_t *pin_exact_match(const char *buf)
{
    bool active = dialplan_pins_active();
    for (int i = 0; i < DIALPLAN_MAX_PIN_ACTIONS; i++) {
        if (pin_entry_live(&s_pin_actions[i], active) && strcmp(s_pin_actions[i].code, buf) == 0)
            return &s_pin_actions[i];
    }
    return NULL;
}

static bool pin_has_longer_prefix(const char *buf)
{
    bool active = dialplan_pins_active();
    size_t blen = strlen(buf);
    for (int i = 0; i < DIALPLAN_MAX_PIN_ACTIONS; i++) {
        if (pin_entry_live(&s_pin_actions[i], active) && strlen(s_pin_actions[i].code) > blen &&
            strncmp(s_pin_actions[i].code, buf, blen) == 0) {
            return true;
        }
    }
    return false;
}

static dialplan_action_t fire_pin(const dialplan_pin_entry_t *p)
{
    dialplan_action_t a = { .kind = ACT_GPIO_OUT, .arg = p->gpio, .arg2 = p->secs };
    strncpy(a.filename, p->desc, sizeof(a.filename) - 1);
    return a;
}

static const entry_t *exact_match(const char *buf)
{
    for (size_t i = 0; i < TABLE_N; i++) {
        if (strcmp(fcode(i), buf) == 0 && entry_available(&TABLE[i])) return &TABLE[i];
    }
    return NULL;
}

bool dialplan_set_mode_by_code(const char *code, int line)
{
    const entry_t *e = NULL;
    for (size_t i = 0; i < TABLE_N && !e; i++) {
        if (TABLE[i].kind == ACT_SET_MODE &&
            (strcmp(TABLE[i].code, code) == 0 || strcmp(fcode(i), code) == 0)) e = &TABLE[i];
    }
    if (!e) return false;
    dialplan_apply_mode(line, (dialplan_mode_change_t)e->arg);
    return true;
}

static bool has_longer_prefix(const char *buf)
{
    size_t blen = strlen(buf);
    for (size_t i = 0; i < TABLE_N; i++) {
        if (strlen(fcode(i)) > blen &&
            strncmp(fcode(i), buf, blen) == 0 &&
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

static bool s_bt_dialout;

void dialplan_set_bt_dialout(bool on) { s_bt_dialout = on; }

/* ---- home-network numbers ---- */
static portMUX_TYPE s_home_lock = portMUX_INITIALIZER_UNLOCKED;
static char   s_home[DIALPLAN_HOME_MAX][DIALPLAN_HOME_NUM_MAX];
static size_t s_home_n;
static char   s_home_own[2][DIALPLAN_HOME_NUM_MAX];

void dialplan_set_home_numbers(const char (*numbers)[DIALPLAN_HOME_NUM_MAX], size_t n)
{
    if (n > DIALPLAN_HOME_MAX) n = DIALPLAN_HOME_MAX;
    portENTER_CRITICAL(&s_home_lock);
    for (size_t i = 0; i < n; i++) strlcpy(s_home[i], numbers[i], DIALPLAN_HOME_NUM_MAX);
    s_home_n = n;
    portEXIT_CRITICAL(&s_home_lock);
}

void dialplan_set_own_home_numbers(const char *line0, const char *line1)
{
    portENTER_CRITICAL(&s_home_lock);
    strlcpy(s_home_own[0], line0 ? line0 : "", DIALPLAN_HOME_NUM_MAX);
    strlcpy(s_home_own[1], line1 ? line1 : "", DIALPLAN_HOME_NUM_MAX);
    portEXIT_CRITICAL(&s_home_lock);
}

/* 1 = exact match, 2 = a longer number starts with buf (keep collecting). */
static int home_match(const char *buf)
{
    int r = 0;
    size_t bl = strlen(buf);
    portENTER_CRITICAL(&s_home_lock);
    for (size_t i = 0; i < s_home_n; i++) {
        if (strcmp(s_home[i], buf) == 0) r |= 1;
        else if (strlen(s_home[i]) > bl && strncmp(s_home[i], buf, bl) == 0) r |= 2;
    }
    portEXIT_CRITICAL(&s_home_lock);
    return r;
}

static dialplan_action_t fire_home(const char *buf)
{
    dialplan_action_t a = { .kind = ACT_HOME_CALL };
    strlcpy(a.number, buf, sizeof(a.number));
    return a;
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

    /* Bluetooth dial-out: 9 + 10 digits (see dialplan_set_bt_dialout()). */
    if (s_bt_dialout && c->buf[0] == DIALPLAN_BT_PREFIX) {
        if (c->len >= DIALPLAN_BT_DIGITS) {
            dialplan_action_t a = make(ACT_BT_DIAL, 0, 0);
            strncpy(a.number, c->buf + 1, sizeof(a.number) - 1);   /* drop the 9 */
            return a;
        }
        return make(ACT_NONE, 0, 0);
    }

    int home = home_match(c->buf);
    bool longer = has_longer_prefix(c->buf) || clip_has_longer_prefix(c->buf) ||
                  pin_has_longer_prefix(c->buf) || (home & 2);

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

    const dialplan_pin_entry_t *pa = pin_exact_match(c->buf);
    if (pa) {
        if (longer) return make(ACT_NONE, 0, 0);
        return fire_pin(pa);
    }

    if (home & 1) {
        if (longer) return make(ACT_NONE, 0, 0);
        return fire_home(c->buf);
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

    const dialplan_pin_entry_t *pa = pin_exact_match(c->buf);
    if (pa) return fire_pin(pa);

    int home = home_match(c->buf);
    if (home & 1) return fire_home(c->buf);

    /* Still a viable prefix of a longer code (slow rotary inter-digit):
     * keep waiting rather than declaring a wrong number. */
    if (has_longer_prefix(c->buf) || clip_has_longer_prefix(c->buf) ||
        pin_has_longer_prefix(c->buf) || (home & 2)) {
        return make(ACT_NONE, 0, 0);
    }

    return make(ACT_WRONG_NUMBER, 0, 0);
}

static dialplan_group_t group_for(dialplan_action_kind_t k)
{
    switch (k) {
    case ACT_CALL_LINE:       return DIALPLAN_GROUP_CALLS;
    case ACT_PI_DIGITS:
    case ACT_TIMER:
    case ACT_ALARM:           return DIALPLAN_GROUP_CLIPS;
    case ACT_SET_MODE:        return DIALPLAN_GROUP_SETTINGS;
    default:                  return DIALPLAN_GROUP_MESSAGES;   /* operator, record, greeting */
    }
}

size_t dialplan_list(dialplan_entry_info_t *out, size_t max)
{
    size_t n = 0;
    for (size_t i = 0; i < TABLE_N && n < max; i++) {
        if (!entry_available(&TABLE[i])) continue; /* e.g. 321-324 on a 1-line board */
        out[n].code = fcode(i);
        out[n].default_code = TABLE[i].code;
        out[n].desc = TABLE[i].desc;
        out[n].implemented = TABLE[i].implemented;
        out[n].group = group_for(TABLE[i].kind);
        n++;
    }
    return n;
}

size_t dialplan_list_fixed(dialplan_fixed_info_t *out, size_t max)
{
    size_t n = 0;
    for (size_t i = 0; i < TABLE_N && n < max; i++, n++) {
        out[n].info.code = fcode(i);
        out[n].info.default_code = TABLE[i].code;
        out[n].info.desc = TABLE[i].desc;
        out[n].info.implemented = TABLE[i].implemented;
        out[n].info.group = group_for(TABLE[i].kind);
        out[n].available = entry_available(&TABLE[i]);
    }
    return n;
}

static bool valid_code(const char *code);
static bool code_conflicts_fx(const char *code, int except_idx, int except_pin, int except_fixed);

esp_err_t dialplan_fixed_set_code(const char *default_code, const char *new_code)
{
    int idx = -1;
    for (size_t i = 0; i < TABLE_N; i++) if (strcmp(TABLE[i].code, default_code) == 0) idx = (int)i;
    if (idx < 0) return ESP_ERR_NOT_FOUND;
    const char *want = (new_code && new_code[0]) ? new_code : TABLE[idx].code;
    bool is_default = strcmp(want, TABLE[idx].code) == 0;
    if (!is_default && !valid_code(want)) return ESP_ERR_INVALID_ARG;
    if (!is_default && code_conflicts_fx(want, -1, -1, idx)) return ESP_ERR_INVALID_STATE;

    nvs_handle_t h;
    esp_err_t err = nvs_open(FIXED_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    char key[16];
    snprintf(key, sizeof(key), "f%s", TABLE[idx].code);
    if (is_default) { nvs_erase_key(h, key); err = ESP_OK; }
    else            err = nvs_set_str(h, key, want);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) return err;
    snprintf(s_fixed_code[idx], sizeof(s_fixed_code[idx]), "%s", want);
    ESP_LOGI(TAG, "built-in %s (%s) now answers to %s", TABLE[idx].code, TABLE[idx].desc, want);
    return ESP_OK;
}

/* True if `code` exactly matches, or is a prefix of / has as a prefix, any
 * existing code -- the fixed table, another clip entry (other than
 * `except_idx`, if >= 0 -- used when recoding an entry so it doesn't
 * conflict with itself), or the 555-xxxx VoIP prefix specifically (NOT every
 * code starting with '5': dialplan_feed() only special-cases "5"/"55"/
 * "555..." itself, so e.g. "501" is fine and must not be rejected here).
 * Comparing only the first min(lengths) characters catches both "equal" and
 * "one is a prefix of the other". */
static bool code_conflicts_fx(const char *code, int except_idx, int except_pin, int except_fixed);
static bool code_conflicts_ex(const char *code, int except_idx, int except_pin)
{
    return code_conflicts_fx(code, except_idx, except_pin, -1);
}
static bool code_conflicts_except(const char *code, int except_idx)
{
    return code_conflicts_ex(code, except_idx, -1);
}

static bool code_conflicts_fx(const char *code, int except_idx, int except_pin, int except_fixed)
{
    size_t clen = strlen(code);
    size_t vlen = (clen < 3) ? clen : 3;
    if (strncmp(code, "555", vlen) == 0) return true; /* shares a prefix with 555-xxxx */
    if (code[0] == DIALPLAN_BT_PREFIX) return true;   /* reserved for Bluetooth dial-out */

    for (size_t i = 0; i < TABLE_N; i++) {
        if ((int)i == except_fixed) continue;
        size_t tlen = strlen(fcode(i));
        size_t minlen = (clen < tlen) ? clen : tlen;
        if (strncmp(fcode(i), code, minlen) == 0) return true;
    }
    for (int i = 0; i < DIALPLAN_MAX_CLIP_ENTRIES; i++) {
        if (i == except_idx || !s_clips[i].code[0]) continue;
        size_t ulen = strlen(s_clips[i].code);
        size_t minlen = (clen < ulen) ? clen : ulen;
        if (strncmp(s_clips[i].code, code, minlen) == 0) return true;
    }
    for (int i = 0; i < DIALPLAN_MAX_PIN_ACTIONS; i++) {
        if (i == except_pin || !s_pin_actions[i].code[0]) continue;
        size_t plen = strlen(s_pin_actions[i].code);
        size_t minlen = (clen < plen) ? clen : plen;
        if (strncmp(s_pin_actions[i].code, code, minlen) == 0) return true;
    }
    bool own = false;
    portENTER_CRITICAL(&s_home_lock);
    for (int i = 0; i < 2; i++) {
        size_t hl = strlen(s_home_own[i]);
        if (hl && strncmp(s_home_own[i], code, (clen < hl) ? clen : hl) == 0) own = true;
    }
    portEXIT_CRITICAL(&s_home_lock);
    return own;
}

const char *dialplan_home_number_problem(const char *number)
{
    size_t n = strlen(number);
    if (n < 3 || n >= DIALPLAN_HOME_NUM_MAX) return "Use 3 to 11 digits";
    for (size_t i = 0; i < n; i++) if (number[i] < '0' || number[i] > '9') return "Digits only";
    if (number[0] == '0') return "Can't start with 0 (that's the operator)";
    if (strncmp(number, "555", 3) == 0) return "Can't start with 555 (VoIP server calls use it)";
    /* Not just while a digit is being dialled on the Bluetooth line
     * (s_bt_dialout): on a board with a module, 1 + digits always means
     * dial-out on that line. */
    if ((s_bt_dialout || board_hw_config_get().has_bt_module) && number[0] == DIALPLAN_BT_PREFIX)
        return "Can't start with 9 on a board with Bluetooth (it dials out with 9)";
    for (size_t i = 0; i < TABLE_N; i++) {
        size_t tl = strlen(fcode(i));
        if (strncmp(fcode(i), number, (n < tl) ? n : tl) == 0) return "Clashes with one of this phone's dial codes";
    }
    for (int i = 0; i < DIALPLAN_MAX_CLIP_ENTRIES; i++) {
        size_t ul = strlen(s_clips[i].code);
        if (ul && strncmp(s_clips[i].code, number, (n < ul) ? n : ul) == 0) return "Clashes with one of this phone's dial codes";
    }
    for (int i = 0; i < DIALPLAN_MAX_PIN_ACTIONS; i++) {
        size_t pl = strlen(s_pin_actions[i].code);
        if (pl && strncmp(s_pin_actions[i].code, number, (n < pl) ? n : pl) == 0) return "Clashes with one of this phone's dial codes";
    }
    return NULL;
}

void dialplan_code_normalize(char *code)
{
    char *w = code;
    for (const char *r = code; *r; r++) {
        if (*r == ' ' || *r == '-' || *r == '.' || *r == '(' || *r == ')') continue;
        *w++ = *r;
    }
    *w = '\0';
}

/* A default added to CLIP_DEFAULTS after a board was first set up never
 * reached it (seeding runs once). Add each such entry, if its code is free
 * and there's a slot; either way remember it was offered, so a user who later
 * recodes it doesn't get it back. Fresh boards already have them all. */
static void clips_add_new_defaults(void)
{
    nvs_handle_t h;
    if (nvs_open(CLIPS_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    uint8_t have = 0;
    if (nvs_get_u8(h, CLIPS_NVS_SEEDED_N, &have) != ESP_OK) have = CLIP_DEFAULTS_LEGACY_N;
    nvs_close(h);
    bool changed = false;
    for (size_t i = have; i < CLIP_DEFAULTS_N; i++) {
        const clip_default_t *d = &CLIP_DEFAULTS[i];
        int slot = -1;
        for (int k = 0; k < DIALPLAN_MAX_CLIP_ENTRIES; k++) {
            if (!s_clips[k].code[0]) { slot = k; break; }
        }
        if (slot < 0 || code_conflicts_ex(d->code, -1, -1)) {
            ESP_LOGW(TAG, "new default %s (%s) not added: %s", d->code, d->filename,
                     slot < 0 ? "no free slot" : "code already in use");
            continue;
        }
        strncpy(s_clips[slot].code, d->code, sizeof(s_clips[slot].code) - 1);
        strncpy(s_clips[slot].filename, d->filename, sizeof(s_clips[slot].filename) - 1);
        strncpy(s_clips[slot].desc, d->desc, sizeof(s_clips[slot].desc) - 1);
        s_clips[slot].builtin = true;
        changed = true;
        ESP_LOGI(TAG, "added new default clip code %s (%s)", d->code, d->filename);
    }
    if (changed && clips_save() != ESP_OK) return;   /* try again next boot */
    if (have != CLIP_DEFAULTS_N && nvs_open(CLIPS_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, CLIPS_NVS_SEEDED_N, (uint8_t)CLIP_DEFAULTS_N);
        nvs_commit(h);
        nvs_close(h);
    }
}

static bool valid_code(const char *code)
{
    size_t len = strlen(code);
    if (len < 2 || len > DIALPLAN_CODE_MAX_DIGITS) return false;
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

esp_err_t dialplan_clip_check_new(const char *code)
{
    if (!code || !valid_code(code)) return ESP_ERR_INVALID_ARG;
    if (code_conflicts_except(code, -1)) return ESP_ERR_INVALID_STATE;
    for (int i = 0; i < DIALPLAN_MAX_CLIP_ENTRIES; i++) {
        if (!s_clips[i].code[0]) return ESP_OK;
    }
    return ESP_ERR_NO_MEM;
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

esp_err_t dialplan_clip_set_desc(const char *code, const char *desc)
{
    int idx = clip_find_idx(code);
    if (idx < 0) return ESP_ERR_NOT_FOUND;
    strncpy(s_clips[idx].desc, desc ? desc : "", sizeof(s_clips[idx].desc) - 1);
    s_clips[idx].desc[sizeof(s_clips[idx].desc) - 1] = '\0';
    clips_save();
    ESP_LOGI(TAG, "clip code %s relabeled: %s", code, s_clips[idx].desc);
    return ESP_OK;
}

esp_err_t dialplan_clip_set_filename(const char *code, const char *filename)
{
    int idx = clip_find_idx(code);
    if (idx < 0) return ESP_ERR_NOT_FOUND;
    if (!filename || !filename[0] || strlen(filename) >= sizeof(s_clips[idx].filename))
        return ESP_ERR_INVALID_ARG;
    strncpy(s_clips[idx].filename, filename, sizeof(s_clips[idx].filename) - 1);
    s_clips[idx].filename[sizeof(s_clips[idx].filename) - 1] = '\0';
    clips_save();
    ESP_LOGI(TAG, "clip code %s now plays %s", code, s_clips[idx].filename);
    return ESP_OK;
}

esp_err_t dialplan_clip_remove(const char *code)
{
    int idx = clip_find_idx(code);
    if (idx < 0) return ESP_ERR_NOT_FOUND;
    /* Built-in codes can go too: they only come back after a factory reset. */
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

/* ---- pin dial codes ---- */

static int pin_find_idx(const char *code)
{
    for (int i = 0; i < DIALPLAN_MAX_PIN_ACTIONS; i++) {
        if (s_pin_actions[i].code[0] && strcmp(s_pin_actions[i].code, code) == 0) return i;
    }
    return -1;
}

esp_err_t dialplan_pin_check_new(const char *code)
{
    if (!code || !valid_code(code)) return ESP_ERR_INVALID_ARG;
    if (code_conflicts_ex(code, -1, -1)) return ESP_ERR_INVALID_STATE;
    for (int i = 0; i < DIALPLAN_MAX_PIN_ACTIONS; i++) {
        if (!s_pin_actions[i].code[0]) return ESP_OK;
    }
    return ESP_ERR_NO_MEM;
}

esp_err_t dialplan_pin_set(const char *code, int gpio, int secs, const char *desc)
{
    if (!code || !valid_code(code) || gpio < 0 || (gpio > 48 && gpio != DIALPLAN_PIN_NONE) ||
        secs < 0 || secs > 3600) {
        return ESP_ERR_INVALID_ARG;
    }
    int idx = pin_find_idx(code);
    if (idx < 0) {
        esp_err_t chk = dialplan_pin_check_new(code);
        if (chk != ESP_OK) return chk;
        for (int i = 0; i < DIALPLAN_MAX_PIN_ACTIONS; i++) {
            if (!s_pin_actions[i].code[0]) { idx = i; break; }
        }
    }
    memset(&s_pin_actions[idx], 0, sizeof(s_pin_actions[idx]));
    strncpy(s_pin_actions[idx].code, code, sizeof(s_pin_actions[idx].code) - 1);
    s_pin_actions[idx].gpio = (uint8_t)gpio;
    s_pin_actions[idx].secs = (uint16_t)secs;
    if (desc) strncpy(s_pin_actions[idx].desc, desc, sizeof(s_pin_actions[idx].desc) - 1);
    pins_save();
    ESP_LOGI(TAG, "pin code %s -> GPIO%d, %s", code, gpio,
             secs ? "timed" : "toggle");
    return ESP_OK;
}

esp_err_t dialplan_pin_remove(const char *code)
{
    int idx = code ? pin_find_idx(code) : -1;
    if (idx < 0) return ESP_ERR_NOT_FOUND;
    memset(&s_pin_actions[idx], 0, sizeof(s_pin_actions[idx]));
    pins_save();
    return ESP_OK;
}

size_t dialplan_list_pin_entries(dialplan_pin_entry_t *out, size_t max)
{
    size_t n = 0;
    for (int i = 0; i < DIALPLAN_MAX_PIN_ACTIONS && n < max; i++) {
        if (s_pin_actions[i].code[0]) out[n++] = s_pin_actions[i];
    }
    return n;
}


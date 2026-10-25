/*
 * dialplan.h — data-driven dial-code table + matcher.
 *
 * Ported from ../pi_rotary_phone/docs/DIAL_CODES.md. The matcher is
 * prefix-aware: a code fires immediately if no longer code shares its prefix,
 * otherwise it waits for the inter-digit timeout (dialplan_tick).
 *
 * The dialplan is pure logic; the telephony layer turns an action into audio,
 * ringing, calls, etc.
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ACT_NONE = 0,       /* keep collecting (valid prefix, no decision yet) */
    ACT_WRONG_NUMBER,   /* no match possible */
    ACT_OPERATOR_HELP,  /* dial 0 */
    ACT_PLAY_CLIP,      /* filename[] holds the clip to play (see dialplan_clip_entry_t) */
    ACT_RECORD_ARCHIVE, /* dial 211 */
    ACT_RECORD_GREETING, /* dial 999: record this line's answering-machine greeting */
    ACT_CALL_LINE,      /* arg = target line index; effect in arg2 */
    ACT_VOIP_DIAL,      /* number[] holds digits (e.g. 555xxxx) */
    ACT_BT_DIAL,        /* number[] holds 1 + 10 digits, placed via the paired phone */
    ACT_SET_MODE,       /* arg = dialplan_mode_change_t */
    ACT_GPIO_OUT,       /* arg = GPIO number, arg2 = seconds high (0 = toggle) */
    ACT_PI_DIGITS,      /* dial 314: read the first 100 decimals of pi aloud */
    ACT_HOME_CALL,      /* number[] is another phone's home-network number (homecall) */
    ACT_TIMER,          /* dial 846: ask how many minutes (1-99), then ring when it's up */
} dialplan_action_kind_t;

typedef enum {
    FX_NORMAL = 0,
    FX_HIGH,
    FX_LOW,
    FX_DALEK,
} voice_effect_t;

typedef enum {
    MODE_READBACK_OFF = 0, /* 921, default */
    MODE_READBACK_ON,      /* 922 */
} dialplan_mode_change_t;

typedef struct {
    dialplan_action_kind_t kind;
    int   arg;          /* effect / target line / station / mode */
    int   arg2;         /* secondary (e.g. call effect) */
    char  number[16];   /* for ACT_VOIP_DIAL / ACT_BT_DIAL */
    char  filename[32]; /* for ACT_PLAY_CLIP -- matches STORAGE_MAX_NAME
                          * (storage.h), duplicated rather than shared since
                          * dialplan doesn't otherwise depend on storage */
} dialplan_action_t;

/* Per-system mode flags (mirrors the Pi runtime flags). Language is NOT
 * here: the phone is set up with a single language at configuration time
 * (see board_language_get/set in board.h), not switched in use -- no 901/902
 * dial codes, no per-line flag. There's no Play/Call-only toggle either --
 * that turned out to be redundant (Play mode already allowed everything
 * Call-only did, just without the restriction) and was removed. */
typedef struct {
    bool readback_digits; /* PlayNumbers */
} dialplan_modes_t;

void dialplan_init(void);
dialplan_modes_t *dialplan_modes(void);
void dialplan_apply_mode(int line, dialplan_mode_change_t change);

/* Web-UI convenience: trigger a 9xx mode-setting dial code by its code
 * string (e.g. "931", "821") instead of dialing it, reusing the same TABLE
 * dialplan_feed() matches against so the web UI can't drift from what
 * dialing the code actually does. Returns false if `code` isn't a mode-
 * setting entry. `line` is unused by any current mode (none are per-line)
 * but kept for API stability if one ever is. */
bool dialplan_set_mode_by_code(const char *code, int line);

/* Per-line digit collector. */
typedef struct {
    char   buf[16];
    size_t len;
} dialplan_collector_t;

void dialplan_collector_reset(dialplan_collector_t *c);

/*
 * Feed one dialed digit ('0'-'9'). Returns an action:
 *   kind == ACT_NONE     -> keep collecting (start/continue inter-digit timer)
 * Any other kind is terminal for this dial attempt (reset collector after).
 */
dialplan_action_t dialplan_feed(dialplan_collector_t *c, int line, char digit);

/*
 * Call when the inter-digit timeout expires with a non-empty buffer. Resolves a
 * pending shorter match, or returns ACT_WRONG_NUMBER. ACT_NONE means "still a
 * valid prefix, keep waiting" (caller may extend or eventually give up).
 */
dialplan_action_t dialplan_timeout(dialplan_collector_t *c, int line);

/* Bluetooth dial-out: while on, "1" + 10 digits (US long-distance style,
 * e.g. 1-211-555-0123) resolves to ACT_BT_DIAL as soon as the 11th digit
 * arrives. The leading 1 is what keeps real numbers from colliding with the
 * short local codes (211 still records a message). While off, a leading 1 is
 * just an unassigned prefix (wrong number). The caller sets this before each
 * dialplan_feed() -- telephony turns it on only for the line the Bluetooth
 * module is wired to, and only while the module is enabled. "1" is reserved:
 * clip codes can never start with it. */
#define DIALPLAN_BT_PREFIX     '1'
#define DIALPLAN_BT_DIGITS     11
void dialplan_set_bt_dialout(bool on);

/* ---- Home-network numbers (homecall component) ----
 * Numbers other Analog Redial phones on the same network answer to, dialled
 * straight (no prefix). This board's own codes win over a clashing number.
 * Set as a whole list whenever discovery changes it. */
#define DIALPLAN_HOME_MAX      16
#define DIALPLAN_HOME_NUM_MAX  12   /* digits + NUL */
void dialplan_set_home_numbers(const char (*numbers)[DIALPLAN_HOME_NUM_MAX], size_t n);
/* This board's own home numbers, so new dial codes can't take them. */
void dialplan_set_own_home_numbers(const char *line0, const char *line1);
/* Why `number` can't be this board's home number, or NULL if it can: it
 * clashes with one of this board's dial codes (or shares a prefix with one,
 * which would make one of them undiallable), or with 555 / Bluetooth dialling. */
const char *dialplan_home_number_problem(const char *number);

/* Inter-digit timeout to use (ms). */
uint32_t dialplan_interdigit_ms(void);

/* ---- Introspection (web UI / bring-up) ----
 * Reflects the same table dialplan_feed()/dialplan_timeout() actually match
 * against -- there is no separate hand-maintained copy of this list, so it
 * can't drift out of sync with what the phone really does. */
/* What a built-in code is for, so the web UI's dial guide can group by
 * function instead of by number (the numbers can be changed). */
typedef enum {
    DIALPLAN_GROUP_CALLS = 0,   /* calls to the other line */
    DIALPLAN_GROUP_MESSAGES,    /* operator, leave a message, the greeting */
    DIALPLAN_GROUP_CLIPS,       /* plays audio: pi, and anything like it */
    DIALPLAN_GROUP_SETTINGS,    /* changes a setting */
} dialplan_group_t;

typedef struct {
    const char *code;          /* what to dial now (may have been changed) */
    const char *default_code;  /* the code it ships with: a stable name for it */
    const char *desc;
    bool        implemented; /* false = telephony.c currently just logs a
                               * "(TODO)" and plays a confirm tone for this */
    dialplan_group_t group;
} dialplan_entry_info_t;

/* Fills `out` (definition order), returns the number of entries written
 * (<= max). */
size_t dialplan_list(dialplan_entry_info_t *out, size_t max);

/* Every built-in code, including ones this board can't use right now (the
 * 3xx calls on a one-line board): `available` says which. */
typedef struct {
    dialplan_entry_info_t info;
    bool available;
} dialplan_fixed_info_t;
size_t dialplan_list_fixed(dialplan_fixed_info_t *out, size_t max);

/* Changes the code a built-in function answers to, named by its default code.
 * new_code "" (or the default itself) puts it back. ESP_ERR_NOT_FOUND (no
 * such built-in), ESP_ERR_INVALID_ARG (not 2-10 digits), ESP_ERR_INVALID_STATE
 * (conflicts with another code). Saved; takes effect at once. */
esp_err_t dialplan_fixed_set_code(const char *default_code, const char *new_code);

/* ---- Clip dial codes (Settings -> Dial Codes) ----
 * A single mutable code<->filename lookup table, covering both the "silly
 * audio" clips (burp, ring of fire, ...) and anything a user adds later --
 * there's no longer a fixed compile-time table for the former or a separate
 * additive-only table for the latter, since both are the same feature: a
 * dial code that plays a specific WAV. `builtin` rows are seeded once (see
 * dialplan.c) from the clips this project ships default codes/descriptions
 * for; their `code` and `filename` are both still freely editable at
 * runtime, they just can't be deleted outright (dialplan_clip_remove()
 * rejects that). Persisted to NVS. `filename` isn't checked against the
 * storage component here (dialplan doesn't depend on it) -- if the file
 * doesn't exist yet when the code is dialed, telephony.c falls back to a
 * confirm tone, same as any other missing clip. Operator help (dial 0) is
 * NOT part of this table -- it's a fixed TABLE[] entry with a hardcoded
 * "operator.wav" filename, replaced from its own Settings -> Operator tab. */
/* Longest dial code a user can add (a full phone number: 8675309, or ten
 * digits with an area code). code[12] holds it and the NUL. */
#define DIALPLAN_CODE_MAX_DIGITS 10

/* Removes the separators people type in a number (spaces, dashes, dots and
 * parentheses) in place, so "(867) 530-9" and "8675309" are the same code. */
void dialplan_code_normalize(char *code);

#define DIALPLAN_MAX_CLIP_ENTRIES 24 /* 2 builtins + headroom for user-added */

typedef struct {
    char code[12];     /* 2-10 digits */
    char filename[32]; /* matches STORAGE_MAX_NAME, see dialplan_action_t */
    char desc[64];
    bool builtin;      /* seeded default -- recodable/re-uploadable, not deletable */
} dialplan_clip_entry_t;

/*
 *   ESP_OK
 *   ESP_ERR_INVALID_ARG    -- code isn't 2-10 digits, or filename is empty
 *   ESP_ERR_INVALID_STATE  -- code conflicts (exact match or a prefix
 *                             relationship, either direction) with the fixed
 *                             table, another clip entry, or the 555 VoIP
 *                             prefix
 *   ESP_ERR_NO_MEM         -- DIALPLAN_MAX_CLIP_ENTRIES already used
 */
/* What dialplan_clip_add() would say about `code`, without adding it:
 * ESP_ERR_INVALID_ARG (not 2-10 digits), ESP_ERR_INVALID_STATE (conflicts with
 * an existing code), ESP_ERR_NO_MEM (table full), or ESP_OK. Lets an upload
 * be refused before its audio is written anywhere. */
esp_err_t dialplan_clip_check_new(const char *code);
esp_err_t dialplan_clip_add(const char *code, const char *filename, const char *desc);
/*
 *   ESP_ERR_NOT_FOUND      -- no entry has old_code
 *   ESP_ERR_INVALID_ARG    -- new_code isn't 2-10 digits
 *   ESP_ERR_INVALID_STATE  -- new_code conflicts with something else (not
 *                             counting the entry being renamed)
 */
esp_err_t dialplan_clip_set_code(const char *old_code, const char *new_code);
/* Relabels an existing code -- e.g. after repurposing a builtin slot for
 * different audio (see dialplan_defaults.h's dial-up-modem comment): the
 * label is a "seeded once" copy from CLIP_DEFAULTS like everything else in
 * this table, so an already-provisioned board doesn't pick up a source
 * change on its own; this is how to fix it up by hand instead of only on a
 * fresh board. Works on builtin entries too, same as set_code/
 * set_filename above -- only dialplan_clip_remove() singles builtins out.
 * ESP_ERR_NOT_FOUND if code is unknown. */
esp_err_t dialplan_clip_set_desc(const char *code, const char *desc);
/* Points a code at a different file in storage (ESP_ERR_NOT_FOUND: no such
 * code; ESP_ERR_INVALID_ARG: empty or too long). Doesn't check the file
 * exists: a missing clip plays the confirmation tone, as everywhere. */
esp_err_t dialplan_clip_set_filename(const char *code, const char *filename);
/* Looks up a single entry's current filename by code, without needing to
 * pull the whole table into a caller-provided array first (keeps callers
 * from needing a DIALPLAN_MAX_CLIP_ENTRIES-sized local just to find one
 * entry). ESP_ERR_NOT_FOUND if code is unknown. */
esp_err_t dialplan_clip_get_filename(const char *code, char *out, size_t out_len);
/* ESP_ERR_INVALID_STATE if the entry is builtin (not deletable, only
 * recodable/re-uploadable); ESP_ERR_NOT_FOUND if code is unknown. */
esp_err_t dialplan_clip_remove(const char *code);
/* Fills `out`, returns the number of entries written (<= max). */
size_t    dialplan_list_clip_entries(dialplan_clip_entry_t *out, size_t max);

/* ---- Pin dial codes (Settings -> Dial Codes -> Pin outputs) ----
 * A code that drives one of a one-line board's spare GPIOs high for a few
 * seconds, or toggles it. Only live while the board is set to one line and no
 * microSD (the pins are free then): otherwise the code is ignored, as if it
 * weren't there. The pin itself is driven by the auxpins component; this
 * table is just code -> (gpio, secs). Shares the code space with everything
 * else: the same prefix/conflict rules as clip codes apply in both
 * directions. Persisted to NVS. */
#define DIALPLAN_MAX_PIN_ACTIONS 8
/* gpio value of a Home Assistant code: no pin, the dial event is the action.
 * Live on every board, unlike pin codes. */
#define DIALPLAN_PIN_NONE 0xFF

typedef struct {
    char     code[12];  /* 2-10 digits */
    uint8_t  gpio;
    uint16_t secs;      /* 0 = toggle on each dial */
    char     desc[40];
} dialplan_pin_entry_t;

/* ESP_OK; ESP_ERR_INVALID_ARG (not 2-10 digits); ESP_ERR_INVALID_STATE
 * (conflicts with an existing code); ESP_ERR_NO_MEM (table full). */
esp_err_t dialplan_pin_check_new(const char *code);
/* Adds the code, or replaces its pin/secs/desc if it already is a pin code.
 * Same errors as the check above, plus ESP_ERR_INVALID_ARG for secs > 3600. */
esp_err_t dialplan_pin_set(const char *code, int gpio, int secs, const char *desc);
esp_err_t dialplan_pin_remove(const char *code);   /* ESP_ERR_NOT_FOUND if unknown */
size_t    dialplan_list_pin_entries(dialplan_pin_entry_t *out, size_t max);
/* True while pin codes that drive a pin are live (one line, no microSD). */
bool      dialplan_pins_active(void);

#ifdef __cplusplus
}
#endif

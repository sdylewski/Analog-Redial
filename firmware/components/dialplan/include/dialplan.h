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
    ACT_PLAY_CLIP,      /* arg = clip id (fixed compile-time table entry) */
    ACT_RECORD_PLAYBACK,/* arg = voice effect */
    ACT_RECORD_ARCHIVE, /* dial 111 */
    ACT_CALL_LINE,      /* arg = target line index; effect in arg2 */
    ACT_RADIO,          /* arg = station index */
    ACT_VOIP_DIAL,      /* number[] holds digits (e.g. 555xxxx) */
    ACT_SET_MODE,       /* arg = dialplan_mode_change_t */
    ACT_PLAY_USER_CLIP, /* filename[] holds the clip to play (user-added code) */
} dialplan_action_kind_t;

typedef enum {
    FX_NORMAL = 0,
    FX_HIGH,
    FX_LOW,
    FX_DALEK,
} voice_effect_t;

typedef enum {
    MODE_PLAY = 0,      /* 931 */
    MODE_CALL_ONLY,     /* 932 */
    MODE_READBACK_OFF,  /* 921 */
    MODE_READBACK_ON,   /* 922 */
} dialplan_mode_change_t;

typedef struct {
    dialplan_action_kind_t kind;
    int   arg;          /* clip id / effect / target line / station / mode */
    int   arg2;         /* secondary (e.g. call effect) */
    char  number[16];   /* for ACT_VOIP_DIAL */
    char  filename[32]; /* for ACT_PLAY_USER_CLIP -- matches STORAGE_MAX_NAME
                          * (storage.h), duplicated rather than shared since
                          * dialplan doesn't otherwise depend on storage */
} dialplan_action_t;

/* Per-system mode flags (mirrors the Pi runtime flags). Language is NOT
 * here: the phone is set up with a single language at configuration time
 * (see board_language_get/set in board.h), not switched in use -- no 901/902
 * dial codes, no per-line flag. */
typedef struct {
    bool call_only;       /* CallOnly */
    bool readback_digits; /* PlayNumbers */
} dialplan_modes_t;

void dialplan_init(void);
dialplan_modes_t *dialplan_modes(void);
void dialplan_apply_mode(int line, dialplan_mode_change_t change);

/* Web-UI convenience: trigger a 9xx mode-setting dial code by its code
 * string (e.g. "931", "921") instead of dialing it, reusing the same TABLE
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

/* Inter-digit timeout to use (ms). */
uint32_t dialplan_interdigit_ms(void);

/* ---- Introspection (web UI / bring-up) ----
 * Reflects the same table dialplan_feed()/dialplan_timeout() actually match
 * against -- there is no separate hand-maintained copy of this list, so it
 * can't drift out of sync with what the phone really does. */
typedef struct {
    const char *code;
    const char *desc;
    bool        implemented; /* false = telephony.c currently just logs a
                               * "(TODO)" and plays a confirm tone for this */
} dialplan_entry_info_t;

/* Fills `out` (definition order), returns the number of entries written
 * (<= max). */
size_t dialplan_list(dialplan_entry_info_t *out, size_t max);

/* ---- User-added dial codes (Settings -> General -> "Custom dial codes") ----
 * On top of the fixed compile-time TABLE above: assign a NEW number to an
 * already-uploaded/recorded/bundled WAV, no firmware rebuild. Persisted to
 * NVS. `filename` isn't checked against the storage component here (dialplan
 * doesn't depend on it) -- if the file doesn't exist yet when the code is
 * dialed, telephony.c falls back to a confirm tone, same as any other
 * missing clip. */
#define DIALPLAN_MAX_USER_ENTRIES 16

typedef struct {
    char code[8];      /* 2-6 digits */
    char filename[32]; /* matches STORAGE_MAX_NAME, see dialplan_action_t */
} dialplan_user_entry_t;

/*
 *   ESP_OK
 *   ESP_ERR_INVALID_ARG    -- code isn't 2-6 digits, or filename is empty
 *   ESP_ERR_INVALID_STATE  -- code conflicts (exact match or a prefix
 *                             relationship, either direction) with the fixed
 *                             table, another user entry, or the 555 VoIP
 *                             prefix
 *   ESP_ERR_NO_MEM         -- DIALPLAN_MAX_USER_ENTRIES already used
 */
esp_err_t dialplan_add_user_entry(const char *code, const char *filename);
esp_err_t dialplan_remove_user_entry(const char *code);
/* Fills `out`, returns the number of entries written (<= max). */
size_t    dialplan_list_user_entries(dialplan_user_entry_t *out, size_t max);

#ifdef __cplusplus
}
#endif

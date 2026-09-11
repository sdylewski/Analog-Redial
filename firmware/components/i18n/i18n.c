#include "i18n.h"
#include "board.h"
#include "storage.h"
#include "esp_log.h"
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static const char *TAG = "i18n";

/* Generous headroom for a full-UI-scale string table (see i18n.h). One-time
 * read per language change, not per request. */
#define I18N_READ_BUF_MAX 98304
#define I18N_MAX_ENTRIES  512

typedef struct { const char *key; const char *value; } i18n_entry_t;

/* Deliberately NOT cJSON here -- an earlier version parsed with cJSON and
 * kept the resulting tree cached for the process lifetime (needed: multiple
 * i18n_str() calls as sibling arguments to the same snprintf() must each
 * return a pointer that stays valid until that snprintf runs, ruling out a
 * "parse, look up, free" per call). cJSON allocates one heap chunk per
 * string/object node, so a ~200-entry table meant several hundred small,
 * long-lived allocations scattered across the heap -- enough heap
 * FRAGMENTATION that a later, unrelated large allocation (Settings ->
 * Storage's ~70KB listing buffer) started failing with plenty of raw free
 * bytes still reported, just no single contiguous run big enough. Real
 * failure, reproduced against the board, not theoretical.
 *
 * This flat parser instead makes exactly TWO heap allocations total: the
 * raw file bytes (unescaped in place, entries point directly into it) and
 * one array of (key, value) pointers. Same "valid until the language
 * changes" lifetime, far less fragmentation. Only handles what our own
 * generated files actually contain -- a flat, one-level JSON object with
 * plain string values and \" \\ \n \t \r \/ escapes -- not a general JSON
 * parser. */
static char         *s_raw = NULL;
static i18n_entry_t *s_entries = NULL;
static size_t         s_n_entries = 0;
static char           s_loaded_lang[8] = "";
static bool           s_load_attempted = false;

static void free_table(void)
{
    free(s_entries);
    s_entries = NULL;
    s_n_entries = 0;
    free(s_raw);
    s_raw = NULL;
}

void i18n_invalidate_cache(void)
{
    free_table();
    s_loaded_lang[0] = '\0';
    s_load_attempted = false;
}

/* Parses `buf` (a NUL-terminated, mutable copy of one of our own
 * upload_i18n.py-generated files) in place: unescapes each string's
 * contents and NUL-terminates it right there in the buffer, recording
 * (key, value) pointers into `out`. Malformed input just stops early and
 * returns however many whole entries it got before that -- best-effort,
 * every caller already falls back to English for any key it doesn't find. */
static size_t parse_flat_json_object(char *buf, i18n_entry_t *out, size_t max_entries)
{
    char *p = buf;
    while (*p && *p != '{') p++;
    if (*p != '{') return 0;
    p++;

    size_t n = 0;
    while (n < max_entries) {
        while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t' || *p == ',') p++;
        if (*p == '}' || *p == '\0') break;
        if (*p != '"') break;
        p++;

        char *key = p, *w = p;
        while (*p && *p != '"') {
            if (*p == '\\' && p[1]) {
                p++;
                switch (*p) {
                    case 'n': *w++ = '\n'; break;
                    case 't': *w++ = '\t'; break;
                    case 'r': *w++ = '\r'; break;
                    default:  *w++ = *p;   break; /* covers \" \\ \/ and any other */
                }
                p++;
            } else {
                *w++ = *p++;
            }
        }
        if (*p != '"') break;
        *w = '\0';
        p++;

        while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++;
        if (*p != ':') break;
        p++;
        while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++;
        if (*p != '"') break;
        p++;

        char *val = p;
        w = p;
        while (*p && *p != '"') {
            if (*p == '\\' && p[1]) {
                p++;
                switch (*p) {
                    case 'n': *w++ = '\n'; break;
                    case 't': *w++ = '\t'; break;
                    case 'r': *w++ = '\r'; break;
                    default:  *w++ = *p;   break;
                }
                p++;
            } else {
                *w++ = *p++;
            }
        }
        if (*p != '"') break;
        *w = '\0';
        p++;

        out[n].key = key;
        out[n].value = val;
        n++;
    }
    return n;
}

/* Loads (or confirms already-loaded) the string table for the board's
 * current language. English needs no table -- every call site's fallback
 * text already IS the English string, so the table just stays empty and
 * i18n_str() falls through to fallback, same as "no table uploaded yet". */
static void ensure_loaded(void)
{
    char lang[8];
    board_language_get(lang, sizeof(lang));

    if (s_load_attempted && strcmp(lang, s_loaded_lang) == 0) {
        return; /* already loaded (or already tried and failed) this exact language */
    }
    i18n_invalidate_cache();
    strncpy(s_loaded_lang, lang, sizeof(s_loaded_lang) - 1);
    s_load_attempted = true;

    if (strcmp(lang, "en") == 0) return;

    char name[32];
    snprintf(name, sizeof(name), "i18n_%s.json", lang);

    /* s_raw isn't freed after parsing -- every entry's key/value points
     * directly into it -- so it lives for as long as this language is
     * selected, not just for this function call. An earlier version
     * over-allocated a generous fixed-size scratch buffer here on the
     * (wrong, carried over from the old cJSON version, which DID free it
     * right after parsing) assumption that the padding was temporary; it
     * wasn't, and permanently wasted tens of KB of heap per language load.
     * Look the file's real size up first instead, via a listing scan (a
     * heap-allocated scratch array, not stack -- see webui.c's
     * append_file_select() doc comment on why a ~4KB storage_entry_t array
     * doesn't belong on this project's httpd worker stack), and allocate
     * exactly that. */
    #define I18N_LIST_MAX 100
    storage_entry_t *listing = malloc(I18N_LIST_MAX * sizeof(storage_entry_t));
    if (!listing) return;
    size_t nfiles = storage_list_internal(listing, I18N_LIST_MAX);
    size_t file_size = 0;
    bool found = false;
    for (size_t i = 0; i < nfiles; i++) {
        if (strcmp(listing[i].name, name) == 0) {
            file_size = listing[i].size;
            found = true;
            break;
        }
    }
    free(listing);
    if (!found || file_size == 0 || file_size >= I18N_READ_BUF_MAX) {
        if (!found) {
            ESP_LOGW(TAG, "no string table for language '%s' (%s not uploaded -- see "
                          "firmware/bringup/upload_i18n.py) -- using English fallback text",
                     lang, name);
        } else {
            ESP_LOGW(TAG, "'%s' is %u bytes, outside the plausible range -- using English "
                          "fallback text", name, (unsigned)file_size);
        }
        return;
    }

    s_raw = malloc(file_size + 1);
    if (!s_raw) return;
    size_t got = 0;
    esp_err_t err = storage_read(name, s_raw, file_size, &got);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "reading '%s' failed: %s -- using English fallback text",
                 name, esp_err_to_name(err));
        free(s_raw);
        s_raw = NULL;
        return;
    }
    s_raw[got] = '\0';

    s_entries = malloc(I18N_MAX_ENTRIES * sizeof(i18n_entry_t));
    if (!s_entries) {
        free(s_raw);
        s_raw = NULL;
        return;
    }
    s_n_entries = parse_flat_json_object(s_raw, s_entries, I18N_MAX_ENTRIES);
    if (s_n_entries == 0) {
        ESP_LOGW(TAG, "'%s' has no readable entries -- using English fallback text", name);
        free_table();
        return;
    }
    ESP_LOGI(TAG, "loaded UI string table for language '%s': %u entries (%u bytes)",
             lang, (unsigned)s_n_entries, (unsigned)got);
}

const char *i18n_str(const char *key, const char *fallback)
{
    ensure_loaded();
    for (size_t i = 0; i < s_n_entries; i++) {
        if (strcmp(s_entries[i].key, key) == 0) {
            return s_entries[i].value[0] ? s_entries[i].value : fallback;
        }
    }
    return fallback;
}

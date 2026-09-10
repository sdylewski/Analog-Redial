#include "i18n.h"
#include "board.h"
#include "storage.h"
#include "cJSON.h"
#include "esp_log.h"
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static const char *TAG = "i18n";

/* Generous headroom for a full-UI-scale string table (see i18n.h) -- this
 * is a one-time load per language change, not per request, and only the
 * raw bytes need this much room; they're freed right after cJSON_Parse()
 * copies what it needs into s_table below. */
#define I18N_READ_BUF_MAX 65536

static cJSON *s_table = NULL;
static char   s_loaded_lang[8] = "";   /* language s_table actually reflects */
static bool   s_load_attempted = false; /* so a missing file isn't re-fread every call */

void i18n_invalidate_cache(void)
{
    if (s_table) {
        cJSON_Delete(s_table);
        s_table = NULL;
    }
    s_loaded_lang[0] = '\0';
    s_load_attempted = false;
}

/* Loads (or confirms already-loaded) the string table for the board's
 * current language. English needs no table -- every call site's fallback
 * text already IS the English string, so s_table just stays NULL and
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

    if (strcmp(lang, "en") == 0) return; /* fallback text handles this language natively */

    char name[32];
    snprintf(name, sizeof(name), "i18n_%s.json", lang);

    char *raw = malloc(I18N_READ_BUF_MAX);
    if (!raw) return;
    size_t got = 0;
    esp_err_t err = storage_read(name, raw, I18N_READ_BUF_MAX - 1, &got);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no string table for language '%s' (%s not uploaded -- see "
                      "firmware/bringup/upload_i18n.py) -- using English fallback text",
                 lang, name);
        free(raw);
        return;
    }
    raw[got] = '\0';

    s_table = cJSON_Parse(raw);
    free(raw);
    if (!s_table) {
        ESP_LOGW(TAG, "'%s' isn't valid JSON -- using English fallback text", name);
        return;
    }
    ESP_LOGI(TAG, "loaded UI string table for language '%s' (%u bytes)", lang, (unsigned)got);
}

const char *i18n_str(const char *key, const char *fallback)
{
    ensure_loaded();
    if (!s_table) return fallback;

    cJSON *item = cJSON_GetObjectItemCaseSensitive(s_table, key);
    if (cJSON_IsString(item) && item->valuestring && item->valuestring[0]) {
        return item->valuestring;
    }
    return fallback;
}

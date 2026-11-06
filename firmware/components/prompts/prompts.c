#include "prompts.h"
#include "storage.h"
#include "cJSON.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "prompts";

#define FILE_MAIN ".prompts.json"
#define FILE_BAK  ".prompts.bak"

/* The old NVS table, read once to move it into the file. */
#define LEGACY_NVS_NS  "prompts"
#define LEGACY_NVS_KEY "entries"
#define LEGACY_ENTRIES 16
typedef struct {
    char name[PROMPT_NAME_MAX];
    char text[PROMPT_TEXT_MAX];
    char generated_text[PROMPT_TEXT_MAX];
} legacy_entry_t;

/* In RAM: only the prompts in use, each text sized to fit. */
typedef struct {
    char     name[PROMPT_NAME_MAX];
    char    *text;
    bool     generated;
    uint32_t generated_hash;
} rec_t;

static rec_t  *s_recs;
static size_t  s_n, s_cap;

uint32_t prompts_hash(const char *text)
{
    uint32_t h = 2166136261u;   /* FNV-1a */
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        h ^= *p;
        h *= 16777619u;
    }
    return h;
}

static int find(const char *name)
{
    for (size_t i = 0; i < s_n; i++)
        if (strcmp(s_recs[i].name, name) == 0) return (int)i;
    return -1;
}

/* Adds an entry (no save). NULL out of memory or past PROMPTS_MAX. */
static rec_t *add(const char *name, const char *text)
{
    if (s_n >= PROMPTS_MAX) return NULL;
    if (s_n == s_cap) {
        size_t ncap = s_cap ? s_cap * 2 : 16;
        rec_t *n = realloc(s_recs, ncap * sizeof(rec_t));
        if (!n) return NULL;
        s_recs = n;
        s_cap = ncap;
    }
    char *t = strndup(text, PROMPT_TEXT_MAX - 1);
    if (!t) return NULL;
    rec_t *r = &s_recs[s_n++];
    memset(r, 0, sizeof(*r));
    strncpy(r->name, name, PROMPT_NAME_MAX - 1);
    r->text = t;
    return r;
}

static char *to_json(void)
{
    cJSON *arr = cJSON_CreateArray();
    if (!arr) return NULL;
    for (size_t i = 0; i < s_n; i++) {
        cJSON *e = cJSON_CreateObject();
        if (!e) { cJSON_Delete(arr); return NULL; }
        cJSON_AddStringToObject(e, "name", s_recs[i].name);
        cJSON_AddStringToObject(e, "text", s_recs[i].text);
        cJSON_AddBoolToObject(e, "generated", s_recs[i].generated);
        cJSON_AddNumberToObject(e, "hash", (double)s_recs[i].generated_hash);
        cJSON_AddItemToArray(arr, e);
    }
    char *out = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return out;
}

static bool write_file(const char *name, const char *data, size_t len)
{
    storage_writer_t w;
    if (storage_write_begin_local(name, len, &w) != ESP_OK) return false;
    if (storage_write_chunk(&w, data, len) != ESP_OK) { storage_write_abort(&w); return false; }
    return storage_write_end(&w) == ESP_OK;
}

/* The backup first, so a power cut mid-save always leaves one whole copy. */
static bool save(void)
{
    char *json = to_json();
    if (!json) { ESP_LOGE(TAG, "save: out of memory"); return false; }
    size_t len = strlen(json);
    bool ok = write_file(FILE_BAK, json, len) && write_file(FILE_MAIN, json, len);
    free(json);
    if (!ok) ESP_LOGE(TAG, "couldn't save the prompts file");
    return ok;
}

static bool load_file(const char *name)
{
    storage_reader_t r;
    if (storage_read_open(name, &r) != ESP_OK) return false;
    size_t size = storage_read_size(&r);
    char *buf = (size > 0 && size < 64 * 1024) ? malloc(size + 1) : NULL;
    size_t got = 0;
    if (buf) {
        size_t n;
        while (got < size && (n = storage_read_chunk(&r, buf + got, size - got)) > 0) got += n;
        buf[got] = '\0';
    }
    storage_read_close(&r);
    if (!buf) return false;
    cJSON *arr = got == size ? cJSON_Parse(buf) : NULL;
    free(buf);
    if (!cJSON_IsArray(arr)) { cJSON_Delete(arr); return false; }
    cJSON *e;
    cJSON_ArrayForEach(e, arr) {
        const char *nm = cJSON_GetStringValue(cJSON_GetObjectItem(e, "name"));
        const char *tx = cJSON_GetStringValue(cJSON_GetObjectItem(e, "text"));
        if (!nm || !nm[0] || !tx || find(nm) >= 0) continue;
        rec_t *rec = add(nm, tx);
        if (!rec) break;
        rec->generated = cJSON_IsTrue(cJSON_GetObjectItem(e, "generated"));
        cJSON *h = cJSON_GetObjectItem(e, "hash");
        rec->generated_hash = cJSON_IsNumber(h) ? (uint32_t)h->valuedouble : 0;
    }
    cJSON_Delete(arr);
    return true;
}

/* The old NVS table: read it, move it to the file, and free the NVS space. */
static void migrate_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(LEGACY_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    size_t len = 0;
    if (nvs_get_blob(h, LEGACY_NVS_KEY, NULL, &len) != ESP_OK || len == 0) { nvs_close(h); return; }
    legacy_entry_t *old = malloc(len);
    bool read = old && len % sizeof(legacy_entry_t) == 0 &&
                len <= LEGACY_ENTRIES * sizeof(legacy_entry_t) &&
                nvs_get_blob(h, LEGACY_NVS_KEY, old, &len) == ESP_OK;
    size_t moved = 0;
    if (read) {
        for (size_t i = 0; i < len / sizeof(legacy_entry_t); i++) {
            old[i].name[PROMPT_NAME_MAX - 1] = '\0';
            old[i].text[PROMPT_TEXT_MAX - 1] = '\0';
            old[i].generated_text[PROMPT_TEXT_MAX - 1] = '\0';
            if (!old[i].name[0] || !old[i].text[0] || find(old[i].name) >= 0) continue;
            rec_t *r = add(old[i].name, old[i].text);
            if (!r) break;
            r->generated = old[i].generated_text[0] != '\0';
            r->generated_hash = r->generated ? prompts_hash(old[i].generated_text) : 0;
            moved++;
        }
    }
    free(old);
    /* Only drop the old table once the file is safely written. */
    if (read && save()) {
        nvs_erase_key(h, LEGACY_NVS_KEY);
        nvs_commit(h);
        ESP_LOGI(TAG, "moved %u prompts from NVS to %s", (unsigned)moved, FILE_MAIN);
    }
    nvs_close(h);
}

void prompts_init(void)
{
    for (size_t i = 0; i < s_n; i++) free(s_recs[i].text);
    s_n = 0;
    if (!load_file(FILE_MAIN) && load_file(FILE_BAK)) {
        ESP_LOGW(TAG, "%s unreadable, using %s", FILE_MAIN, FILE_BAK);
        save();
    }
    migrate_nvs();
    ESP_LOGI(TAG, "%u prompts", (unsigned)s_n);
}

esp_err_t prompts_set_text(const char *name, const char *text)
{
    if (!name || !name[0]) return ESP_ERR_INVALID_ARG;
    int i = find(name);

    if (!text || !text[0]) {
        /* Blank text: nothing left to generate, drop the entry entirely
         * rather than leave a pending-forever empty-text row in the UI. */
        if (i < 0) return ESP_OK;
        free(s_recs[i].text);
        memmove(&s_recs[i], &s_recs[i + 1], (s_n - (size_t)i - 1) * sizeof(rec_t));
        s_n--;
        save();
        return ESP_OK;
    }

    if (i < 0) {
        if (!add(name, text)) return ESP_ERR_NO_MEM;
    } else {
        char *t = strndup(text, PROMPT_TEXT_MAX - 1);
        if (!t) return ESP_ERR_NO_MEM;
        free(s_recs[i].text);
        s_recs[i].text = t;
    }
    save();
    ESP_LOGI(TAG, "'%s' text set (%u chars)", name, (unsigned)strlen(text));
    return ESP_OK;
}

void prompts_mark_generated(const char *name)
{
    int i = find(name);
    if (i < 0) return;
    s_recs[i].generated = true;
    s_recs[i].generated_hash = prompts_hash(s_recs[i].text);
    save();
    ESP_LOGI(TAG, "'%s' marked generated", name);
}

static void copy_out(const rec_t *r, prompt_entry_t *out)
{
    memset(out, 0, sizeof(*out));
    strncpy(out->name, r->name, PROMPT_NAME_MAX - 1);
    strncpy(out->text, r->text, PROMPT_TEXT_MAX - 1);
    out->generated = r->generated;
    out->generated_hash = r->generated_hash;
}

size_t prompts_count(void) { return s_n; }

size_t prompts_list(prompt_entry_t *out, size_t max_entries)
{
    size_t n = 0;
    for (size_t i = 0; i < s_n && n < max_entries; i++) copy_out(&s_recs[i], &out[n++]);
    return n;
}

bool prompts_get(const char *name, prompt_entry_t *out)
{
    int i = find(name);
    if (i < 0) return false;
    if (out) copy_out(&s_recs[i], out);
    return true;
}

bool prompts_text_pending(const prompt_entry_t *e)
{
    return !e->generated || prompts_hash(e->text) != e->generated_hash;
}

void prompts_reset(void)
{
    for (size_t i = 0; i < s_n; i++) free(s_recs[i].text);
    s_n = 0;
    storage_delete(FILE_MAIN);
    storage_delete(FILE_BAK);
    ESP_LOGI(TAG, "prompt texts cleared");
}

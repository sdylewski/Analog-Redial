#include "prompts.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "prompts";
#define PROMPTS_NVS_NS  "prompts"
#define PROMPTS_NVS_KEY "entries"

static prompt_entry_t s_prompts[PROMPT_MAX_ENTRIES];

static void save(void)
{
    nvs_handle_t h;
    if (nvs_open(PROMPTS_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_blob(h, PROMPTS_NVS_KEY, s_prompts, sizeof(s_prompts)) == ESP_OK) {
        nvs_commit(h);
    }
    nvs_close(h);
}

void prompts_init(void)
{
    memset(s_prompts, 0, sizeof(s_prompts));
    nvs_handle_t h;
    if (nvs_open(PROMPTS_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    size_t len = sizeof(s_prompts);
    nvs_get_blob(h, PROMPTS_NVS_KEY, s_prompts, &len); /* short/missing -> stays zeroed */
    nvs_close(h);
}

static int find_slot(const char *name)
{
    for (int i = 0; i < PROMPT_MAX_ENTRIES; i++) {
        if (strcmp(s_prompts[i].name, name) == 0) return i;
    }
    return -1;
}

esp_err_t prompts_set_text(const char *name, const char *text)
{
    if (!name || !name[0]) return ESP_ERR_INVALID_ARG;
    int i = find_slot(name);

    if (!text || !text[0]) {
        /* Blank text: nothing left to generate, drop the entry entirely
         * rather than leave a pending-forever empty-text row in the UI. */
        if (i < 0) return ESP_OK;
        memset(&s_prompts[i], 0, sizeof(s_prompts[i]));
        save();
        return ESP_OK;
    }

    if (i < 0) {
        i = find_slot(""); /* first empty slot */
        if (i < 0) return ESP_ERR_NO_MEM;
        strncpy(s_prompts[i].name, name, PROMPT_NAME_MAX - 1);
    }
    strncpy(s_prompts[i].text, text, PROMPT_TEXT_MAX - 1);
    s_prompts[i].text[PROMPT_TEXT_MAX - 1] = '\0';
    save();
    ESP_LOGI(TAG, "'%s' text set (%u chars)", name, (unsigned)strlen(text));
    return ESP_OK;
}

void prompts_mark_generated(const char *name)
{
    int i = find_slot(name);
    if (i < 0) return;
    strncpy(s_prompts[i].generated_text, s_prompts[i].text, PROMPT_TEXT_MAX - 1);
    s_prompts[i].generated_text[PROMPT_TEXT_MAX - 1] = '\0';
    save();
    ESP_LOGI(TAG, "'%s' marked generated", name);
}

size_t prompts_list(prompt_entry_t *out, size_t max_entries)
{
    size_t n = 0;
    for (int i = 0; i < PROMPT_MAX_ENTRIES && n < max_entries; i++) {
        if (!s_prompts[i].name[0]) continue;
        out[n++] = s_prompts[i];
    }
    return n;
}

bool prompts_get(const char *name, prompt_entry_t *out)
{
    int i = find_slot(name);
    if (i < 0) return false;
    if (out) *out = s_prompts[i];
    return true;
}

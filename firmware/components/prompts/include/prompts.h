/*
 * prompts.h — tracks "I want this WAV to say this text," so a host-side
 * script (gen_prompt.py) can synthesize it locally and upload the result,
 * without depending on any shared server. The ESP32 has no TTS of its own.
 *
 * Persistence mirrors dialplan.c's clip-code table exactly: a fixed-size
 * struct array as one NVS blob, empty slot = name[0] == '\0'. Small enough
 * (a handful of prompts, each a short sentence) that this beats a per-entry
 * key scheme or pulling in cJSON just for storage.
 *
 * A prompt is "pending" whenever text != generated_text -- no separate
 * status field. generated_text starts empty (never generated) and is set
 * to match text by prompts_mark_generated(), which the upload handlers
 * (webui.c) call once a WAV actually lands under that name -- fulfillment
 * falls out of the upload that already has to happen, no separate "mark
 * done" round trip from the host script.
 */
#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PROMPT_NAME_MAX 32   /* matches STORAGE_MAX_NAME (storage.h) -- not
                              * included here to keep this component free of
                              * a storage.h dependency; kept in sync by hand,
                              * both are WAV filenames in the same partition */
#define PROMPT_TEXT_MAX 200  /* a short spoken prompt, not an essay -- keeps
                              * the NVS blob small */
#define PROMPT_MAX_ENTRIES 16

typedef struct {
    char name[PROMPT_NAME_MAX];
    char text[PROMPT_TEXT_MAX];
    char generated_text[PROMPT_TEXT_MAX];
} prompt_entry_t;

void prompts_init(void);

/* Creates a new tracked entry for `name` (up to PROMPT_MAX_ENTRIES) or
 * updates its text if already tracked. Does NOT touch generated_text --
 * editing text alone is what makes an entry "pending" again. Blank text
 * removes the entry entirely (nothing left to generate). */
esp_err_t prompts_set_text(const char *name, const char *text);

/* Call once a WAV actually lands under `name` (the upload handlers, after a
 * successful write) -- no-op if `name` isn't a tracked entry. */
void prompts_mark_generated(const char *name);

size_t prompts_list(prompt_entry_t *out, size_t max_entries);

/* For a page that only needs its own entry (Operator/Record tabs) rather
 * than the whole list. False if `name` isn't tracked. */
bool prompts_get(const char *name, prompt_entry_t *out);

#ifdef __cplusplus
}
#endif

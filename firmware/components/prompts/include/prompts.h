/*
 * prompts.h — tracks "I want this WAV to say this text," so a host-side
 * script (gen_prompt.py) can synthesize it locally and upload the result,
 * without depending on any shared server. The ESP32 has no TTS of its own.
 *
 * Kept in a hidden file on internal flash (".prompts.json", with a
 * ".prompts.bak" copy written first), not NVS: the 24 KB NVS partition is
 * shared by every setting, while a prompt costs only its own text here, so
 * the number of prompts is limited by PROMPTS_MAX, not by a fixed table.
 * Files starting with "." don't show in the storage lists and are never
 * evicted. A board that had the old NVS table moves it to the file on its
 * first boot and frees the NVS space.
 *
 * A prompt is "pending" whenever its text isn't the text it was last
 * generated from (prompts_text_pending()), or (checked in webui.c, which has
 * storage) it was generated but its file is gone. The generated text is kept
 * as a hash, set by prompts_mark_generated(), which the upload handlers
 * (webui.c) call once a WAV actually lands under that name -- fulfillment
 * falls out of the upload that already has to happen, no separate "mark
 * done" round trip from the host script.
 *
 * Call prompts_init() after storage_init().
 */
#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PROMPT_NAME_MAX 32   /* matches STORAGE_MAX_NAME (storage.h) -- not
                              * included here to keep this header free of a
                              * storage.h dependency; kept in sync by hand,
                              * both are WAV filenames in the same partition */
#define PROMPT_TEXT_MAX 200  /* a short spoken prompt, not an essay */
#define PROMPTS_MAX     100  /* a sanity cap; each prompt costs only its text */

typedef struct {
    char     name[PROMPT_NAME_MAX];
    char     text[PROMPT_TEXT_MAX];
    bool     generated;      /* audio was made at least once */
    uint32_t generated_hash; /* prompts_hash() of the text it was made from */
} prompt_entry_t;

void prompts_init(void);

/* Creates a new tracked entry for `name` or updates its text if already
 * tracked. Doesn't touch the generated state -- editing text alone is what
 * makes an entry pending again. Blank text removes the entry entirely
 * (nothing left to generate). ESP_ERR_NO_MEM past PROMPTS_MAX or out of
 * memory. */
esp_err_t prompts_set_text(const char *name, const char *text);

/* Call once a WAV actually lands under `name` (the upload handlers, after a
 * successful write) -- no-op if `name` isn't a tracked entry. */
void prompts_mark_generated(const char *name);

/* How many prompts are tracked, to size a prompts_list() buffer. */
size_t prompts_count(void);
size_t prompts_list(prompt_entry_t *out, size_t max_entries);

/* For a page that only needs its own entry (Operator/Record tabs) rather
 * than the whole list. False if `name` isn't tracked. */
bool prompts_get(const char *name, prompt_entry_t *out);

/* True when the entry's text isn't what its audio was made from (or no audio
 * was ever made). Doesn't check that the file still exists. */
bool prompts_text_pending(const prompt_entry_t *e);

uint32_t prompts_hash(const char *text);

/* Forgets every prompt text (factory reset): deletes the file and its backup.
 * The generated audio files stay. */
void prompts_reset(void);

#ifdef __cplusplus
}
#endif

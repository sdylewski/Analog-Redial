/*
 * dialplan_defaults.h — builder-editable defaults for the mutable clip
 * dial-code table (Settings -> Dial Codes, see dialplan_clip_entry_t in
 * dialplan.h). Edit this file to change what ships as the out-of-box
 * "burp"/"ring of fire"/etc. dial-code set for a fresh build — none of
 * this is compiled-in behavior beyond the very first boot: dialplan.c
 * seeds these into NVS exactly once (see the "seeded" flag in
 * dialplan.c), and every code/filename is freely editable afterward from
 * the web UI without a rebuild.
 *
 * `filename` just needs to match whatever you actually plan to have in
 * storage (bundled via firmware/assets/, or upload it later through the
 * web UI) — dialplan doesn't validate it exists, telephony.c falls back
 * to a confirm tone the same way it does for any other missing clip.
 */
#pragma once

typedef struct {
    const char *code;      /* 2-6 digits */
    const char *filename;  /* matches STORAGE_MAX_NAME (storage.h) */
    const char *desc;      /* shown in Settings -> Dial Codes and the Dial Guide */
} clip_default_t;

static const clip_default_t CLIP_DEFAULTS[] = {
    { "401", "ring_of_fire.wav",    "Ring of Fire / That's Amore" },
    { "402", "burp.wav",            "Burp" },
    { "403", "crocodile_rock.wav",  "Crocodile Rock / Crocfa" },
    { "404", "martians.wav",        "Martians" },
    { "411", "banana_phone.wav",    "Banana Phone" },
    { "412", "banana_boat.wav",     "Banana Boat" },
    { "413", "banana_split.wav",    "Banana Split" },
    { "414", "lime_in_coconut.wav", "Lime in the Coconut" },
};
#define CLIP_DEFAULTS_N (sizeof(CLIP_DEFAULTS) / sizeof(CLIP_DEFAULTS[0]))

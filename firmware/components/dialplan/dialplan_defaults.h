/*
 * dialplan_defaults.h — builder-editable defaults for the mutable clip
 * dial-code table (Settings -> Dial Codes, see dialplan_clip_entry_t in
 * dialplan.h). Edit this file to change what ships as the out-of-box
 * "burp"/"dial-up modem"/etc. dial-code set for a fresh build — none of
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
    const char *code;      /* 2-10 digits */
    const char *filename;  /* matches STORAGE_MAX_NAME (storage.h) */
    const char *desc;      /* shown in Settings -> Dial Codes and the Dial Guide */
} clip_default_t;

static const clip_default_t CLIP_DEFAULTS[] = {
    { "412", "dialup.wav",          "Dial up modem" },
    /* Easter egg: an original spoken message (synthesized, not a recording of
     * any song), 38 KB as 8 kHz u-law. Appended after the others so boards
     * set up earlier pick it up at boot (see clips_add_new_defaults()). */
    { "8675309", "jenny.wav",       "Jenny" },
    /* 412 used to be "Banana Boat" -- repurposed (Wikimedia Commons,
     * "Dial_up_connection.ogg", public domain). Shipped as 8 kHz u-law WAV
     * (314 KB full length; 16-bit PCM would be 630 KB and overflow an 8 MB
     * board's 1 MB bundled partition). It was an MP3 before, which made
     * every clip's player task take minimp3's 28 KB stack; boards that still
     * have dialup.mp3 saved or bundled keep working through telephony.c's
     * .mp3/.wav fallback.
     * 411/413/414 (Banana Phone/Split, Lime in the Coconut) retired --
     * their pi_rotary_phone source audio was never part of this checkout
     * and isn't coming; see dialplan_clip_remove()'s "still_default" check
     * for how already-provisioned boards drop these from their persisted
     * table too, not just fresh ones.
     * 401/403/404 (Ring of Fire / That's Amore, Crocodile Rock / Crocfa,
     * Martians) retired for the same reason -- same still_default handling
     * drops them from already-provisioned boards' persisted tables.
     * 402 (Burp) retired from the defaults 2026-10-02: its audio is a
     * personal clip that isn't shipped, so a new board would have had a
     * dead code. Boards that already have 402 keep it (seeding runs once;
     * nothing removes retired codes) -- it just becomes deletable. That
     * leaves the dial-up modem sound (412) as the one out-of-box clip. */
};
#define CLIP_DEFAULTS_N (sizeof(CLIP_DEFAULTS) / sizeof(CLIP_DEFAULTS[0]))

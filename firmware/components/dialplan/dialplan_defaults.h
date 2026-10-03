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
    const char *code;      /* 2-6 digits */
    const char *filename;  /* matches STORAGE_MAX_NAME (storage.h) */
    const char *desc;      /* shown in Settings -> Dial Codes and the Dial Guide */
} clip_default_t;

static const clip_default_t CLIP_DEFAULTS[] = {
    { "412", "dialup.mp3",          "Dial up modem" },
    /* 412 used to be "Banana Boat" -- repurposed (Wikimedia Commons,
     * "Dial_up_connection.ogg", public domain) once its own source audio
     * turned out to be the first real test of CONFIG_PLAYER_MP3 playback
     * (see player/Kconfig): full-length at 40kbps mono MP3 is ~197KB vs.
     * ~630KB as 8kHz/16-bit PCM WAV, a real difference on a device with a
     * few MB of flash for everything combined. See gen_prompt.py's
     * convert_to_mp3()/--name *.mp3 for the host-side encode half.
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

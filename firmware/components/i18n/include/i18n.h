/*
 * i18n.h -- UI string translation.
 *
 * Design (see the SMD4 bring-up session that added this file for the
 * reasoning): exactly ONE language's string table lives on a board at a
 * time, as a small JSON file in the existing storage component (internal
 * flash/SPIFFS, the same place uploaded audio clips/recordings live) --
 * NOT compiled into firmware. The app partition is small and already
 * tight; bundling every supported language into every board's binary would
 * cost real, scarce flash for languages that board never uses. A host-side
 * script, firmware/bringup/upload_i18n.py, uploads a given language's file
 * (POST /media/i18n_<lang>.json, reusing the same generic upload endpoint
 * gen_prompt.py already uses for audio) -- translation content is authored
 * and edited on a dev machine (firmware/assets/i18n/, one JSON file per
 * language), not through the web UI.
 *
 * i18n_str() lazily loads and caches the current language's file (re-reads
 * it whenever board_language_get() changes) and looks `key` up in it,
 * falling back to `fallback` for English, an unloaded language (no file
 * uploaded yet), or a key missing from that language's file. Every call
 * site's `fallback` argument must be the real, correct English text --
 * it's what actually renders whenever a translation isn't available, not a
 * placeholder.
 *
 * Separately: firmware/bringup/gen_prompt.py (host-side TTS for spoken
 * prompts) does not read board_language_get() at all today -- picking a
 * matching voice/accent for a given language is entirely manual (pass the
 * right --piper-model/--engine yourself). Worth teaching it to fetch the
 * board's configured language (would need a small addition to GET
 * /api/status) and suggest a matching Piper voice, but that's a separate,
 * smaller change from the web UI string table above and doesn't need to
 * wait for it.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Returns the UI string for `key` in the board's current language
 * (board_language_get(), Settings -> General / the header language
 * picker), or `fallback` if that language is "en", has no uploaded string
 * table, or the table doesn't have this key. Callers must pass the real,
 * correct English text as `fallback` -- see this header's top comment. */
const char *i18n_str(const char *key, const char *fallback);

/* Drops the cached table so the next i18n_str() call re-reads from storage
 * -- call after a language change (board_language_set()) takes effect so a
 * newly-uploaded file for that language is picked up without a reboot.
 * i18n_str() also detects a language change on its own (compares against
 * board_language_get() each call), so calling this is an optimization
 * (forces an immediate re-read) rather than a correctness requirement. */
void i18n_invalidate_cache(void);

#ifdef __cplusplus
}
#endif

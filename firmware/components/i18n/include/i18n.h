/*
 * i18n.h -- seam for translating the web UI, not a built-out translation
 * system. Right now i18n_str() always returns `fallback` -- there is no
 * translation data on any board today. This header exists so a future pass
 * can wire individual webui.c strings to it one at a time
 * (i18n_str("status.uptime", "Uptime") instead of a bare "Uptime" literal)
 * with zero behavior change until real translation data actually exists,
 * rather than needing an all-or-nothing rewrite later.
 *
 * Recommended design for when that data actually gets built (not done yet --
 * write this up before implementing, since it's a real architecture choice,
 * not just "add more C"):
 *
 * - Do NOT compile multiple languages' string tables into firmware. The app
 *   partition is small and already tight (~200KB free as of the SMD4
 *   bring-up session that added this file) -- every bundled language is
 *   dead weight on every board that isn't using it, and that cost only
 *   grows as more languages get added.
 * - Instead: exactly ONE language's string table lives on a given board at
 *   a time, as a small file in the existing storage component (internal
 *   flash/SPIFFS, the same place uploaded audio clips and recordings
 *   live -- several MB free there, vs. the app partition's few hundred KB).
 *   Switching languages replaces that one file; nothing else on the board
 *   needs to change size.
 * - A host-side script (mirroring firmware/bringup/gen_prompt.py's existing
 *   device-sync pattern) would generate/upload that file for a given
 *   language code, so translation content lives and gets edited on a dev
 *   machine, not hand-typed through the web UI.
 * - i18n_str() would lazily load and cache that one active file into a
 *   small in-RAM lookup table the first time it's needed after boot (or
 *   after a language change), falling back to `fallback` for any key with
 *   no entry in the loaded table -- including "no file loaded at all,"
 *   which is exactly today's state and why every call site's fallback
 *   text must always be the real, correct English string, never a
 *   placeholder.
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
 * (board_language_get(), Settings -> General / the header language picker).
 * Always returns `fallback` today -- see this header's top comment. Callers
 * must pass the real, correct English text as `fallback`, not a placeholder
 * -- it's what actually renders until translation data exists. */
const char *i18n_str(const char *key, const char *fallback);

#ifdef __cplusplus
}
#endif

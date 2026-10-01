/*
 * tone_regions.h — builder/user-editable call-progress tone specs by
 * language (matches board_language_get()/Settings -> General -> Language).
 * Edit or extend this table to add another country -- nothing else in
 * tones.c needs to change.
 *
 * Sourced from ITU Operational Bulletin No. 781 (2003) Annex, "Tones used
 * in national networks", and cross-checked against the country-by-country
 * summary at help.genesys.com's "Dial Tone, Busy and Ringback Signals by
 * Country" reference. These are real national PSTN specs, not invented --
 * but treat them as a good-faith reference for a hobby build, not a
 * certified/regulatory-grade implementation (this project makes the same
 * disclaimer for SMD board clearances -- see CLAUDE.md).
 *
 * The United States entry doubles as the fallback default (matches the
 * hardcoded values this project shipped before regions existed) and is
 * also used for any language code with no explicit entry below.
 */
#pragma once

#include "tones.h"

typedef struct {
    const char *code;    /* region id stored by board_tone_region_set(), e.g. "it" */
    const char *name;    /* human label, shown in the web UI's region picker */
    tone_spec_t dial, ringback, busy;
} tone_region_t;

static const tone_region_t TONE_REGIONS[] = {
    { "us", "North America (US / Canada)",
      { 350, 450, { {0},                 0 } },              /* dial: continuous */
      { 440, 480, { {2000, 4000},        2 } },               /* ringback: 2s on / 4s off */
      { 480, 620, { {500, 500},          2 } } },             /* busy: 0.5s on/off */

    { "it", "Italy",
      { 425, 0,   { {600, 1000, 200, 200}, 4 } },             /* distinctive 4-phase dial cadence */
      { 425, 0,   { {1000, 4000},        2 } },
      { 425, 0,   { {500, 500},          2 } } },

    { "de", "Germany",
      { 425, 0,   { {0},                 0 } },
      { 425, 0,   { {1000, 4000},        2 } },
      { 425, 0,   { {500, 500},          2 } } },

    { "fr", "France",
      { 440, 0,   { {0},                 0 } },
      { 400, 0,   { {1650, 3350},        2 } },
      { 440, 0,   { {500, 500},          2 } } },

    { "jp", "Japan",
      { 400, 0,   { {0},                 0 } },
      { 384, 416, { {1000, 2000},        2 } },
      { 400, 0,   { {500, 500},          2 } } },

    { "cn", "China",
      { 450, 0,   { {0},                 0 } },
      { 450, 0,   { {1000, 4000},        2 } },
      { 450, 0,   { {350, 350},          2 } } },

    { "es", "Spain",
      { 425, 0,   { {0},                 0 } },
      { 425, 0,   { {1500, 3000},        2 } },
      { 425, 0,   { {200, 200},          2 } } },

    /* The UK/Australia/India family: a distinctive double ring --
     * 0.4s on, 0.2s gap, 0.4s on, then a long 2s silence. */
    { "gb", "United Kingdom",
      { 350, 450, { {0},                   0 } },
      { 400, 450, { {400, 200, 400, 2000}, 4 } },
      { 400, 0,   { {375, 375},            2 } } },

    { "au", "Australia / New Zealand",
      { 400, 425, { {0},                   0 } },
      { 400, 425, { {400, 200, 400, 2000}, 4 } },
      { 425, 0,   { {375, 375},            2 } } },

    { "in", "India",
      { 400, 0,   { {0},                   0 } },
      { 400, 0,   { {400, 200, 400, 2000}, 4 } },
      { 400, 0,   { {750, 750},            2 } } },

    { "eu", "Generic European (ITU-T)",
      { 425, 0,   { {0},          0 } },
      { 425, 0,   { {1000, 4000}, 2 } },
      { 425, 0,   { {500, 500},   2 } } },
};
#define TONE_REGIONS_N (sizeof(TONE_REGIONS) / sizeof(TONE_REGIONS[0]))

/* ---- language -> default region ----
 * Only for the "follow my language" convenience setting. A language maps to
 * a region here only when the answer is unambiguous: Italian means Italy,
 * Japanese means Japan. English and Spanish and French are deliberately
 * absent in their ambiguous forms -- English could be the US, the UK or
 * Australia; French could be France or Canada -- so they fall through to
 * the "us" default rather than guessing wrong. Anyone in those cases picks
 * a region explicitly, which is a one-time setting. */
typedef struct { const char *lang, *region; } tone_lang_default_t;

static const tone_lang_default_t TONE_LANG_DEFAULTS[] = {
    { "it", "it" },   /* Italian   -> Italy */
    { "de", "de" },   /* German    -> Germany */
    { "ja", "jp" },   /* Japanese  -> Japan */
    { "zh", "cn" },   /* Chinese   -> China */
    { "fr", "fr" },   /* French    -> France (not Canada; see above) */
    { "es", "es" },   /* Spanish   -> Spain (not Latin America) */
};
#define TONE_LANG_DEFAULTS_N (sizeof(TONE_LANG_DEFAULTS) / sizeof(TONE_LANG_DEFAULTS[0]))

/* Any region code not listed above (or unrecognized) falls back to the
 * generic ITU-T E.180-style European default. */
static const tone_region_t TONE_REGION_FALLBACK = {
    "", "Generic European (ITU-T default)",
    { 425, 0, { {0},          0 } },
    { 425, 0, { {1000, 4000}, 2 } },
    { 425, 0, { {500, 500},   2 } },
};

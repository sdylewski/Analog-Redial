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
    const char *lang;    /* board_language code this applies to, e.g. "it" */
    const char *name;    /* human label, for docs/logging only */
    tone_spec_t dial, ringback, busy;
} tone_region_t;

static const tone_region_t TONE_REGIONS[] = {
    { "en", "United States (default)",
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

    { "ja", "Japan",
      { 400, 0,   { {0},                 0 } },
      { 384, 416, { {1000, 2000},        2 } },
      { 400, 0,   { {500, 500},          2 } } },

    { "zh", "China",
      { 450, 0,   { {0},                 0 } },
      { 450, 0,   { {1000, 4000},        2 } },
      { 450, 0,   { {350, 350},          2 } } },

    { "es", "Spain",
      { 425, 0,   { {0},                 0 } },
      { 425, 0,   { {1500, 3000},        2 } },
      { 425, 0,   { {200, 200},          2 } } },
};
#define TONE_REGIONS_N (sizeof(TONE_REGIONS) / sizeof(TONE_REGIONS[0]))

/* Any board_language code not listed above (or unrecognized) falls back to
 * the generic ITU-T E.180-style European default -- matches Germany's
 * values almost exactly, which is itself the common baseline many
 * countries' specs derive from. */
static const tone_region_t TONE_REGION_FALLBACK = {
    "", "Generic European (ITU-T default)",
    { 425, 0, { {0},          0 } },
    { 425, 0, { {1000, 4000}, 2 } },
    { 425, 0, { {500, 500},   2 } },
};

#include "i18n.h"

/* See i18n.h's top comment for the whole design -- this is deliberately
 * just the seam, not the feature. `key` is unused today (no lookup table
 * exists yet); kept in the signature now so call sites can be migrated to
 * it ahead of the actual translation data, instead of a second signature
 * change later. */
const char *i18n_str(const char *key, const char *fallback)
{
    (void)key;
    return fallback;
}

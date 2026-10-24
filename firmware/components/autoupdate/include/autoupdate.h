/* autoupdate.h -- "keep this board up to date": once a night, at the hour the
 * user picks, the board reads the release manifest from GitHub, and if a newer
 * release exists for this board it downloads it into the spare firmware slot,
 * checks it (SHA-256 from the manifest, a valid image, built for this board)
 * and reboots into it. The same rollback as any update applies: if the new
 * image doesn't finish starting, the board returns to the old one.
 *
 * Off by default. Only releases are installed: a development build (a version
 * with a suffix such as -dirty or -2-gabc123) is never replaced. Nothing runs
 * while a phone is in use; it tries again a few minutes later within the hour.
 */
#pragma once

#include <stdbool.h>
#include <time.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AUTOUPDATE_MANIFEST_URL \
    "https://raw.githubusercontent.com/sdylewski/Analog-Redial/firmware/latest.json"

typedef struct {
    bool enabled;   /* default false */
    int  hour;      /* 0-23, local time, default 2 */
} autoupdate_cfg_t;

void             autoupdate_init(void);
autoupdate_cfg_t autoupdate_get(void);
esp_err_t        autoupdate_set(bool enabled, int hour);

/* What the last automatic check did, for the Hardware tab ("" if none yet). */
const char *autoupdate_status(void);
/* True while a check or install is running. */
bool        autoupdate_busy(void);

/* Runs a check (and install) as soon as possible, whatever the hour. */
void        autoupdate_check_now(void);

/* The spare firmware slot has one writer at a time: the automatic update or
 * a web upload. Claim before esp_ota_begin(), release when done or failed. */
bool        autoupdate_claim_slot(void);
void        autoupdate_release_slot(void);

#ifdef __cplusplus
}
#endif

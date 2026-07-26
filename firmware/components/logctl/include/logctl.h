/*
 * logctl.h — runtime-settable global log level, persisted to NVS.
 *
 * ESP-IDF's log level is compiled with a ceiling (CONFIG_LOG_MAXIMUM_LEVEL,
 * raised to DEBUG for this project so debug output isn't stripped) and an
 * initial runtime default. This wraps esp_log_level_set() so the level can
 * be changed at runtime from the web UI or the console and survives a
 * reboot -- e.g. crank it to DEBUG to watch rotary pulse / hook-switch
 * timing, then drop it back to INFO.
 */
#pragma once

#include "esp_log.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Reads the saved level from NVS (default INFO on first boot) and applies
 * it. Call once early in app_main, before subsystems start logging in
 * earnest. Ensures NVS is initialized (safe to call before other NVS
 * users). */
void logctl_init(void);

/* Persist + apply a new global level. Clamped to [NONE, VERBOSE]. Noisy
 * WiFi/PHY tags are re-pinned to WARN so they don't drown out app logs at
 * DEBUG/VERBOSE. */
esp_err_t logctl_set_level(esp_log_level_t level);

/* Current global level. */
esp_log_level_t logctl_get_level(void);

/* "none"/"error"/"warn"/"info"/"debug"/"verbose" <-> level. name never
 * returns NULL; from_name returns -1 (0xFF cast) if unrecognized -- callers
 * check for a valid range. */
const char     *logctl_level_name(esp_log_level_t level);
esp_log_level_t logctl_level_from_name(const char *name);

#ifdef __cplusplus
}
#endif

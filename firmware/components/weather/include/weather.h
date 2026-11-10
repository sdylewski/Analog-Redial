/*
 * weather.h -- the weather report: dial 1212 to hear the forecast.
 *
 * The forecast comes from Open-Meteo (free, no key) for one saved place, picked
 * on the Dial Codes tab by name. While the report is on, the board fetches it
 * every 30 minutes, so a call plays at once.
 *
 * The speech is a voice pack: every number and phrase the report uses, spoken
 * once (firmware/bringup/gen_weather_pack.py), in one WAV the board downloads
 * from the project's GitHub (WEATHER_PACK_BASE) and keeps in storage as
 * WEATHER_PACK_NAME. Its "wxix" chunk says where each clip starts; a report is
 * a list of those pieces, played as one clip (player_play_segments()).
 * About 1 MB: internal flash on a 16 MB board, a microSD card otherwise.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include "esp_err.h"
#include "player.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WEATHER_PACK_NAME "weather_en.pak"
#define WEATHER_PACK_BASE "https://raw.githubusercontent.com/sdylewski/Analog-Redial/main/firmware/assets/weather/"
#define WEATHER_PLACE_MAX 64

/* What the report includes (weather_cfg_t.parts). */
#define WEATHER_PART_NOW      0x01   /* "Right now, partly cloudy, and 58 degrees." */
#define WEATHER_PART_TODAY    0x02   /* today's high, low and chance of rain (evenings: tonight's low) */
#define WEATHER_PART_TOMORROW 0x04   /* tomorrow's */

typedef struct {
    bool    enabled;
    char    place[WEATHER_PLACE_MAX];   /* as shown, e.g. "Berkeley, California, United States" */
    double  lat, lon;                   /* both 0 = no place picked yet */
    bool    celsius;
    uint8_t parts;
} weather_cfg_t;

typedef enum {
    WEATHER_PACK_MISSING = 0,
    WEATHER_PACK_READY,
    WEATHER_PACK_INSTALLING,
} weather_pack_state_t;

typedef struct {
    weather_pack_state_t pack;
    uint32_t pack_bytes;        /* installed size, or the size being downloaded */
    uint32_t pack_done;         /* bytes downloaded so far while installing */
    bool     pack_on_sd;
    char     pack_msg[112];     /* the last install's result, "" if none */
    bool     have_forecast;
    time_t   fetched;           /* wall-clock time of the forecast held, 0 = none */
    char     fetch_msg[112];    /* the last fetch's error, "" after a good one */
} weather_status_t;

void weather_init(void);

weather_cfg_t weather_cfg(void);
/* Saves to NVS; a new place or units fetches the forecast again. */
esp_err_t weather_set_cfg(const weather_cfg_t *c);

weather_status_t weather_status(void);

/* NULL if the pack can be stored on this board now, else why not. */
const char *weather_pack_blocker(void);
/* Starts downloading the voice pack in the background (status shows progress).
 * `base` NULL = WEATHER_PACK_BASE; another base (console, testing) must end in "/". */
esp_err_t weather_install(const char *base);
esp_err_t weather_remove_pack(void);

/* Fetches the forecast now, in the background. */
void weather_refresh(void);

/* What the report would say right now, as text (web UI, console). */
size_t weather_report_text(char *buf, size_t cap);

/* Plays the report on `line` (one playback; done_cb when it ends). Without a
 * recent forecast it plays the pack's "isn't available" message and fetches
 * one. ESP_ERR_NOT_FOUND without a voice pack. */
esp_err_t weather_play(int line, player_done_cb_t done_cb, void *ctx);

#ifdef __cplusplus
}
#endif

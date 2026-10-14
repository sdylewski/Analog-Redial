/*
 * mqtt_ha.h — optional MQTT client with Home Assistant auto-discovery.
 *
 * Off by default and switchable at run time (Settings -> General): while off,
 * nothing is allocated. While on, the board
 *   - announces itself to Home Assistant (retained discovery messages under
 *     homeassistant/...) so it shows up as a device with no YAML,
 *   - publishes each line's state, the same events the webhook sends, Wi-Fi
 *     signal and free memory under redial/<id>/..., and
 *   - listens on redial/<id>/cmd/ring/<line>, cmd/pin/<code> and
 *     cmd/messages_seen (the Ring, Pin output and Mark messages heard buttons
 *     in Home Assistant).
 *
 * Plain mqtt:// only (TLS would cost ~35 KB of heap).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MQTT_HA_HOST_MAX 64
#define MQTT_HA_USER_MAX 32
#define MQTT_HA_PASS_MAX 64

typedef struct {
    bool enabled;
    char host[MQTT_HA_HOST_MAX];   /* "host" or "host:port" */
    char user[MQTT_HA_USER_MAX];
    char pass[MQTT_HA_PASS_MAX];
    bool ring_quiet;               /* ring commands obey quiet hours (default on) */
} mqtt_ha_cfg_t;

typedef enum { MQTT_HA_OFF = 0, MQTT_HA_CONNECTING, MQTT_HA_CONNECTED } mqtt_ha_state_t;

/* Loads the saved settings and starts if enabled. */
void mqtt_ha_init(void);

mqtt_ha_cfg_t mqtt_ha_get(void);
/* Saves and applies at once: stops a running client, starts a new one if
 * enabled. ESP_ERR_INVALID_ARG if enabled with an empty or bad host. */
esp_err_t mqtt_ha_set(const mqtt_ha_cfg_t *cfg);

mqtt_ha_state_t mqtt_ha_state(void);
/* Free heap bytes the running client held back (measured at connect), 0 if off. */
unsigned mqtt_ha_heap_cost(void);

#ifdef __cplusplus
}
#endif

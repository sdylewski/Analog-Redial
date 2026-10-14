idf_component_register(
    SRCS "mqtt_ha.c"
    INCLUDE_DIRS "include"
    REQUIRES mqtt nvs_flash board telephony ag1170 dialplan auxpins activity webhook esp_timer esp_wifi
)

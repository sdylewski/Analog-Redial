idf_component_register(
    SRCS "idle_power.c"
    INCLUDE_DIRS "include"
    REQUIRES board ag1170 audio telephony voip wifi nvs_flash esp_timer
)

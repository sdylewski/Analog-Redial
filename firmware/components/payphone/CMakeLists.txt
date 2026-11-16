idf_component_register(
    SRCS "payphone.c" "coin_tone.c" "payphone_types.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash esp_timer
)

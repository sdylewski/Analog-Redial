idf_component_register(
    SRCS "coinboard.c" "coin_inputs.c" "coin_fire.c"
    INCLUDE_DIRS "include"
    REQUIRES board payphone telephony driver esp_timer nvs_flash
)

idf_component_register(
    SRCS "payphone.c" "coin_tone.c" "green_box.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash esp_timer audio
)

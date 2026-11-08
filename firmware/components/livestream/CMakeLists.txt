idf_component_register(
    SRCS "livestream.c"
    INCLUDE_DIRS "include"
    REQUIRES audio callcap idle_power activity nvs_flash esp_timer lwip
)

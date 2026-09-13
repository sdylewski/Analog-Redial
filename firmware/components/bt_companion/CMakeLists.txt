idf_component_register(
    SRCS "bt_companion.c"
    INCLUDE_DIRS "include"
    REQUIRES board driver nvs_flash esp_timer
)

idf_component_register(
    SRCS "bt_companion.c" "bk3266.c" "bk3266_proto.c"
    INCLUDE_DIRS "include"
    REQUIRES board driver nvs_flash esp_timer
)

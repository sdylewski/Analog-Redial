idf_component_register(
    SRCS "bt_companion.c" "bt401.c" "bt401_proto.c"
    INCLUDE_DIRS "include"
    REQUIRES board driver nvs_flash esp_timer
)

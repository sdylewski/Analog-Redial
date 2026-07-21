idf_component_register(
    SRCS "board.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_timer nvs_flash esp_hw_support
)

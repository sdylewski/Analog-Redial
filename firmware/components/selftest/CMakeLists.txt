idf_component_register(
    SRCS "selftest.c"
    INCLUDE_DIRS "include"
    REQUIRES board ag1170 es8388 nvs_flash esp_timer esp_hw_support
)

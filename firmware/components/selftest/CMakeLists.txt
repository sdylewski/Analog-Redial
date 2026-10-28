idf_component_register(
    SRCS "selftest.c"
    INCLUDE_DIRS "include"
    REQUIRES board audio ag1170 es8388 aec telephony bt_companion nvs_flash esp_timer esp_hw_support
)

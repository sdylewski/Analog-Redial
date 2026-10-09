idf_component_register(
    SRCS "webauth.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash mbedtls esp_timer esp_hw_support
)

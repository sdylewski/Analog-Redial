idf_component_register(
    SRCS "autoupdate.c"
    INCLUDE_DIRS "include"
    REQUIRES board telephony activity wifi nvs_flash esp_timer esp_http_client esp-tls mbedtls json app_update esp_partition
)

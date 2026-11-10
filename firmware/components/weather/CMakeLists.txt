idf_component_register(
    SRCS "weather.c"
    INCLUDE_DIRS "include"
    REQUIRES player storage dialplan wifi nvs_flash esp_timer esp_http_client esp-tls mbedtls json
)

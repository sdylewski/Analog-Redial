idf_component_register(
    SRCS "webhook.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_client nvs_flash board
)

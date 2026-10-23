idf_component_register(
    SRCS "voip_prov.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_client mbedtls json nvs_flash mdns esp_hw_support board voip storage
)

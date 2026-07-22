idf_component_register(
    SRCS "voip.c" "rtp.c"
    INCLUDE_DIRS "include"
    REQUIRES audio wifi mbedtls nvs_flash mdns lwip esp_netif esp_timer esp_hw_support
)

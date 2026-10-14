idf_component_register(
    SRCS "voip.c" "rtp.c"
    INCLUDE_DIRS "include"
    REQUIRES audio audio_convert aec wifi board mbedtls nvs_flash mdns lwip esp_netif esp_timer esp_hw_support
)

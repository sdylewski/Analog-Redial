idf_component_register(
    SRCS "homecall.c"
    INCLUDE_DIRS "include"
    REQUIRES mdns nvs_flash esp_timer esp_netif esp_system wifi voip dialplan board
)

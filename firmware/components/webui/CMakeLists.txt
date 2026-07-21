idf_component_register(
    SRCS "webui.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server json wifi storage logbuf telephony dialplan board spi_flash
    EMBED_TXTFILES "style.css"
)

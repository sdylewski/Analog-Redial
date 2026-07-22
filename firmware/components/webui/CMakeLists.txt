idf_component_register(
    SRCS "webui.c" "multipart.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server json wifi storage logbuf telephony dialplan board spi_flash voip audio_convert
    EMBED_TXTFILES "style.css"
)

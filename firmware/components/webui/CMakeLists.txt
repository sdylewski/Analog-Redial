idf_component_register(
    SRCS "webui.c" "multipart.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server esp_http_client json wifi storage logbuf telephony dialplan board spi_flash voip audio_convert logctl audio aec es8388 sdcard prompts
    EMBED_TXTFILES "style.css"
)

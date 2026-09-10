idf_component_register(
    SRCS "webui.c" "multipart.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server json wifi storage logbuf activity telephony dialplan board spi_flash voip audio_convert logctl audio aec es8388 sdcard prompts idle_power nvs_flash esp_partition
    EMBED_TXTFILES "style.css" "favicon.svg"
)

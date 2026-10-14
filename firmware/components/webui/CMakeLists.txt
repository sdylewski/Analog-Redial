idf_component_register(
    SRCS "webui.c" "multipart.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server json wifi storage ag1170 tones logbuf activity telephony dialplan board spi_flash voip voip_prov audio_convert logctl audio aec es8388 sdcard prompts idle_power nvs_flash esp_partition i18n app_update recorder bootloader_support bt_companion selftest webauth mbedtls auxpins webhook
    EMBED_TXTFILES "style.css" "favicon.svg"
    EMBED_FILES "favicon.ico"
)

idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES board wm8731 es8388 audio aec prompts ag1170 dialplan telephony bringup wifi storage sdcard logbuf logctl activity webui idle_power app_update
)

# Bakes firmware/assets/ into the "bundled" partition (partitions.csv) as a
# SPIFFS image, flashed alongside the app by a normal `idf.py flash`
# (FLASH_IN_PROJECT) -- separate from the "storage" partition, so this never
# touches recordings. See storage.c's read fallback and firmware/assets/README.md.
spiffs_create_partition_image(bundled "${CMAKE_CURRENT_LIST_DIR}/../assets" FLASH_IN_PROJECT)

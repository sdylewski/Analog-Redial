idf_component_register(
    SRCS "auxpins.c"
    INCLUDE_DIRS "include"
    REQUIRES board ag1170 sdcard bt_companion driver esp_timer
)

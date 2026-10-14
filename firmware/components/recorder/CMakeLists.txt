idf_component_register(
    SRCS "recorder.c"
    INCLUDE_DIRS "include"
    REQUIRES audio storage activity esp_timer webhook
)

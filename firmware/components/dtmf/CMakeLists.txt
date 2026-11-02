idf_component_register(
    SRCS "dtmf.c" "dtmf_test.c"
    INCLUDE_DIRS "include"
    REQUIRES tones esp_timer
)

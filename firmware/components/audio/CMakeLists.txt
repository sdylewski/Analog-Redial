idf_component_register(
    SRCS "audio.c"
    INCLUDE_DIRS "include"
    REQUIRES board tones dtmf aec driver
)

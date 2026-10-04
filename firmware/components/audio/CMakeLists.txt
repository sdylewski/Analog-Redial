idf_component_register(
    SRCS "audio.c" "voice_fx.c" "tone_purity.c"
    INCLUDE_DIRS "include"
    REQUIRES board tones dtmf aec driver
)

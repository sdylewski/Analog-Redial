idf_component_register(
    SRCS "audio.c" "voice_fx.c" "tone_purity.c" "fx_test.c"
    INCLUDE_DIRS "include"
    REQUIRES board tones dtmf aec driver esp_timer
)

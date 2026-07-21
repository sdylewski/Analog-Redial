idf_component_register(
    SRCS "telephony.c"
    INCLUDE_DIRS "include"
    REQUIRES dialplan ag1170 audio dtmf tones voip player storage esp_timer
)

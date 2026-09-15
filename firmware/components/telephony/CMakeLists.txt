idf_component_register(
    SRCS "telephony.c"
    INCLUDE_DIRS "include"
    REQUIRES dialplan ag1170 audio dtmf tones voip player recorder storage board activity esp_timer bt_companion
)

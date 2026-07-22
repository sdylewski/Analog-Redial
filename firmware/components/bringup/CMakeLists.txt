idf_component_register(
    SRCS "bringup.c"
    INCLUDE_DIRS "include"
    REQUIRES board wm8731 es8388 audio ag1170 dtmf telephony wifi storage sdcard player voip console driver
)

idf_component_register(
    SRCS "bringup.c"
    INCLUDE_DIRS "include"
    REQUIRES board wm8731 audio ag1170 dtmf telephony console driver
)

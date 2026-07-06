idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES board wm8731 audio ag1170 dialplan telephony bringup
)

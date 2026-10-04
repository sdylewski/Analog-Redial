idf_component_register(
    SRCS "bringup.c"
    INCLUDE_DIRS "include"
    REQUIRES board es8388 audio aec ag1170 dtmf telephony wifi storage sdcard player voip bt_companion selftest idle_power logctl console driver
)

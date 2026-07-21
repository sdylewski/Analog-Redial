if(CONFIG_BOARD_TYPE_SMD2)
    set(srcs "es8388.c")
else()
    set(srcs "")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include"
    REQUIRES board driver
)

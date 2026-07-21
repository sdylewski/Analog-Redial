if(CONFIG_BOARD_TYPE_SMD2)
    set(srcs "")
else()
    set(srcs "wm8731.c")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include"
    REQUIRES board driver
)

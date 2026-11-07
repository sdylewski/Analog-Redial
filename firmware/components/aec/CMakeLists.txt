idf_component_register(
    SRCS "aec.c"
    INCLUDE_DIRS "include"
    REQUIRES board esp_pm
)
# The filter's inner loops run 5M times a second on the audio task: built
# optimized even when the rest of the firmware is a debug (-Og) build.
target_compile_options(${COMPONENT_LIB} PRIVATE -O2)

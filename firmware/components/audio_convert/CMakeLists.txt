set(srcs "audio_convert.c")

# MP3 support is a separate translation unit, added to the build only when
# the Kconfig option is on -- lets a size-delta measurement (this option's
# whole purpose right now) be exact: with it off, minimp3 is never even
# compiled, let alone linked in.
if(CONFIG_AUDIO_CONVERT_MP3)
    list(APPEND srcs "audio_convert_mp3.c")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include"
)

if(CONFIG_AUDIO_CONVERT_MP3)
    # minimp3 is third-party (CC0, not written to this project's warning
    # strictness) -- relax warnings for just this one file rather than
    # patching the vendored header.
    set_source_files_properties(audio_convert_mp3.c PROPERTIES COMPILE_FLAGS
        "-Wno-unused-parameter -Wno-sign-compare -Wno-maybe-uninitialized -Wno-unused-function")
endif()

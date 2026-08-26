set(srcs "audio_convert.c")

# MP3 support is a separate translation unit, added to the build only when
# either Kconfig option that needs it is on -- CONFIG_AUDIO_CONVERT_MP3
# (browser-upload decode, webui.c) or CONFIG_PLAYER_MP3 (playback decode,
# player.c). Both share the same minimp3-backed decoder API in this file;
# neither pays for it (measurable via .bin size) when both are off.
if(CONFIG_AUDIO_CONVERT_MP3 OR CONFIG_PLAYER_MP3)
    list(APPEND srcs "audio_convert_mp3.c")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include"
)

if(CONFIG_AUDIO_CONVERT_MP3 OR CONFIG_PLAYER_MP3)
    # minimp3 is third-party (CC0, not written to this project's warning
    # strictness) -- relax warnings for just this one file rather than
    # patching the vendored header.
    set_source_files_properties(audio_convert_mp3.c PROPERTIES COMPILE_FLAGS
        "-Wno-unused-parameter -Wno-sign-compare -Wno-maybe-uninitialized -Wno-unused-function")
endif()

idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES board es8388 audio aec prompts ag1170 dialplan telephony bringup wifi storage sdcard logbuf logctl activity webui idle_power app_update bt_companion webauth webhook mqtt_ha homecall autoupdate
)

# Bakes the default audio into the "bundled" partition (partitions.csv) as a
# SPIFFS image, flashed alongside the app by a normal `idf.py flash`
# (FLASH_IN_PROJECT) -- separate from the "storage" partition, so this never
# touches recordings. See storage.c's read fallback and firmware/assets/README.md.
#
# Only the files listed here go in. firmware/assets/ also holds personal
# clips (burp.wav, ...) and draft translations that stay on the developer's
# disk; baking the whole directory put those on every board built from it.
set(BUNDLED_AUDIO
    operator.wav        # dial 0
    operator_2line.wav  # dial 0 on a two-line board
    record_prompt.wav   # dial 211, before the beep
    timer_prompt.wav    # dial 846, "How many minutes for the timer?"
    timer_done.wav      # dial 846: "Your timer is done." when it goes off
    alarm_prompt.wav    # dial 256, "What time should the alarm ring?..."
    alarm_done.wav      # dial 256: "This is your alarm." when it rings
    am_greeting.wav     # answering machine's default greeting
    dialup.wav          # 412, public domain (8-bit u-law: full length in 314 KB)
    jenny.wav           # 8675309, spoken message (8-bit u-law, 38 KB)
    0.wav 1.wav 2.wav 3.wav 4.wav 5.wav 6.wav 7.wav 8.wav 9.wav point.wav
                        # spoken digits: digit readback, and dial 314 (pi); ~38 KB in all
)
set(BUNDLED_STAGE "${CMAKE_BINARY_DIR}/bundled_assets")
file(REMOVE_RECURSE "${BUNDLED_STAGE}")
foreach(f ${BUNDLED_AUDIO})
    set(src "${CMAKE_CURRENT_LIST_DIR}/../assets/${f}")
    if(EXISTS "${src}")
        # configure_file (not file(COPY)) so editing the source re-runs CMake
        # and the image picks the change up.
        configure_file("${src}" "${BUNDLED_STAGE}/${f}" COPYONLY)
    else()
        message(WARNING "bundled audio: ${f} is missing from firmware/assets/ -- "
                        "the board will build without it")
    endif()
endforeach()
spiffs_create_partition_image(bundled "${BUNDLED_STAGE}" FLASH_IN_PROJECT)

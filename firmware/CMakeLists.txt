# ESP32-C6 Rotary Phone Gateway firmware
# Pinned to ESP-IDF v5.2.8 (target esp32c6) -- see IDF_PINNED_VERSION below.
cmake_minimum_required(VERSION 3.16)

# Regenerate version.txt fresh every configure with a clean git describe,
# restricted to real "vX.Y.Z" tags via --match -- this repo has none (see
# `git tag -l`), so it always falls through to --always's plain short-hash
# behavior instead. Deliberately NOT project.cmake's own default a few
# lines down ("git describe --always --tags --dirty", no --match): this
# repo has real annotated tags left over from an "archive/<branch> tag
# before deleting the branch" workflow (archive/freepbx-voip-provisioner,
# archive/record-and-callback) that are reachable from HEAD and much
# closer than any actual release tag would ever be, so the stock command
# picks one of THOSE as "the version" -- confirmed showing up, truncated
# to esp_app_desc_t's 32-byte field, as the web UI's Firmware card Version
# value. version.txt (checked by project.cmake below, before it falls
# back to running git describe itself) is the intended override point for
# exactly this -- see its own doc comment there. Gitignored: this file is
# build output, not something to commit.
find_package(Git QUIET)
if(GIT_EXECUTABLE)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} describe --always --dirty --match "v*"
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_VERSION_STRING
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(GIT_VERSION_STRING)
        file(WRITE "${CMAKE_CURRENT_SOURCE_DIR}/version.txt" "${GIT_VERSION_STRING}")
    endif()
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Every dev machine must build with the same ESP-IDF: a different version
# regenerates the tracked sdkconfig (header, CONFIG_SOC_* lines, ...) and it
# shows up as a modified file with no real setting changed -- this laptop
# (5.2.0) vs. the work machine (5.2.8) did exactly that until 2026-09-28.
# A warning, not an error, so a deliberate upgrade can still be tried.
set(IDF_PINNED_VERSION "5.2.8")
set(IDF_ACTUAL_VERSION "${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}.${IDF_VERSION_PATCH}")
if(NOT IDF_ACTUAL_VERSION VERSION_EQUAL IDF_PINNED_VERSION)
    message(WARNING "ESP-IDF ${IDF_ACTUAL_VERSION} in use, but this project is pinned to "
        "v${IDF_PINNED_VERSION}. Building will rewrite firmware/sdkconfig -- don't commit "
        "that. See firmware/README.md to switch versions.")
endif()

project(rotary_phone_gateway)

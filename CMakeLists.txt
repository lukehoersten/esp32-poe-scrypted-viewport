cmake_minimum_required(VERSION 3.16)

# Stamp the build with the current git short hash + dirty marker so
# the firmware can log it at boot and we can verify which commit is
# running on the device when reading log output. Falls back to
# "unknown" outside a git checkout.
execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
execute_process(
    COMMAND git diff --quiet HEAD --
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    RESULT_VARIABLE GIT_DIRTY_RC
    ERROR_QUIET)
if(NOT GIT_HASH)
    set(GIT_HASH "unknown")
endif()
if(NOT GIT_DIRTY_RC EQUAL 0)
    set(GIT_HASH "${GIT_HASH}-dirty")
endif()
set(PROJECT_VER "${GIT_HASH}")
set(PROJECT_VER_NUMBER 1)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(scrypted-viewport)

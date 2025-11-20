cmake_minimum_required(VERSION 2.8.12)

# Configurable options
option(StreamCore32_TARGET_CLI "Enable SteramCore32 cli target" OFF)
option(StreamCore32_TARGET_ESP32 "Enable StreamCore32-esp32 target" OFF)
option(BELL_SINK_VS1053 "Enable VS1053 sink" ON)

# Enable spotifycli target
if(StreamCore32_TARGET_CLI)
    add_subdirectory(targets/cli)
    project(StreamCore32cli)
endif()

# Enable spotify-esp32 target
if(StreamCore32_TARGET_ESP32)
    include($ENV{IDF_PATH}/tools/cmake/project.cmake)
    set(IDF_NO_INCLUDE ON)
    add_subdirectory(targets/esp32)
    project(StreamCore32-esp32)
endif()


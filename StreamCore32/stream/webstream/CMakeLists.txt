cmake_minimum_required(VERSION 3.15)
project(stream_webstream C CXX)

file(GLOB SOUNDCLOUD_SRC "src/*.cpp" "src/*.c")
list(APPEND EXTRA_LIBS ${BELL_TARGET})

add_library(stream_webstream STATIC
  ${SOUNDCLOUD_SRC} )

target_include_directories(stream_webstream
  PUBLIC "include" ${CMAKE_CURRENT_BINARY_DIR})

target_link_libraries(stream_webstream PUBLIC StreamCore32::core )
target_compile_definitions(stream_webstream PUBLIC PB_ENABLE_MALLOC PB_FIELD_32BIT)

add_library(StreamCore32::stream_webstream ALIAS stream_webstream)

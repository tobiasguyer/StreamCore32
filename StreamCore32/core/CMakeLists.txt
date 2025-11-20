cmake_minimum_required(VERSION 3.15)
project(streamcore_core CXX)

file(GLOB SOURCES "src/*.cpp" "src/*.c")
file(GLOB HEADERS "include/*.h" "*.h")
list(APPEND EXTRA_LIBS ${BELL_TARGET})

add_library(streamcore_core STATIC ${SOURCES})
target_link_libraries(streamcore_core PUBLIC ${EXTRA_LIBS} )
target_compile_definitions(streamcore_core PUBLIC PB_ENABLE_MALLOC PB_FIELD_32BIT)
target_include_directories(streamcore_core PUBLIC "include" ${CMAKE_CURRENT_BINARY_DIR} ${EXTRA_INCLUDES})

add_library(StreamCore32::core ALIAS streamcore_core)

cmake_minimum_required(VERSION 3.5)

# Main target sources
file(GLOB SOURCES "*.cpp" "*.c")

# Configure the target
idf_component_register(
    SRCS ${SOURCES}
REQUIRES mdns esp_wifi nvs_flash efuse
    INCLUDE_DIRS "."
)
#idf.py add-dependency "espressif/esp_websocket_client^1.0.0" 
idf_build_set_property(COMPILE_OPTIONS "-fdiagnostics-color=always" APPEND)
# Build static library, do not build test executables
option(BUILD_SHARED_LIBS OFF)
option(BUILD_TESTING OFF)
get_filename_component(STREAMCORE32_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../StreamCore32" ABSOLUTE)
add_subdirectory("${STREAMCORE32_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/StreamCore32")

# Link to the spotify target
target_link_libraries(
  ${COMPONENT_LIB}
  PRIVATE
  StreamCore32::core
  StreamCore32::stream_spotify
  StreamCore32::stream_qobuz
  StreamCore32::stream_webstream
  )
# Make sure 'main' can see Nanopb generated headers from spotify
# Keep this so "protobuf/*.pb.h" resolves:
# Make sure 'main' sees headers & generated code
target_include_directories(${COMPONENT_LIB} PRIVATE
  # generated .pb.h from spotify:
  "${CMAKE_CURRENT_BINARY_DIR}/StreamCore32/stream/spotify"
  "${CMAKE_CURRENT_BINARY_DIR}/StreamCore32/stream/qobuz"

  # source headers:
  "${STREAMCORE32_ROOT}/core/include"
  "${STREAMCORE32_ROOT}/stream/spotify/include"
  "${STREAMCORE32_ROOT}/stream/qobuz/include"
  "${STREAMCORE32_ROOT}/stream/webstream/include"
)

get_filename_component(STREAMCORE32_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../StreamCore32" ABSOLUTE)

target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++17)

cmake_minimum_required(VERSION 3.15)
project(stream_spotify C CXX)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

# Nanopb
set(NANOPB_ROOT      "${CMAKE_CURRENT_LIST_DIR}/../../bell/external/nanopb")
set(NANOPB_GEN_PY    "${NANOPB_ROOT}/generator/nanopb_generator.py")
set(NANOPB_PROTO_DIR "${NANOPB_ROOT}/generator/proto")

# protoc
find_program(PROTOC NAMES protoc protoc.exe HINTS $ENV{PROTOC} "C:/proto3/bin")
if(NOT PROTOC)
  message(FATAL_ERROR "protoc not found. Install or set PROTOC env var.")
endif()

# Pfade
set(PROTO_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/protobuf")
set(GEN_DIR       "${CMAKE_CURRENT_BINARY_DIR}")

file(GLOB SPOTIFY_PROTOS "${PROTO_SRC_DIR}/*.proto")
list(REMOVE_DUPLICATES SPOTIFY_PROTOS)

set(SPOTIFY_PROTO_SRCS)
set(SPOTIFY_PROTO_HDRS)
foreach(proto ${SPOTIFY_PROTOS})
  get_filename_component(base "${proto}" NAME_WE)

  # Descriptor kann flach liegen
  set(desc  "${GEN_DIR}/${base}.pb.desc")
  # Generierte C/H liegen UNTER ${GEN_DIR}/protobuf
  set(gen_c "${GEN_DIR}/protobuf/${base}.pb.c")
  set(gen_h "${GEN_DIR}/protobuf/${base}.pb.h")

  add_custom_command(
    OUTPUT  "${desc}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${GEN_DIR}"
    COMMAND "${PROTOC}"
            -I "${CMAKE_CURRENT_SOURCE_DIR}"   # eine Ebene über "protobuf"
            -I "${PROTO_SRC_DIR}"
            -I "${NANOPB_PROTO_DIR}"
            --include_imports
            --descriptor_set_out "${desc}"
            "${proto}"
    DEPENDS "${proto}" ${SPOTIFY_PROTOS}
    VERBATIM
  )

  add_custom_command(
    OUTPUT  "${gen_c}" "${gen_h}"
    COMMAND "${Python3_EXECUTABLE}" "${NANOPB_GEN_PY}"
            --output-dir "${GEN_DIR}"          # schreibt nach ${GEN_DIR}/protobuf/...
            --options-path "${PROTO_SRC_DIR}"
            "${desc}"
    DEPENDS "${desc}" "${NANOPB_GEN_PY}"
    VERBATIM
  )

  list(APPEND SPOTIFY_PROTO_SRCS "${gen_c}")
  list(APPEND SPOTIFY_PROTO_HDRS "${gen_h}")
endforeach()

# Quellcode
file(GLOB SPOTIFY_SRC "src/*.cpp" "src/*.c")

add_library(stream_spotify STATIC
  ${SPOTIFY_SRC}
  ${SPOTIFY_PROTO_SRCS}
)

# Damit Ninja weiß, dass die generierten Dateien vor dem Build da sein müssen
add_custom_target(spotify_protos DEPENDS ${SPOTIFY_PROTO_SRCS} ${SPOTIFY_PROTO_HDRS})
add_dependencies(stream_spotify spotify_protos)

target_include_directories(stream_spotify PUBLIC
  "${CMAKE_CURRENT_BINARY_DIR}"
  "${GEN_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
  "${NANOPB_ROOT}"
)

target_link_libraries(stream_spotify PUBLIC StreamCore32::core)
target_compile_definitions(stream_spotify PUBLIC PB_ENABLE_MALLOC PB_FIELD_32BIT)

add_library(StreamCore32::stream_spotify ALIAS stream_spotify)

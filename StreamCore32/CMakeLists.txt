cmake_minimum_required(VERSION 3.15)
project(StreamCore32 C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Falls bell extern gebaut wird: -DSTREAMCORE_EXTERNAL_BELL=<target>
set(STREAMCORE_EXTERNAL_BELL "" CACHE STRING "External bell target")

# Nanopb-FindModul von bell bekannt machen
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/bell/external/nanopb/extra")
  list(APPEND CMAKE_MODULE_PATH
       "${CMAKE_CURRENT_SOURCE_DIR}/bell/external/nanopb/extra")
endif()
# ---- bell einbinden / verfügbar machen -----------------------------
if(STREAMCORE_EXTERNAL_BELL)
  set(BELL_TARGET ${STREAMCORE_EXTERNAL_BELL})
else()
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/bell/CMakeLists.txt")
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/bell")
    # die bell-Repo-Targets nennen sich üblicherweise einfach 'bell'
    target_compile_definitions(bell PUBLIC PB_ENABLE_MALLOC PB_FIELD_32BIT)
    add_library(StreamCore32::bell ALIAS bell)
    set(BELL_TARGET StreamCore32::bell)
  else()
    message(FATAL_ERROR "bell nicht gefunden: ${CMAKE_CURRENT_SOURCE_DIR}/bell")
  endif()
endif()
# ---- optionale Teilbäume -------------------------------------------
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/core/CMakeLists.txt")
  add_subdirectory(core)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/stream/CMakeLists.txt")
  add_subdirectory(stream)
endif()

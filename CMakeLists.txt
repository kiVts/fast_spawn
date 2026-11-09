cmake_minimum_required(VERSION 3.24)
project(fast_spawn LANGUAGES C CXX)

# Needs the SWML SDK folder: cmake -B build -A x64 -DSWML_SDK_DIR=path/to/sdk
set(SWML_SDK_DIR "" CACHE PATH "SWML SDK folder (the one with SwmlMod.cmake)")
if(NOT EXISTS "${SWML_SDK_DIR}/SwmlMod.cmake")
  message(FATAL_ERROR "Set SWML_SDK_DIR to the SWML SDK folder")
endif()
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_subdirectory("${SWML_SDK_DIR}" swml_sdk)

swml_add_mod(fast_spawn SOURCES
  src/fast_spawn.cpp
  src/watch.cpp
  src/vehicle_jobs.cpp
  src/sound_jobs.cpp
  src/file_cache.cpp
  src/warmup.cpp
  src/load_heap.cpp
  src/setup.cpp
)
target_include_directories(fast_spawn PRIVATE src)

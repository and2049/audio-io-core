cmake_minimum_required(VERSION 3.25)
set(AUDIO_ENGINE_REQUESTED_BUILD_TYPE "${CMAKE_BUILD_TYPE}")
project(AudioEngine VERSION 0.1.0 LANGUAGES C CXX)

if(PROJECT_IS_TOP_LEVEL
   AND NOT CMAKE_CONFIGURATION_TYPES
   AND "${AUDIO_ENGINE_REQUESTED_BUILD_TYPE}" STREQUAL "")
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

add_library(audio_engine INTERFACE)
add_library(audio_engine::audio_engine ALIAS audio_engine)
target_include_directories(audio_engine INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")

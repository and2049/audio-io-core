cmake_minimum_required(VERSION 3.25)
set(AUDIO_ENGINE_REQUESTED_BUILD_TYPE "${CMAKE_BUILD_TYPE}")
project(AudioEngine VERSION 0.1.0 LANGUAGES C CXX)

if(PROJECT_IS_TOP_LEVEL
   AND NOT CMAKE_CONFIGURATION_TYPES
   AND "${AUDIO_ENGINE_REQUESTED_BUILD_TYPE}" STREQUAL "")
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(PROJECT_IS_TOP_LEVEL)
  include(CTest)
  set(AUDIO_ENGINE_TESTS_DEFAULT ${BUILD_TESTING})
else()
  set(AUDIO_ENGINE_TESTS_DEFAULT OFF)
endif()

option(AUDIO_ENGINE_BUILD_TESTS "Build the audio_engine tests" ${AUDIO_ENGINE_TESTS_DEFAULT})

function(audio_engine_set_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 $<$<COMPILE_LANGUAGE:CXX>:/permissive->)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
  endif()
endfunction()

add_library(audio_engine STATIC src/engine.cpp)
add_library(audio_engine::audio_engine ALIAS audio_engine)
target_compile_features(audio_engine PRIVATE cxx_std_20)
target_include_directories(audio_engine PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
set_target_properties(audio_engine PROPERTIES CXX_EXTENSIONS OFF)
audio_engine_set_warnings(audio_engine)

if(AUDIO_ENGINE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(unittests)
endif()

include(tools/DeveloperTools.cmake)

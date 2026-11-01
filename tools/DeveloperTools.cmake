if(NOT PROJECT_IS_TOP_LEVEL)
  return()
endif()

get_filename_component(compiler_directory "${CMAKE_CXX_COMPILER}" DIRECTORY)
find_program(AUDIO_ENGINE_CLANG_FORMAT clang-format HINTS "${compiler_directory}")
if(AUDIO_ENGINE_CLANG_FORMAT)
  file(GLOB_RECURSE format_sources CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/include/*.h"
    "${PROJECT_SOURCE_DIR}/src/*.h"
    "${PROJECT_SOURCE_DIR}/src/*.hpp"
    "${PROJECT_SOURCE_DIR}/src/*.cpp"
    "${PROJECT_SOURCE_DIR}/unittests/*.h"
    "${PROJECT_SOURCE_DIR}/unittests/*.hpp"
    "${PROJECT_SOURCE_DIR}/unittests/*.c"
    "${PROJECT_SOURCE_DIR}/unittests/*.cpp"
    "${PROJECT_SOURCE_DIR}/examples/*.c"
    "${PROJECT_SOURCE_DIR}/examples/*.cpp"
  )
  add_custom_target(format
    COMMAND "${AUDIO_ENGINE_CLANG_FORMAT}" -i ${format_sources}
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    VERBATIM
  )
  add_custom_target(format-check
    COMMAND "${AUDIO_ENGINE_CLANG_FORMAT}" --dry-run --Werror ${format_sources}
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    VERBATIM
  )
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND CMAKE_EXPORT_COMPILE_COMMANDS)
  add_custom_target(ir
    COMMAND "${CMAKE_COMMAND}"
      "-DBUILD_DIR=${PROJECT_BINARY_DIR}"
      "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}"
      -P "${CMAKE_CURRENT_LIST_DIR}/emit-ir.cmake"
    VERBATIM
  )
endif()

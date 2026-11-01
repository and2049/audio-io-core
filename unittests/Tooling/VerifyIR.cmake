cmake_minimum_required(VERSION 3.25)

file(MAKE_DIRECTORY "${WORK_DIR}")
configure_file("${CMAKE_CURRENT_LIST_DIR}/IRFixture.cpp" "${WORK_DIR}/fixture.cpp" COPYONLY)
if(CL_MODE)
  set(command "\"${CLANG}\" /nologo /O2 /DNDEBUG /clang:-O3 /FoCMakeFiles/audio_engine.dir/fixture.obj /c -- \"${WORK_DIR}/fixture.cpp\"")
else()
  set(command "\"${CLANG}\" -O3 -DNDEBUG -o CMakeFiles/audio_engine.dir/fixture.o -c -- \"${WORK_DIR}/fixture.cpp\"")
endif()
string(REPLACE "\\" "\\\\" escaped_command "${command}")
string(REPLACE "\"" "\\\"" escaped_command "${escaped_command}")
file(WRITE "${WORK_DIR}/compile_commands.json"
  "[{\"directory\":\"${WORK_DIR}\",\"file\":\"${WORK_DIR}/fixture.cpp\",\"command\":\"${escaped_command}\"},"
  "{\"directory\":\"${WORK_DIR}\",\"file\":\"ignored.cpp\",\"command\":\"invalid-compiler -o CMakeFiles/unrelated.dir/ignored.o\"}]")

execute_process(COMMAND "${CMAKE_COMMAND}"
  "-DBUILD_DIR=${WORK_DIR}" "-DSOURCE_DIR=${WORK_DIR}"
  -P "${ENGINE_SOURCE_DIR}/tools/emit-ir.cmake"
  COMMAND_ERROR_IS_FATAL ANY
)
set(stem "${WORK_DIR}/ir/fixture.cpp")
foreach(suffix IN ITEMS ll opt.yaml command.txt)
  if(NOT EXISTS "${stem}.${suffix}")
    message(FATAL_ERROR "Missing IR artifact: ${stem}.${suffix}")
  endif()
endforeach()
file(READ "${stem}.ll" ir)
file(READ "${stem}.command.txt" recorded_command)
if(NOT ir MATCHES "define.*@apply_gain" OR ir MATCHES "optnone")
  message(FATAL_ERROR "Expected optimized LLVM IR for apply_gain.")
endif()
if(NOT recorded_command MATCHES "clang version" OR NOT recorded_command MATCHES "-O3")
  message(FATAL_ERROR "Compiler version or optimization arguments were lost.")
endif()
if(EXISTS "${WORK_DIR}/CMakeFiles/audio_engine.dir/fixture.obj"
    OR EXISTS "${WORK_DIR}/CMakeFiles/audio_engine.dir/fixture.o")
  message(FATAL_ERROR "The IR tool wrote to the original object output.")
endif()

file(WRITE "${WORK_DIR}/compile_commands.json" "[]")
execute_process(COMMAND "${CMAKE_COMMAND}"
  "-DBUILD_DIR=${WORK_DIR}" "-DSOURCE_DIR=${WORK_DIR}"
  -P "${ENGINE_SOURCE_DIR}/tools/emit-ir.cmake"
  RESULT_VARIABLE result OUTPUT_QUIET ERROR_QUIET
)
if(result STREQUAL "0")
  message(FATAL_ERROR "An empty compilation database must fail.")
endif()

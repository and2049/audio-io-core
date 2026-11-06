find_package(Threads REQUIRED)

if(AUDIO_ENGINE_ENABLE_ASIO)
  add_executable(AudioEngineAsioTest Windows/AsioTest.cpp)
  target_link_libraries(AudioEngineAsioTest PRIVATE AudioEngineTestBackend)
  target_include_directories(AudioEngineAsioTest SYSTEM PRIVATE "${AUDIO_ENGINE_ASIO_SDK}/common")
  target_compile_definitions(AudioEngineAsioTest PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_compile_features(AudioEngineAsioTest PRIVATE cxx_std_20)
  audio_engine_set_warnings(AudioEngineAsioTest)
  add_test(NAME AudioEngine.ASIO COMMAND AudioEngineAsioTest)
  set_tests_properties(AudioEngine.ASIO PROPERTIES TIMEOUT 30)
endif()

if(AUDIO_ENGINE_ENABLE_COREAUDIO)
  add_executable(AudioEngineCoreAudioTest Darwin/CoreAudioTest.cpp)
  target_link_libraries(AudioEngineCoreAudioTest PRIVATE AudioEngineTestBackend)
  target_compile_features(AudioEngineCoreAudioTest PRIVATE cxx_std_20)
  audio_engine_set_warnings(AudioEngineCoreAudioTest)
  add_test(NAME AudioEngine.CoreAudio COMMAND AudioEngineCoreAudioTest)
  set_tests_properties(AudioEngine.CoreAudio PROPERTIES TIMEOUT 30)
endif()

add_executable(AudioEngineConversionTest ConversionTest.cpp)
target_link_libraries(AudioEngineConversionTest PRIVATE AudioEngineTestBackend)
target_compile_features(AudioEngineConversionTest PRIVATE cxx_std_20)
audio_engine_set_warnings(AudioEngineConversionTest)
add_test(NAME AudioEngine.Conversion COMMAND AudioEngineConversionTest)

add_executable(AudioEngineCapabilitiesTest CapabilitiesTest.cpp)
target_link_libraries(AudioEngineCapabilitiesTest PRIVATE AudioEngineTestBackend)
target_compile_features(AudioEngineCapabilitiesTest PRIVATE cxx_std_20)
audio_engine_set_warnings(AudioEngineCapabilitiesTest)
add_test(NAME AudioEngine.Capabilities COMMAND AudioEngineCapabilitiesTest)
set_tests_properties(AudioEngine.Capabilities PROPERTIES TIMEOUT 30)

add_library(AudioEngineTestBackend STATIC TestBackend.cpp)
target_link_libraries(AudioEngineTestBackend PUBLIC audio_engine::audio_engine Threads::Threads)
target_include_directories(AudioEngineTestBackend PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
                                                         "${PROJECT_SOURCE_DIR}/src")
set_target_properties(AudioEngineTestBackend PROPERTIES CXX_STANDARD 20
                                                        CXX_STANDARD_REQUIRED YES
                                                        CXX_EXTENSIONS OFF)
audio_engine_set_warnings(AudioEngineTestBackend)

add_executable(AudioEngineValidationTest ValidationTest.cpp)
target_link_libraries(AudioEngineValidationTest PRIVATE AudioEngineTestBackend)
set_target_properties(AudioEngineValidationTest PROPERTIES CXX_STANDARD 20
                                                           CXX_STANDARD_REQUIRED YES
                                                           CXX_EXTENSIONS OFF)
audio_engine_set_warnings(AudioEngineValidationTest)
add_test(NAME AudioEngine.Validation COMMAND AudioEngineValidationTest)
set_tests_properties(AudioEngine.Validation PROPERTIES TIMEOUT 30)

add_executable(AudioEngineLifecycleTest LifecycleTest.cpp)
target_link_libraries(AudioEngineLifecycleTest PRIVATE AudioEngineTestBackend)
set_target_properties(AudioEngineLifecycleTest PROPERTIES CXX_STANDARD 20
                                                          CXX_STANDARD_REQUIRED YES
                                                          CXX_EXTENSIONS OFF)
audio_engine_set_warnings(AudioEngineLifecycleTest)
add_test(NAME AudioEngine.Lifecycle COMMAND AudioEngineLifecycleTest)
set_tests_properties(AudioEngine.Lifecycle PROPERTIES TIMEOUT 30)

add_executable(AudioEngineCApiTest CApiTest.c)
target_link_libraries(AudioEngineCApiTest PRIVATE audio_engine::audio_engine)
set_target_properties(AudioEngineCApiTest PROPERTIES C_STANDARD 11
                                                     C_STANDARD_REQUIRED YES
                                                     C_EXTENSIONS OFF
                                                     LINKER_LANGUAGE CXX)
audio_engine_set_warnings(AudioEngineCApiTest)
add_test(NAME AudioEngine.CApi COMMAND AudioEngineCApiTest)
set_tests_properties(AudioEngine.CApi PROPERTIES TIMEOUT 30)

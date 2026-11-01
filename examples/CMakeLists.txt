add_executable(AudioEngineHeadlessExample headless/main.c)
target_link_libraries(AudioEngineHeadlessExample PRIVATE audio_engine::audio_engine)
set_target_properties(AudioEngineHeadlessExample PROPERTIES C_STANDARD 11
                                                            C_STANDARD_REQUIRED YES
                                                            C_EXTENSIONS OFF
                                                            LINKER_LANGUAGE CXX)
audio_engine_set_warnings(AudioEngineHeadlessExample)

add_executable(AudioEngineDeviceCheck device-check/main.cpp)
target_link_libraries(AudioEngineDeviceCheck PRIVATE audio_engine::audio_engine)
target_compile_features(AudioEngineDeviceCheck PRIVATE cxx_std_20)
audio_engine_set_warnings(AudioEngineDeviceCheck)

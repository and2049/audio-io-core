add_executable(AudioEngineHeadlessExample headless/main.c)
target_link_libraries(AudioEngineHeadlessExample PRIVATE audio_engine::audio_engine)
set_target_properties(AudioEngineHeadlessExample PROPERTIES C_STANDARD 11
                                                            C_STANDARD_REQUIRED YES
                                                            C_EXTENSIONS OFF
                                                            LINKER_LANGUAGE CXX)
audio_engine_set_warnings(AudioEngineHeadlessExample)

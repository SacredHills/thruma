add_executable(thruma_tests
    TestMain.cpp
    TestCore.cpp
    TestDetection.cpp
    TestSampleEngine.cpp
    TestReplace.cpp
    TestDebleed.cpp
    TestRealtime.cpp)

target_link_libraries(thruma_tests PRIVATE thruma_core)
target_compile_definitions(thruma_tests PRIVATE THRUMA_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")

add_test(NAME thruma_dsp_tests COMMAND thruma_tests)
set_tests_properties(thruma_dsp_tests PROPERTIES TIMEOUT 900)

# Host-level validation of the built VST3 (loads the real binary through JUCE's VST3 host).
if(THRUMA_BUILD_PLUGIN)
    juce_add_console_app(ThrumaHostValidator PRODUCT_NAME "ThrumaHostValidator")
    target_sources(ThrumaHostValidator PRIVATE HostValidator/Main.cpp)
    target_compile_definitions(ThrumaHostValidator PRIVATE
        JUCE_PLUGINHOST_VST3=1 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1)
    target_link_libraries(ThrumaHostValidator PRIVATE
        juce::juce_audio_processors juce::juce_audio_utils juce::juce_gui_extra
        juce::juce_recommended_config_flags)
    add_dependencies(ThrumaHostValidator Thruma_VST3)
endif()

add_executable(thruma_tests
    TestMain.cpp
    TestCore.cpp
    TestDetection.cpp
    TestSampleEngine.cpp
    TestReplace.cpp
    TestDebleed.cpp
    TestRealtime.cpp
    TestRealRecordings.cpp
    TestPreProcessing.cpp)

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

# Developer screenshot tool (not built by default): -DTHRUMA_BUILD_SCREENSHOT=ON
if(THRUMA_BUILD_PLUGIN AND THRUMA_BUILD_SCREENSHOT)
    add_executable(ThrumaScreenshot Screenshot/Main.cpp)
    target_link_libraries(ThrumaScreenshot PRIVATE Thruma thruma_core)
    target_include_directories(ThrumaScreenshot PRIVATE $<TARGET_PROPERTY:Thruma,INCLUDE_DIRECTORIES>)
    target_compile_definitions(ThrumaScreenshot PRIVATE $<TARGET_PROPERTY:Thruma,COMPILE_DEFINITIONS>)
endif()
if(THRUMA_BUILD_PLUGIN AND THRUMA_BUILD_SCREENSHOT)
    add_executable(ThrumaLogDebug Screenshot/LogDebug.cpp)
    target_link_libraries(ThrumaLogDebug PRIVATE Thruma thruma_core)
    target_include_directories(ThrumaLogDebug PRIVATE $<TARGET_PROPERTY:Thruma,INCLUDE_DIRECTORIES>)
    target_compile_definitions(ThrumaLogDebug PRIVATE $<TARGET_PROPERTY:Thruma,COMPILE_DEFINITIONS>)
    set_target_properties(ThrumaLogDebug PROPERTIES ENABLE_EXPORTS ON)   # exports a fake host API for HostBridge
endif()

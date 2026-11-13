set(THRUMA_FORMATS VST3)
if(THRUMA_EXTRA_FORMATS)
    list(APPEND THRUMA_FORMATS ${THRUMA_EXTRA_FORMATS})   # e.g. -DTHRUMA_EXTRA_FORMATS="Standalone;AU"
endif()

juce_add_plugin(Thruma
    PRODUCT_NAME                "Thruma"
    COMPANY_NAME                "SacredHills"
    COMPANY_WEBSITE             "https://github.com/SacredHills/thruma"
    BUNDLE_ID                   "com.sacredhills.thruma"
    PLUGIN_MANUFACTURER_CODE    Sacr
    PLUGIN_CODE                 Thrm
    FORMATS                     ${THRUMA_FORMATS}
    VERSION                     ${PROJECT_VERSION}
    IS_SYNTH                    FALSE
    NEEDS_MIDI_INPUT            TRUE
    NEEDS_MIDI_OUTPUT           TRUE
    IS_MIDI_EFFECT              FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    COPY_PLUGIN_AFTER_BUILD     FALSE
    VST3_CATEGORIES             Fx Drum Tools
    VST3_AUTO_MANIFEST          FALSE
    DESCRIPTION                 "Drum replacement and debleed")

target_sources(Thruma PRIVATE
    Parameters.cpp
    KitManager.cpp
    PluginProcessor.cpp
    PluginEditor.cpp
    UI/LookAndFeel.cpp
    UI/Widgets.cpp
    UI/WaveformEditor.cpp
    UI/ReplacePage.cpp
    UI/DebleedPage.cpp
    UI/SamplesPage.cpp)

target_include_directories(Thruma PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_compile_definitions(Thruma PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_VST3_EMULATE_MIDI_CC_WITH_PARAMETERS=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_REPORT_APP_USAGE=0
    JUCE_MODAL_LOOPS_PERMITTED=0
    JUCE_USE_FLAC=1
    JUCE_USE_OGGVORBIS=0
    JUCE_USE_MP3AUDIOFORMAT=0)

target_link_libraries(Thruma
    PRIVATE
        thruma_core
        juce::juce_audio_utils
        juce::juce_audio_formats
        juce::juce_audio_processors
        juce::juce_gui_extra
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

#-------------------------------------------------------------------------------
# Install rules (used by the Linux package; Windows/macOS installers use packaging/ scripts)
if(UNIX AND NOT APPLE)
    # TARGET_FILE is Thruma.vst3/Contents/x86_64-linux/Thruma.so -> install the whole bundle
    install(DIRECTORY "$<TARGET_FILE_DIR:Thruma_VST3>/../../" DESTINATION lib/vst3/Thruma.vst3 USE_SOURCE_PERMISSIONS)
    install(FILES "${PROJECT_SOURCE_DIR}/README.md" "${PROJECT_SOURCE_DIR}/docs/USER_GUIDE.md"
            DESTINATION share/doc/thruma)
endif()

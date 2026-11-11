cmake_minimum_required(VERSION 3.22)

set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "Minimum macOS version")

project(Atelier VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(ATELIER_INSTALL_PLUGINS "Copy built plugins into ~/Library/Audio/Plug-Ins" ON)

add_subdirectory(JUCE)

juce_add_plugin(Atelier
    COMPANY_NAME "Channel Audio"
    BUNDLE_ID "audio.channel.atelier"
    PLUGIN_MANUFACTURER_CODE Chnl
    PLUGIN_CODE Atlr
    FORMATS AU VST3 Standalone
    PRODUCT_NAME "Atelier"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    AU_MAIN_TYPE kAudioUnitType_Effect
    VST3_CATEGORIES Fx Dynamics EQ Mastering
    COPY_PLUGIN_AFTER_BUILD ${ATELIER_INSTALL_PLUGINS}
    NEEDS_WEB_BROWSER FALSE
    NEEDS_CURL FALSE)

target_sources(Atelier PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)

target_include_directories(Atelier PUBLIC Source)

target_compile_definitions(Atelier PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_MODAL_LOOPS_PERMITTED=0)

target_link_libraries(Atelier
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# Offline test harness: runs audio through the processor and checks the results.
juce_add_console_app(AtelierTests PRODUCT_NAME "AtelierTests")
target_sources(AtelierTests PRIVATE Tests/Main.cpp)
target_link_libraries(AtelierTests PRIVATE Atelier juce::juce_audio_utils juce::juce_dsp)
target_compile_options(AtelierTests PRIVATE -Wno-macro-redefined)

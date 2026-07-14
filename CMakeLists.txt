cmake_minimum_required(VERSION 3.25)
project(MetaPlugin VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_subdirectory(external/JUCE SYSTEM)

juce_add_plugin(MetaPlugin
    COMPANY_NAME "uhyun"
    PLUGIN_MANUFACTURER_CODE Uhyn
    PLUGIN_CODE Mtpl
    FORMATS VST3 AU Standalone
    PRODUCT_NAME "MetaPlugin"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    COPY_PLUGIN_AFTER_BUILD TRUE
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "MetaPlugin needs audio input to process it."
)

target_sources(MetaPlugin PRIVATE
    src/PluginProcessor.cpp
    src/dsp/SvfPeakNode.h
)

# Warnings on our sources only — JUCE module sources compile inside this same
# target and are not clean under -Wconversion.
set_source_files_properties(src/PluginProcessor.cpp PROPERTIES
    COMPILE_OPTIONS "-Wall;-Wextra;-Wconversion;-Wshadow"
)

target_compile_definitions(MetaPlugin PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
)

target_link_libraries(MetaPlugin
    PRIVATE
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
)

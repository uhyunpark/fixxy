cmake_minimum_required(VERSION 3.25)
project(LpngSound VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_subdirectory(external/JUCE SYSTEM)

# Each classic effect ships as its own plugin product (see PROJECT.md — "제품
# 구조"). They share the src/dsp/ node library. This function wraps the
# per-target boilerplate that is identical across products; anything
# product-specific (name, code, sources) stays an explicit argument.
function(lpng_add_plugin)
    set(oneValueArgs TARGET PRODUCT_NAME PLUGIN_CODE)
    set(multiValueArgs SOURCES)
    cmake_parse_arguments(ARG "" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    juce_add_plugin(${ARG_TARGET}
        COMPANY_NAME "uhyun"
        PLUGIN_MANUFACTURER_CODE Uhyn
        PLUGIN_CODE ${ARG_PLUGIN_CODE}
        FORMATS VST3 AU Standalone
        PRODUCT_NAME "${ARG_PRODUCT_NAME}"
        IS_SYNTH FALSE
        NEEDS_MIDI_INPUT FALSE
        NEEDS_MIDI_OUTPUT FALSE
        COPY_PLUGIN_AFTER_BUILD TRUE
        MICROPHONE_PERMISSION_ENABLED TRUE
        MICROPHONE_PERMISSION_TEXT "${ARG_PRODUCT_NAME} needs audio input to process it."
    )

    target_sources(${ARG_TARGET} PRIVATE ${ARG_SOURCES})

    # src/ is the include root so plugin code says #include "dsp/SvfPeakNode.h"
    # regardless of which product directory it lives in.
    target_include_directories(${ARG_TARGET} PRIVATE ${CMAKE_SOURCE_DIR}/src)

    # Warnings on our sources only — JUCE module sources compile inside this
    # same target and are not clean under -Wconversion.
    foreach(src ${ARG_SOURCES})
        if(src MATCHES "\\.cpp$")
            set_source_files_properties(${src} PROPERTIES
                COMPILE_OPTIONS "-Wall;-Wextra;-Wconversion;-Wshadow"
            )
        endif()
    endforeach()

    target_compile_definitions(${ARG_TARGET} PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
    )

    target_link_libraries(${ARG_TARGET}
        PRIVATE
            juce::juce_audio_utils
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_lto_flags
    )
endfunction()

lpng_add_plugin(
    TARGET EQ
    PRODUCT_NAME "LpngEQ"
    PLUGIN_CODE Lpeq
    SOURCES
        src/plugins/eq/PluginProcessor.cpp
        src/plugins/eq/ui/CoordMap.h
        src/plugins/eq/ui/BackgroundPanel.h
        src/plugins/eq/ui/CurveView.h
        src/plugins/eq/ui/EqEditor.h
        src/dsp/SvfBandNode.h
        src/dsp/MultibandEq.h
        src/dsp/BandResponse.h
)

# Offline unit test: BandResponse.h's analytic curve vs. the actual SvfBandNode
# kernel (impulse + DFT). Deliberately plain C++, no JUCE — kept out of the
# plugin target so it builds fast and can't drag JUCE headers into scope.
add_executable(BandResponseTest test/band_response_test.cpp)
target_include_directories(BandResponseTest PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_features(BandResponseTest PRIVATE cxx_std_20)

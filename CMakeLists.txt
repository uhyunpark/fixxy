cmake_minimum_required(VERSION 3.25)
project(FixxySound VERSION 0.1.0)

# 빌드 타입 미지정 = CMake 기본값이 빈 값(-O0) — DAW에서 시스템 과부하를 일으킨다
# (docs/architecture.md 함정). 기본을 Release로 강제.
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_subdirectory(external/JUCE SYSTEM)

# Each classic effect ships as its own plugin product (see docs/architecture.md — "제품
# 구조"). They share the src/dsp/ node library. This function wraps the
# per-target boilerplate that is identical across products; anything
# product-specific (name, code, sources) stays an explicit argument.
function(fixxy_add_plugin)
    set(oneValueArgs TARGET PRODUCT_NAME PLUGIN_CODE)
    set(multiValueArgs SOURCES)
    cmake_parse_arguments(ARG "" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    juce_add_plugin(${ARG_TARGET}
        BUNDLE_ID "com.uhyun.${ARG_TARGET}"
        AU_EXPORT_PREFIX "Fixxy${ARG_TARGET}AU"
        COMPANY_NAME "fixxy"
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
            juce::juce_dsp
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_lto_flags
    )
endfunction()

fixxy_add_plugin(
    TARGET EQ
    PRODUCT_NAME "fixxy EQ"
    PLUGIN_CODE Lpeq
    SOURCES
        src/plugins/eq/PluginProcessor.cpp
        src/plugins/ui/SpectrumFifo.h
        src/plugins/eq/ui/CoordMap.h
        src/plugins/eq/ui/BackgroundPanel.h
        src/plugins/eq/ui/CurveView.h
        src/plugins/ui/AnalyzerEngine.h
        src/plugins/eq/ui/AnalyzerView.h
        src/plugins/eq/ui/DraggerLayer.h
        src/plugins/eq/ui/EqEditor.h
        src/dsp/SvfBandNode.h
        src/dsp/MultibandEq.h
        src/dsp/BandResponse.h
        src/dsp/MatchedCoeffs.h
)

fixxy_add_plugin(
    TARGET Comp
    PRODUCT_NAME "fixxy Comp"
    PLUGIN_CODE Lpcm
    SOURCES
        src/plugins/comp/PluginProcessor.cpp
        src/plugins/FixxyLookAndFeel.h
        src/plugins/comp/ui/CompMeterMap.h
        src/plugins/comp/ui/CompCurveMap.h
        src/plugins/comp/ui/LevelMeter.h
        src/plugins/comp/ui/GrMeter.h
        src/plugins/comp/ui/TransferCurveView.h
        src/plugins/comp/ui/CompEditor.h
        src/dsp/CompressorNode.h
)

fixxy_add_plugin(
    TARGET Spectral
    PRODUCT_NAME "fixxy Spectral"
    PLUGIN_CODE Lpsp
    SOURCES
        src/plugins/spectral/PluginProcessor.cpp
        src/plugins/FixxyLookAndFeel.h
        src/plugins/spectral/SpectralMix.h
        src/plugins/spectral/ReductionPublisher.h
        src/plugins/spectral/ui/SpectralDisplayMap.h
        src/plugins/spectral/ui/SpectralAnalyzerView.h
        src/plugins/spectral/ui/WeightBandLayer.h
        src/plugins/spectral/ui/SpectralEditor.h
        src/plugins/ui/SpectrumFifo.h
        src/plugins/ui/AnalyzerEngine.h
        src/dsp/SpectralNode.h
        src/dsp/SpectralFft.h
)

# Offline unit test: BandResponse.h's analytic curve vs. the actual SvfBandNode
# kernel (impulse + DFT). Deliberately plain C++, no JUCE — kept out of the
# plugin target so it builds fast and can't drag JUCE headers into scope.
add_executable(BandResponseTest test/band_response_test.cpp)
target_include_directories(BandResponseTest PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_features(BandResponseTest PRIVATE cxx_std_20)

# Offline unit test: Ivantsov matched kernel (MatchedCoeffs.h + SvfBandNode's
# normal-form path) vs. proto-generated fixtures (test/matched_kernel_fixtures.h
# from proto/dump_matched_fixtures.py). Coefficients to 1e-12, magnitudes to
# 1e-9 dB, realization reconstruction, passthrough bit-exactness, modulation
# sanity. Plain C++, no JUCE — same rationale as above.
add_executable(MatchedKernelTest test/matched_kernel_test.cpp)
target_include_directories(MatchedKernelTest PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/test)
target_compile_features(MatchedKernelTest PRIVATE cxx_std_20)
set_source_files_properties(test/matched_kernel_test.cpp PROPERTIES
    COMPILE_OPTIONS "-Wall;-Wextra;-Wconversion;-Wshadow")

# Offline unit test: CompressorNode vs. proto/compressor.py asserts (static curve,
# C1 knee, ballistics, null). Plain C++, no JUCE — same rationale as above.
add_executable(CompressorNodeTest test/compressor_node_test.cpp)
target_include_directories(CompressorNodeTest PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_features(CompressorNodeTest PRIVATE cxx_std_20)
set_source_files_properties(test/compressor_node_test.cpp PROPERTIES
    COMPILE_OPTIONS "-Wall;-Wextra;-Wconversion;-Wshadow")

# Offline unit test: the dynamic-EQ path in SvfBandNode (detector -> knee ->
# ballistics -> K-cadence matched-bell re-bake -> Direct Form I) vs. proto-
# generated fixtures (test/dynamic_eq_fixtures.h from
# proto/dump_dynamic_fixtures.py). offsetDb trajectory to 1e-9, settled coeffs to
# 1e-12, below-threshold bit-exactness, settled null, symmetry, DF1 impulse vs
# transfer, worst-corner deep-cut stability. Plain C++, no JUCE — same rationale.
add_executable(DynamicEqTest test/dynamic_eq_test.cpp)
target_include_directories(DynamicEqTest PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/test)
target_compile_features(DynamicEqTest PRIVATE cxx_std_20)
set_source_files_properties(test/dynamic_eq_test.cpp PROPERTIES
    COMPILE_OPTIONS "-Wall;-Wextra;-Wconversion;-Wshadow")

# AnalyzerEngine test: pushes known signals through the analyzer and asserts the
# dB output (peak bin/level, tilt slope). Needs juce::dsp::FFT, so it's a JUCE
# console app linking juce_dsp only — deliberately kept out of the plugin target.
# Offline unit test: the machine-readable parameter schema (dsp/ParamSchema.h,
# dsp/CompressorSchema.h, dsp/EqSchema.h) vs. an independently transcribed
# copy of the actual APVTS layouts. Plain C++, no JUCE — same rationale as
# above. Also prints the full schema JSON for both plugins.
add_executable(ParamSchemaTest test/param_schema_test.cpp)
target_include_directories(ParamSchemaTest PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_features(ParamSchemaTest PRIVATE cxx_std_20)
set_source_files_properties(test/param_schema_test.cpp PROPERTIES
    COMPILE_OPTIONS "-Wall;-Wextra;-Wconversion;-Wshadow")

# Offline unit test: fixxy Comp editor UI math — meter scale mappers (GR
# magnitude vs dBFS level, the analyzer lesson applied to meters), transfer-
# curve CompCurveMap round-trips, and CompressorNode::staticCurve refactor
# equivalence. Plain C++, no JUCE — the mapped headers are deliberately
# JUCE-free.
add_executable(CompUiMapTest test/comp_ui_map_test.cpp)
target_include_directories(CompUiMapTest PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_features(CompUiMapTest PRIVATE cxx_std_20)
set_source_files_properties(test/comp_ui_map_test.cpp PROPERTIES
    COMPILE_OPTIONS "-Wall;-Wextra;-Wconversion;-Wshadow")

# Offline unit test: SpectralNode / SpectralFft (hand-rolled radix-2 real FFT +
# streaming STFT + envelope/exceed/tanh-knee/octave-smooth/ballistics gain map)
# vs. proto-generated fixtures (test/spectral_fixtures.h from
# proto/dump_spectral_fixtures.py). FFT round-trip/forward, latency==N,
# chunk-size invariance, depth=0 passthrough, full-chain reproduction, stereo
# link. Plain C++, no JUCE — same rationale as above.
add_executable(SpectralNodeTest test/spectral_node_test.cpp)
target_include_directories(SpectralNodeTest PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/test)
target_compile_features(SpectralNodeTest PRIVATE cxx_std_20)
set_source_files_properties(test/spectral_node_test.cpp PROPERTIES
    COMPILE_OPTIONS "-Wall;-Wextra;-Wconversion;-Wshadow")
# SpectralFft.h uses the double vDSP real FFT (vDSP_fft_zripD) on macOS.
if(APPLE)
    target_link_libraries(SpectralNodeTest PRIVATE "-framework Accelerate")
endif()

# Verified MaxCut automation, retained histories and prepared heap audit.
foreach(cap_target SpectralCapNodeTest SpectralCapScalarTest)
    add_executable(${cap_target} test/spectral_cap_node_test.cpp)
    target_include_directories(${cap_target} PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/test)
    target_compile_features(${cap_target} PRIVATE cxx_std_20)
    if(APPLE)
        target_link_libraries(${cap_target} PRIVATE "-framework Accelerate")
    endif()
endforeach()
target_compile_definitions(SpectralCapScalarTest PRIVATE SPECTRALFFT_FORCE_SCALAR=1)
set_source_files_properties(test/spectral_cap_node_test.cpp PROPERTIES
    COMPILE_OPTIONS "-Wall;-Wextra;-Wconversion;-Wshadow")

# Offline unit test: the fixxy Spectral editor UI/publish seams — DisplayMap's
# three separate scales (freq log x, spectrum level dB, reduction ribbon extent),
# the mix/delta/bypass blend algebra (SpectralMix.h), the ReductionPublisher
# seqlock round-trip, and a publish round-trip through the real SpectralNode
# (resonance fixture -> copyReductionDb peaks at the tone's bin). Plain C++, no
# JUCE — every mapped/published header is deliberately JUCE-free.
add_executable(SpectralUiMapTest test/spectral_ui_map_test.cpp)
target_include_directories(SpectralUiMapTest PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_features(SpectralUiMapTest PRIVATE cxx_std_20)
set_source_files_properties(test/spectral_ui_map_test.cpp PROPERTIES
    COMPILE_OPTIONS "-Wall;-Wextra;-Wconversion;-Wshadow")
# SpectralFft.h uses the double vDSP real FFT (vDSP_fft_zripD) on macOS.
if(APPLE)
    target_link_libraries(SpectralUiMapTest PRIVATE "-framework Accelerate")
endif()

juce_add_console_app(AnalyzerEngineTest PRODUCT_NAME "AnalyzerEngineTest")
target_sources(AnalyzerEngineTest PRIVATE test/analyzer_engine_test.cpp)
target_include_directories(AnalyzerEngineTest PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(AnalyzerEngineTest PRIVATE
    JUCE_STANDALONE_APPLICATION=1
    JUCE_USE_CURL=0
    JUCE_WEB_BROWSER=0
)
target_link_libraries(AnalyzerEngineTest
    PRIVATE
        juce::juce_dsp
        juce::juce_recommended_config_flags
)

# Exercise the real APVTS -> processor -> delayed output seam using the same
# shared code as the plugin, without building or copying a plugin wrapper.
add_executable(SpectralProcessorTest test/spectral_processor_test.cpp)
target_include_directories(SpectralProcessorTest PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/external/JUCE/modules)
target_compile_definitions(SpectralProcessorTest PRIVATE
    JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1
    JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0 JucePlugin_Name="fixxy Spectral")
target_link_libraries(SpectralProcessorTest PRIVATE Spectral)
set_source_files_properties(test/spectral_processor_test.cpp PROPERTIES
    COMPILE_OPTIONS "-Wall;-Wextra;-Wconversion;-Wshadow")

# Processor-owned editor workflow: host-notifying A/B snapshots, undoable preset
# loads and workflow metadata in the processor state wrapper.
add_executable(SpectralWorkflowTest test/spectral_workflow_test.cpp)
target_include_directories(SpectralWorkflowTest PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/external/JUCE/modules)
target_compile_definitions(SpectralWorkflowTest PRIVATE
    JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1
    JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0 JucePlugin_Name="fixxy Spectral")
target_link_libraries(SpectralWorkflowTest PRIVATE Spectral)
set_source_files_properties(test/spectral_workflow_test.cpp PROPERTIES
    COMPILE_OPTIONS "-Wall;-Wextra;-Wconversion;-Wshadow")

# Offline reference host: loads installed VST3/AU plug-ins through JUCE's public
# hosting API, inspects their automation surface/latency, and renders WAV files.
juce_add_console_app(ReferencePluginHost PRODUCT_NAME "ReferencePluginHost")
target_sources(ReferencePluginHost PRIVATE test/reference_plugin_host.cpp)
target_compile_definitions(ReferencePluginHost PRIVATE
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_AU=1
    JUCE_STANDALONE_APPLICATION=1
    JUCE_USE_CURL=0
    JUCE_WEB_BROWSER=0
)
target_link_libraries(ReferencePluginHost
    PRIVATE
        juce::juce_audio_formats
        juce::juce_audio_processors
        juce::juce_recommended_config_flags
)
if(APPLE)
    target_link_libraries(ReferencePluginHost PRIVATE
        "-framework AudioUnit"
        "-framework CoreAudioKit")
endif()
set_source_files_properties(test/reference_plugin_host.cpp PROPERTIES
    COMPILE_OPTIONS "-Wall;-Wextra;-Wconversion;-Wshadow")

# Verified static minimum-phase FIR synthesis and zero-buffer-delay convolution.
foreach(fir_target IN ITEMS SpectralFirTest SpectralFirScalarTest)
    add_executable(${fir_target} test/spectral_fir_test.cpp)
    target_include_directories(${fir_target} PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/test)
    target_compile_features(${fir_target} PRIVATE cxx_std_20)
    target_compile_options(${fir_target} PRIVATE -Wall -Wextra -Wconversion -Wshadow)
endforeach()
target_compile_definitions(SpectralFirScalarTest PRIVATE SPECTRALFFT_FORCE_SCALAR=1)
if(APPLE)
    target_link_libraries(SpectralFirTest PRIVATE "-framework Accelerate")
endif()

# Verified spectral control maps, exact median, smoothing and decoupled state.
add_executable(SpectralControlTest test/spectral_control_test.cpp)
target_include_directories(SpectralControlTest PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_features(SpectralControlTest PRIVATE cxx_std_20)
target_compile_options(SpectralControlTest PRIVATE -Wall -Wextra -Wconversion -Wshadow)

foreach(transition_target IN ITEMS SpectralTransitionTest SpectralTransitionScalarTest)
    add_executable(${transition_target} test/spectral_transition_test.cpp)
    target_include_directories(${transition_target} PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_features(${transition_target} PRIVATE cxx_std_20)
    target_compile_options(${transition_target} PRIVATE -Wall -Wextra -Wconversion -Wshadow)
endforeach()
target_compile_definitions(SpectralTransitionScalarTest PRIVATE SPECTRALFFT_FORCE_SCALAR=1)
if(APPLE)
    target_link_libraries(SpectralTransitionTest PRIVATE "-framework Accelerate")
endif()

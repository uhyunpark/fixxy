#pragma once

#include <vector>

#include "dsp/CompressorNode.h"
#include "dsp/ParamSchema.h"

/**
    Machine-readable parameter schema for the compressor plugin, mirroring the
    ACTUAL APVTS layout in src/plugins/comp/PluginProcessor.cpp
    ::createParameterLayout() exactly — ids, ranges, defaults, skews, units.
    This IS the plugin's control surface contract: a future LLM layer emits
    JSON keyed on these ids.

    Ground truth for ids/ranges/defaults/skews: PluginProcessor.cpp. Ground
    truth for the semantics behind each `note`: CompressorNode.h's Params doc
    comments. Cross-checked against both in test/param_schema_test.cpp (the
    drift guard) and, at runtime, against the live apvts in the plugin
    constructor (see PluginProcessor.cpp, JUCE_DEBUG only).
*/
namespace compressorschema {

inline std::vector<paramschema::ParamDescriptor> compressorSchema() {
    using paramschema::ParamDescriptor;
    using paramschema::ParamType;
    using paramschema::Scale;

    std::vector<ParamDescriptor> s;

    s.push_back(ParamDescriptor{
        .id = "threshold",
        .type = ParamType::Float,
        .min = -60.0,
        .max = 0.0,
        .unit = "dB",
        .defaultVal = -20.0,
    });

    s.push_back(ParamDescriptor{
        .id = "ratio",
        .type = ParamType::Float,
        .min = 1.0,
        .max = 20.0,
        .unit = ":1",
        .defaultVal = 4.0,
        .scale = Scale::Skewed,
        .skewValue = 4.0,  // JUCE setSkewForCentre(4.0)
    });

    s.push_back(ParamDescriptor{
        .id = "knee",
        .type = ParamType::Float,
        .min = 0.0,
        .max = 24.0,
        .unit = "dB",
        .defaultVal = 6.0,
        .note = "FULL knee width W: the soft-knee region spans "
                "[threshold - W/2, threshold + W/2] — not a half-width.",
    });

    s.push_back(ParamDescriptor{
        .id = "attack",
        .type = ParamType::Float,
        .min = 0.0,
        .max = 200.0,
        .unit = "ms",
        .defaultVal = 10.0,
        .scale = Scale::Skewed,
        .skewValue = 10.0,  // JUCE setSkewForCentre(10.0)
    });

    s.push_back(ParamDescriptor{
        .id = "release",
        .type = ParamType::Float,
        .min = 1.0,
        .max = 2000.0,
        .unit = "ms",
        .defaultVal = 100.0,
        .scale = Scale::Skewed,
        .skewValue = 100.0,  // JUCE setSkewForCentre(100.0)
    });

    s.push_back(ParamDescriptor{
        .id = "makeup",
        .type = ParamType::Float,
        .min = -24.0,
        .max = 24.0,
        .unit = "dB",
        .defaultVal = 0.0,
    });

    s.push_back(ParamDescriptor{
        .id = "mix",
        .type = ParamType::Float,
        .min = 0.0,
        .max = 100.0,
        .unit = "%",
        .defaultVal = 100.0,
        .note = "dry/wet crossfade. The dry path is the untouched input and "
                "bypasses makeup gain entirely — makeup only applies to the "
                "compressed (wet) path.",
    });

    s.push_back(ParamDescriptor{
        .id = "range",
        .type = ParamType::Float,
        .min = 0.0,
        .max = CompressorNode::kRangeOffDb,
        .unit = "dB",
        .defaultVal = CompressorNode::kRangeOffDb,
        .note = "ceiling on APPLIED gain reduction. The maximum value "
                "(96 dB) is an OFF sentinel, not a literal clamp at 96 dB — "
                "at or above it, gain reduction is unlimited.",
    });

    s.push_back(ParamDescriptor{
        .id = "hold",
        .type = ParamType::Float,
        .min = 0.0,
        .max = 500.0,
        .unit = "ms",
        .defaultVal = 0.0,
        .scale = Scale::Skewed,
        .skewValue = 50.0,  // JUCE setSkewForCentre(50.0)
        .note = "unlike every other float param here, hold is read directly "
                "from the host value each block — it is NOT smoothed.",
    });

    s.push_back(ParamDescriptor{
        .id = "autoMakeup",
        .type = ParamType::Bool,
        .defaultVal = 0.0,
        .note = "when on, overrides the makeup knob with "
                "|threshold|*(1-1/ratio) — a first-order estimate that "
                "assumes the program peaks near 0 dBFS.",
    });

    s.push_back(ParamDescriptor{
        .id = "progRelease",
        .type = ParamType::Bool,
        .defaultVal = 0.0,
        .note = "swaps the single-pole release for a dual-pole fast/slow "
                "blend: sustained gain reduction slows the release, a brief "
                "gain-reduction spike recovers fast (program-dependent "
                "\"glue\").",
    });

    s.push_back(ParamDescriptor{
        .id = "scHpf",
        .type = ParamType::Bool,
        .defaultVal = 0.0,
        .note = "fixed 100 Hz 2-pole highpass in the sidechain/detector path "
                "only — never filters the audio signal path itself.",
    });

    s.push_back(ParamDescriptor{
        .id = "rmsMode",
        .type = ParamType::Bool,
        .defaultVal = 0.0,
        .note = "detector level becomes 10*log10(mean-square) instead of "
                "peak 20*log10|x|, with its own independent time constant "
                "(not the attack/release times).",
    });

    return s;
}

inline std::string compressorSchemaJson() {
    return paramschema::toJson(compressorSchema());
}

}  // namespace compressorschema

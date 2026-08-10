#pragma once

#include <vector>

#include "dsp/ParamSchema.h"
#include "dsp/SpectralNode.h"

/**
    Machine-readable parameter schema for the spectral plugin, mirroring the
    ACTUAL APVTS layout in src/plugins/spectral/PluginProcessor.cpp
    ::createParameterLayout() exactly — ids, ranges, defaults, skews, units.
    This IS the plugin's control surface contract: a future LLM layer emits
    JSON keyed on these ids.

    Ground truth for ids/ranges/defaults/skews: PluginProcessor.cpp. Ground
    truth for the semantics behind each `note`: SpectralNode.h's Params doc
    comments. Cross-checked against both in test/param_schema_test.cpp (the
    drift guard) and, at runtime, against the live apvts in the plugin
    constructor (see PluginProcessor.cpp, JUCE_DEBUG only).
*/
namespace spectralschema {

inline std::vector<paramschema::ParamDescriptor> spectralSchema() {
    using paramschema::ParamDescriptor;
    using paramschema::ParamType;
    using paramschema::Scale;

    std::vector<ParamDescriptor> s;

    s.push_back(ParamDescriptor{
        .id = "depth",
        .type = ParamType::Float,
        .min = 0.0,
        .max = 10.0,
        .defaultVal = 5.0,
        .note = "0 = bypass (bit-near identity); 5 = calibrated soothe-class curves",
    });

    s.push_back(ParamDescriptor{
        .id = "detail",
        .type = ParamType::Float,
        .min = 0.0,
        .max = 10.0,
        .defaultVal = 5.0,
        .note = "single knob folding notch width AND depth",
    });

    s.push_back(ParamDescriptor{
        .id = "attack",
        .type = ParamType::Float,
        .min = 1.0,
        .max = 250.0,
        .unit = "ms",
        .defaultVal = 27.0,
        .scale = Scale::Skewed,
        .skewValue = 27.0,  // JUCE setSkewForCentre(27.0)
    });

    s.push_back(ParamDescriptor{
        .id = "release",
        .type = ParamType::Float,
        .min = 5.0,
        .max = 1000.0,
        .unit = "ms",
        .defaultVal = 50.0,
        .scale = Scale::Skewed,
        .skewValue = 100.0,  // JUCE setSkewForCentre(100.0)
    });

    s.push_back(ParamDescriptor{
        .id = "mix",
        .type = ParamType::Float,
        .min = 0.0,
        .max = 100.0,
        .unit = "%",
        .defaultVal = 100.0,
        .note = "dry path is latency-aligned to the N=1024-sample STFT delay "
                "before the blend",
    });

    s.push_back(ParamDescriptor{
        .id = "output",
        .type = ParamType::Float,
        .min = -24.0,
        .max = 12.0,
        .unit = "dB",
        .defaultVal = 0.0,
    });

    return s;
}

inline std::string spectralSchemaJson() {
    return paramschema::toJson(spectralSchema());
}

}  // namespace spectralschema

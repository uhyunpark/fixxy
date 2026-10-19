#pragma once

#include <array>
#include <string>
#include <vector>

#include "dsp/ParamSchema.h"
#include "dsp/SpectralNode.h"
#include "dsp/SpectralWeight.h"

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

/** Choice order MUST match svfcoeffs::Type (Bell=0, LowShelf, HighShelf,
    Tilt, Notch, Bandpass, Lowpass, Highpass) — see dsp/SpectralWeight.h's
    WeightBand::type contract. */
inline std::vector<std::string> weightBandTypeChoices() {
    return {"bell", "low shelf", "high shelf", "tilt",
            "notch", "bandpass", "lowpass", "highpass"};
}

/** Per-band default center frequencies for the weight curve. MUST match
    kWbandDefaultFreqs in plugins/spectral/PluginProcessor.cpp — duplicated
    here for the same reason as eqschema::defaultBandFreqsHz(): that array is
    local to the .cpp, so this copy is cross-checked against an independently
    transcribed literal in test/param_schema_test.cpp. */
inline const std::array<double, spectralweight::kMaxWeightBands>& defaultWeightBandFreqsHz() {
    static const std::array<double, spectralweight::kMaxWeightBands> f{
        120.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0, 12000.0};
    return f;
}

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

    s.push_back(ParamDescriptor{
        .id = "bypass",
        .type = ParamType::Bool,
        .defaultVal = 0.0,
        .note = "transparent latency-preserving passthrough (output = latency-"
                "aligned dry); ignores mix and output trim",
    });

    s.push_back(ParamDescriptor{
        .id = "delta",
        .type = ParamType::Bool,
        .defaultVal = 0.0,
        .note = "solo the removed component (wet - dry); ignores mix, applies "
                "output trim; depth=0 => silence",
    });

    // --- SIDEBAR globals (proto/spectral_sidebar.py). Every default is an exact
    // no-op; the node collapses to the pre-sidebar shared-gain path there. ---
    s.push_back(ParamDescriptor{
        .id = "maxCut",
        .type = ParamType::Float,
        .min = 1.0,
        .max = 24.0,
        .unit = "dB",
        .defaultVal = 24.0,
        .note = "ceiling on per-bin reduction; 24 = OFF (displayed \"off\"), the "
                "default no-op",
    });

    s.push_back(ParamDescriptor{
        .id = "wetTrim",
        .type = ParamType::Float,
        .min = -12.0,
        .max = 12.0,
        .unit = "dB",
        .defaultVal = 0.0,
        .note = "flat gain on the WET component only; applied in the output stage "
                "(plugins/spectral/SpectralMix.h), NOT in the node",
    });

    s.push_back(ParamDescriptor{
        .id = "msMode",
        .type = ParamType::Bool,
        .defaultVal = 0.0,
        .note = "process M/S instead of L/R (energy-preserving); mono => no-op",
    });

    s.push_back(ParamDescriptor{
        .id = "linkPct",
        .type = ParamType::Float,
        .min = 0.0,
        .max = 100.0,
        .unit = "%",
        .defaultVal = 100.0,
        .note = "detection blend: 100 = fully linked common gain map (single "
                "envelope), 0 = per-channel independent",
    });

    s.push_back(ParamDescriptor{
        .id = "focus",
        .type = ParamType::Float,
        .min = -1.0,
        .max = 1.0,
        .defaultVal = 0.0,
        .note = "per-channel depth bias toward L/M (+) or R/S (-); 0 = balanced",
    });

    s.push_back(ParamDescriptor{
        .id = "tiltDetail",
        .type = ParamType::Float,
        .min = -1.0,
        .max = 1.0,
        .defaultVal = 0.0,
        .note = "frequency tilt of the per-bin depth multiplier around 1 kHz",
    });

    s.push_back(ParamDescriptor{
        .id = "tiltAttack",
        .type = ParamType::Float,
        .min = -1.0,
        .max = 1.0,
        .defaultVal = 0.0,
        .note = "frequency tilt of the per-bin attack time around 1 kHz",
    });

    s.push_back(ParamDescriptor{
        .id = "tiltRelease",
        .type = ParamType::Float,
        .min = -1.0,
        .max = 1.0,
        .defaultVal = 0.0,
        .note = "frequency tilt of the per-bin release time around 1 kHz",
    });

    // --- Weight curve (v1 plumbing only — DSP ignores these fields for now,
    // see SpectralNode::Params / dsp/SpectralWeight.h). Ids are MECHANICAL:
    // "wband<i><Suffix>", i = 0..kMaxWeightBands-1 (0-based, unlike the EQ's
    // 1-based band ids) — matches wbandID() in
    // plugins/spectral/PluginProcessor.cpp exactly. ---
    const auto& wbandFreqs = defaultWeightBandFreqsHz();
    for (int i = 0; i < spectralweight::kMaxWeightBands; ++i) {
        const std::string idx = std::to_string(i);

        s.push_back(ParamDescriptor{
            .id = "wband" + idx + "On",
            .type = ParamType::Bool,
            .defaultVal = 0.0,
        });

        s.push_back(ParamDescriptor{
            .id = "wband" + idx + "Type",
            .type = ParamType::Choice,
            .defaultVal = 0.0,  // index of "bell"
            .choices = weightBandTypeChoices(),
        });

        s.push_back(ParamDescriptor{
            .id = "wband" + idx + "Freq",
            .type = ParamType::Float,
            .min = 20.0,
            .max = 20000.0,
            .unit = "Hz",
            .defaultVal = wbandFreqs[static_cast<std::size_t>(i)],
            .scale = Scale::Log,
            .skewValue = 1000.0,  // JUCE setSkewForCentre(1000.0)
        });

        s.push_back(ParamDescriptor{
            .id = "wband" + idx + "Gain",
            .type = ParamType::Float,
            .min = -24.0,
            .max = 24.0,
            .unit = "dB",
            .defaultVal = 0.0,
        });

        s.push_back(ParamDescriptor{
            .id = "wband" + idx + "Q",
            .type = ParamType::Float,
            .min = 0.3,
            .max = 12.0,
            .defaultVal = 0.7,
            .scale = Scale::Skewed,
            .skewValue = 0.5,  // JUCE raw skew factor (not setSkewForCentre)
        });
    }

    s.push_back(ParamDescriptor{
        .id = "rangeLo",
        .type = ParamType::Float,
        .min = 20.0,
        .max = 20000.0,
        .unit = "Hz",
        .defaultVal = 20.0,
        .scale = Scale::Log,
        .skewValue = 1000.0,
        .note = "weight-curve bands below rangeLo are gated toward zero "
                "influence (dsp/SpectralWeight.h gateDb()).",
    });

    s.push_back(ParamDescriptor{
        .id = "rangeHi",
        .type = ParamType::Float,
        .min = 20.0,
        .max = 20000.0,
        .unit = "Hz",
        .defaultVal = 20000.0,
        .scale = Scale::Log,
        .skewValue = 1000.0,
        .note = "weight-curve bands above rangeHi are gated toward zero "
                "influence (dsp/SpectralWeight.h gateDb()).",
    });

    s.push_back(ParamDescriptor{
        .id = "detectionMode",
        .type = ParamType::Choice,
        .defaultVal = 0.0,
        .note = "resonance detector: Soft = legacy detector, Hard = WOLA detector",
        .choices = {"soft", "hard"},
    });

    s.push_back(ParamDescriptor{
        .id = "externalSidechain",
        .type = ParamType::Bool,
        .defaultVal = 0.0,
        .note = "use the external key for detection; missing key creates zero new demand",
    });
    s.push_back(ParamDescriptor{
        .id = "sidechainListen",
        .type = ParamType::Bool,
        .defaultVal = 0.0,
        .note = "listen to the raw latency-aligned external key independently of detection",
    });

    return s;
}

inline std::string spectralSchemaJson() {
    return paramschema::toJson(spectralSchema());
}

}  // namespace spectralschema

#pragma once

#include <array>
#include <sstream>
#include <string>
#include <vector>

#include "dsp/MultibandEq.h"
#include "dsp/ParamSchema.h"

/**
    Machine-readable parameter schema for the 16-band parametric EQ, mirroring
    the ACTUAL APVTS layout in src/plugins/eq/PluginProcessor.cpp
    ::createParameterLayout() exactly — ids, ranges, defaults, skews, units,
    choice order.

    The 16-band expansion is MECHANICAL: every band uses the identical
    6-parameter schema below, with its 1-based index substituted into the id
    (pattern "band<N><Suffix>", e.g. "band3Freq"). This file represents that
    ONE per-band schema plus the band count — not 16 duplicated entries.

    Ground truth for ids/ranges/defaults/skews: PluginProcessor.cpp. Ground
    truth for the semantics behind each `note`: SvfBandNode.h / BandResponse.h
    doc comments. Cross-checked in test/param_schema_test.cpp and, at
    runtime, against the live apvts in the plugin constructor (see
    PluginProcessor.cpp, JUCE_DEBUG only).
*/
namespace eqschema {

/** Per-band default center frequencies (Pro-Q-style log spread across the 16
    bands). MUST match kDefaultFreqs in plugins/eq/PluginProcessor.cpp —
    duplicated here because that array is local to the .cpp; the drift-guard
    test cross-checks this copy against an independently transcribed literal,
    and the runtime apvts check in the plugin constructor checks it against
    the actual live parameter defaults. */
inline const std::array<double, MultibandEq::kNumBands>& defaultBandFreqsHz() {
    static const std::array<double, MultibandEq::kNumBands> f{
        30.0, 50.0, 80.0, 120.0, 180.0, 260.0, 400.0, 600.0,
        900.0, 1400.0, 2000.0, 3000.0, 4500.0, 7000.0, 10000.0, 15000.0};
    return f;
}

/** Choice order MUST match svfcoeffs::Type (Bell=0, LowShelf, HighShelf,
    Tilt, Notch, Bandpass, Lowpass, Highpass) — the same ordinal contract
    SvfBandNode and the plugin's AudioParameterChoice both key off. */
inline std::vector<std::string> bandTypeChoices() {
    return {"Bell", "Low Shelf", "High Shelf", "Tilt",
            "Notch", "Bandpass", "Lowpass", "Highpass"};
}

/** Choice order MUST match kSlopeValues in PluginProcessor.cpp. Labels are
    the dB/oct values themselves. */
inline std::vector<std::string> slopeChoices() {
    return {"6", "12", "18", "24", "30", "36", "48", "72", "96"};
}

/** The single output-trim param (not per-band). */
inline paramschema::ParamDescriptor outputGainSchema() {
    using namespace paramschema;
    return ParamDescriptor{
        .id = "gain",
        .type = ParamType::Float,
        .min = -24.0,
        .max = 12.0,
        .unit = "dB",
        .defaultVal = 0.0,
    };
}

/** The per-band schema. Ids contain the literal placeholder "<N>" — expand
    with bandParamId() for a concrete 1-based band index. */
inline std::vector<paramschema::ParamDescriptor> bandSchema() {
    using namespace paramschema;
    std::vector<ParamDescriptor> s;

    s.push_back(ParamDescriptor{
        .id = "band<N>Enabled",
        .type = ParamType::Bool,
        .defaultVal = 0.0,
        .note = "toggling crossfades over a 10 ms linear ramp, not an "
                "instant switch — the band keeps running (and costing CPU) "
                "until the ramp completes.",
    });

    s.push_back(ParamDescriptor{
        .id = "band<N>Type",
        .type = ParamType::Choice,
        .defaultVal = 0.0,  // index of "Bell"
        .note = "Tilt uses a TOTAL-tilt convention: gain splits to "
                "+/-gainDb/2 on the two sides of the pivot frequency, with "
                "0 dB AT the pivot — not a single-sided shelf-style gain.",
        .choices = bandTypeChoices(),
    });

    s.push_back(ParamDescriptor{
        .id = "band<N>Freq",
        .type = ParamType::Float,
        .min = 20.0,
        .max = 20000.0,
        .unit = "Hz",
        .defaultVal = 1000.0,  // representative only; see defaultsByIndex
        .scale = Scale::Log,
        .skewValue = 1000.0,  // JUCE setSkewForCentre(1000.0)
        .note = "default varies per band (Pro-Q-style log spread) — see "
                "defaultsByIndex for the real per-band defaults, band 1..16 "
                "in order.",
        .defaultsByIndex = std::vector<double>(defaultBandFreqsHz().begin(),
                                                defaultBandFreqsHz().end()),
    });

    s.push_back(ParamDescriptor{
        .id = "band<N>Q",
        .type = ParamType::Float,
        .min = 0.1,
        .max = 18.0,
        .defaultVal = 0.7071,
        .scale = Scale::Skewed,
        .skewValue = 0.5,  // JUCE raw skew factor (not setSkewForCentre)
        .note = "on Lowpass/Highpass types, Q shapes ONLY the corner "
                "resonance of the LAST section in the slope cascade — it "
                "does not change slope order or the earlier sections.",
    });

    s.push_back(ParamDescriptor{
        .id = "band<N>Gain",
        .type = ParamType::Float,
        .min = -24.0,
        .max = 24.0,
        .unit = "dB",
        .defaultVal = 0.0,
        .note = "used by Bell/Low Shelf/High Shelf/Tilt only — has no "
                "effect for Notch/Bandpass/Lowpass/Highpass.",
    });

    s.push_back(ParamDescriptor{
        .id = "band<N>Slope",
        .type = ParamType::Choice,
        .defaultVal = 1.0,  // index of "12" (12 dB/oct)
        .note = "dB/oct; used by Lowpass/Highpass only, ignored for all "
                "other band types.",
        .choices = slopeChoices(),
    });

    // --- Dynamic EQ (level-dependent band gain) ---
    s.push_back(ParamDescriptor{
        .id = "band<N>DynOn",
        .type = ParamType::Bool,
        .defaultVal = 0.0,
        .note = "dynamics apply to the gain-bearing matched types only "
                "(Bell/Low Shelf/High Shelf/Tilt); ignored for "
                "Notch/Bandpass/Lowpass/Highpass.",
    });

    s.push_back(ParamDescriptor{
        .id = "band<N>DynRange",
        .type = ParamType::Float,
        .min = -30.0,
        .max = 30.0,
        .unit = "dB",
        .defaultVal = 0.0,
        .note = "SIGNED excursion: the sign folds direction (positive = the "
                "band gain rises toward gain+range as the detector engages, "
                "negative = falls) and the magnitude folds amount, in one "
                "control. Added to the static band gain, scaled 0..1 by the "
                "detector demand.",
    });

    s.push_back(ParamDescriptor{
        .id = "band<N>DynThresh",
        .type = ParamType::Float,
        .min = -60.0,
        .max = 0.0,
        .unit = "dB",
        .defaultVal = -24.0,
        .note = "refers to the BAND-LIMITED detector level (input through a "
                "matched bandpass at the band's own f0/Q), not the full-band "
                "level — a de-esser-style sidechain, so the same dB reads "
                "differently than a broadband threshold.",
    });

    s.push_back(ParamDescriptor{
        .id = "band<N>DynAttack",
        .type = ParamType::Float,
        .min = 0.05,
        .max = 100.0,
        .unit = "ms",
        .defaultVal = 2.0,
        .scale = Scale::Skewed,
        .skewValue = 0.4,
    });

    s.push_back(ParamDescriptor{
        .id = "band<N>DynRelease",
        .type = ParamType::Float,
        .min = 5.0,
        .max = 1000.0,
        .unit = "ms",
        .defaultVal = 80.0,
        .scale = Scale::Skewed,
        .skewValue = 0.4,
    });

    return s;
}

/** Expands an id template's "<N>" placeholder to a concrete 1-based band
    index, e.g. bandParamId("band<N>Freq", 3) -> "band3Freq". */
inline std::string bandParamId(const std::string& idTemplate, int bandIndexOneBased) {
    std::string out = idTemplate;
    const std::string placeholder = "<N>";
    const auto pos = out.find(placeholder);
    if (pos != std::string::npos)
        out.replace(pos, placeholder.size(), std::to_string(bandIndexOneBased));
    return out;
}

/** Full EQ schema as JSON: the output-gain param, plus the per-band schema
    (ids left as the literal "<N>" template) and the band count. Mechanical
    expansion into 16 concrete bands is the caller's job. */
inline std::string eqSchemaJson() {
    std::ostringstream os;
    os << "{\n";
    os << "  \"gain\": " << paramschema::toJson(outputGainSchema()) << ",\n";
    os << "  \"bandCount\": " << MultibandEq::kNumBands << ",\n";
    os << "  \"bandIdPattern\": \"band<N><Suffix>, N = 1..bandCount (e.g. "
          "band3Freq for band index 3)\",\n";
    os << "  \"band\": " << paramschema::toJson(bandSchema()) << "\n";
    os << "}";
    return os.str();
}

}  // namespace eqschema

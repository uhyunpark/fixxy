// Drift guard for the machine-readable parameter schema (dsp/ParamSchema.h,
// dsp/CompressorSchema.h, dsp/EqSchema.h): checks the schema tables against
// an INDEPENDENTLY transcribed copy of the actual APVTS layouts (ids, ranges,
// defaults, skews, choices — transcribed straight from PluginProcessor.cpp,
// not derived from the schema itself) and against the DSP nodes' own Params
// defaults where the two are supposed to agree.
//
// This is the "lighter approach" from the task: instantiating a real APVTS
// here would drag JUCE into a plain-C++ console target. The complementary
// half of the drift guard is a JUCE_DEBUG-only runtime check inside each
// plugin's constructor (see PluginProcessor.cpp) that walks the SAME schema
// tables against the LIVE apvts objects — that one catches drift against
// reality, this one catches drift against intent and prints the artifact.
//
// Plain C++ console target — no JUCE (CMakeLists.txt: separate add_executable).

#include <array>
#include <cmath>
#include <cstdio>
#include <string>

#include "dsp/CompressorNode.h"
#include "dsp/CompressorSchema.h"
#include "dsp/EqSchema.h"
#include "dsp/SpectralNode.h"
#include "dsp/SpectralSchema.h"
#include "dsp/SvfBandNode.h"

namespace {

bool gAllPass = true;

void check(const char* tag, const char* name, bool cond, const char* detail) {
    if (!cond) gAllPass = false;
    std::printf("[%s] %s: %s -- %s\n", tag, cond ? "PASS" : "FAIL", name, detail);
}

bool nearlyEqual(double a, double b, double eps = 1e-9) {
    return std::fabs(a - b) < eps;
}

const paramschema::ParamDescriptor* find(const std::vector<paramschema::ParamDescriptor>& v,
                                          const std::string& id) {
    for (const auto& d : v)
        if (d.id == id) return &d;
    return nullptr;
}

// ============================================================
// Compressor: schema vs. an independently transcribed copy of
// PluginProcessor.cpp's createParameterLayout(), and vs. CompressorNode's
// own Params defaults (they must agree — the plugin passes them straight
// through with the same numeric defaults).
// ============================================================
void verifyCompressorSchema() {
    const auto schema = compressorschema::compressorSchema();

    struct Expected {
        const char* id;
        double min, max, defaultVal;
        bool isBool;
    };
    // Transcribed independently from src/plugins/comp/PluginProcessor.cpp.
    const Expected expected[] = {
        {"threshold", -60.0, 0.0, -20.0, false},
        {"ratio", 1.0, 20.0, 4.0, false},
        {"knee", 0.0, 24.0, 6.0, false},
        {"attack", 0.0, 200.0, 10.0, false},
        {"release", 1.0, 2000.0, 100.0, false},
        {"makeup", -24.0, 24.0, 0.0, false},
        {"mix", 0.0, 100.0, 100.0, false},
        {"range", 0.0, 96.0, 96.0, false},
        {"hold", 0.0, 500.0, 0.0, false},
        {"autoMakeup", 0.0, 1.0, 0.0, true},
        {"progRelease", 0.0, 1.0, 0.0, true},
        {"scHpf", 0.0, 1.0, 0.0, true},
        {"rmsMode", 0.0, 1.0, 0.0, true},
    };

    check("comp", "schema has exactly 13 params", schema.size() == 13,
          ("count=" + std::to_string(schema.size())).c_str());

    for (const auto& e : expected) {
        const auto* d = find(schema, e.id);
        std::string tag = std::string("comp.") + e.id;
        if (d == nullptr) {
            check("comp", tag.c_str(), false, "missing from schema");
            continue;
        }
        if (!e.isBool) {
            // min/max are only meaningful (and only serialized) for non-bool
            // params; bools are on/off by construction.
            check("comp", (tag + ".min").c_str(), nearlyEqual(d->min, e.min), "vs APVTS layout");
            check("comp", (tag + ".max").c_str(), nearlyEqual(d->max, e.max), "vs APVTS layout");
        }
        check("comp", (tag + ".default").c_str(), nearlyEqual(d->defaultVal, e.defaultVal),
              "vs APVTS layout");
        check("comp", (tag + ".type").c_str(),
              e.isBool ? (d->type == paramschema::ParamType::Bool)
                       : (d->type == paramschema::ParamType::Float),
              "vs APVTS layout");
    }

    // Cross-check against CompressorNode::Params defaults directly — the
    // schema's numeric defaults MUST equal what a freshly-constructed node
    // (and the plugin, which forwards APVTS defaults 1:1) actually starts at.
    const CompressorNode::Params defaults{};
    check("comp", "node threshold default matches schema",
          nearlyEqual(find(schema, "threshold")->defaultVal, defaults.thresholdDb), "");
    check("comp", "node ratio default matches schema",
          nearlyEqual(find(schema, "ratio")->defaultVal, defaults.ratio), "");
    check("comp", "node knee default matches schema",
          nearlyEqual(find(schema, "knee")->defaultVal, defaults.kneeDb), "");
    check("comp", "node attack default matches schema",
          nearlyEqual(find(schema, "attack")->defaultVal, defaults.attackMs), "");
    check("comp", "node release default matches schema",
          nearlyEqual(find(schema, "release")->defaultVal, defaults.releaseMs), "");
    check("comp", "node makeup default matches schema",
          nearlyEqual(find(schema, "makeup")->defaultVal, defaults.makeupDb), "");
    check("comp", "node mix default matches schema (0..1 vs 0..100%)",
          nearlyEqual(find(schema, "mix")->defaultVal / 100.0, defaults.mix), "");
    check("comp", "node range default matches schema",
          nearlyEqual(find(schema, "range")->defaultVal, defaults.rangeDb), "");
    check("comp", "node hold default matches schema",
          nearlyEqual(find(schema, "hold")->defaultVal, defaults.holdMs), "");
    check("comp", "range-off sentinel matches CompressorNode::kRangeOffDb",
          nearlyEqual(find(schema, "range")->max, CompressorNode::kRangeOffDb), "");
}

// ============================================================
// EQ: schema vs. an independently transcribed copy of PluginProcessor.cpp's
// createParameterLayout(), and vs. SvfBandNode::Type ordinal contract.
// ============================================================
void verifyEqSchema() {
    const auto band = eqschema::bandSchema();
    const auto gain = eqschema::outputGainSchema();

    check("eq", "output gain range", nearlyEqual(gain.min, -24.0) && nearlyEqual(gain.max, 12.0),
          "vs APVTS layout");
    check("eq", "output gain default", nearlyEqual(gain.defaultVal, 0.0), "vs APVTS layout");

    check("eq", "band schema has exactly 11 params", band.size() == 11,
          ("count=" + std::to_string(band.size())).c_str());
    check("eq", "bandCount == MultibandEq::kNumBands", MultibandEq::kNumBands == 16,
          ("kNumBands=" + std::to_string(MultibandEq::kNumBands)).c_str());

    const auto* enabled = find(band, "band<N>Enabled");
    check("eq", "band.Enabled is bool, default off",
          enabled && enabled->type == paramschema::ParamType::Bool &&
              nearlyEqual(enabled->defaultVal, 0.0),
          "vs APVTS layout");

    const auto* type = find(band, "band<N>Type");
    check("eq", "band.Type choice count == 8",
          type && type->choices.size() == 8, "vs svfcoeffs::Type");
    if (type != nullptr) {
        // Choice order must match svfcoeffs::Type ordinals exactly — this is
        // the contract both SvfBandNode and the plugin's AudioParameterChoice
        // rely on.
        check("eq", "band.Type[0] == Bell (svfcoeffs::Type::Bell == 0)",
              static_cast<int>(svfcoeffs::Type::Bell) == 0 && type->choices[0] == "Bell", "");
        check("eq", "band.Type[6] == Lowpass (svfcoeffs::Type::Lowpass == 6)",
              static_cast<int>(svfcoeffs::Type::Lowpass) == 6 && type->choices[6] == "Lowpass",
              "");
        check("eq", "band.Type[7] == Highpass (svfcoeffs::Type::Highpass == 7)",
              static_cast<int>(svfcoeffs::Type::Highpass) == 7 && type->choices[7] == "Highpass",
              "");
    }

    const auto* freq = find(band, "band<N>Freq");
    const std::array<double, 16> expectedFreqs{30, 50, 80, 120, 180, 260, 400, 600,
                                                900, 1400, 2000, 3000, 4500, 7000, 10000, 15000};
    check("eq", "band.Freq range == [20, 20000] Hz",
          freq && nearlyEqual(freq->min, 20.0) && nearlyEqual(freq->max, 20000.0),
          "vs APVTS layout");
    check("eq", "band.Freq defaultsByIndex has 16 entries",
          freq && freq->defaultsByIndex.size() == 16, "");
    if (freq != nullptr && freq->defaultsByIndex.size() == 16) {
        bool allMatch = true;
        for (int i = 0; i < 16; ++i)
            allMatch &= nearlyEqual(freq->defaultsByIndex[static_cast<std::size_t>(i)],
                                     expectedFreqs[static_cast<std::size_t>(i)]);
        check("eq", "band.Freq defaultsByIndex matches Pro-Q-spread default array", allMatch,
              "vs kDefaultFreqs in PluginProcessor.cpp (independently transcribed)");
    }

    const auto* q = find(band, "band<N>Q");
    check("eq", "band.Q range/default", q && nearlyEqual(q->min, 0.1) && nearlyEqual(q->max, 18.0) &&
                                             nearlyEqual(q->defaultVal, 0.7071),
          "vs APVTS layout");

    const auto* gainBand = find(band, "band<N>Gain");
    check("eq", "band.Gain range/default",
          gainBand && nearlyEqual(gainBand->min, -24.0) && nearlyEqual(gainBand->max, 24.0) &&
              nearlyEqual(gainBand->defaultVal, 0.0),
          "vs APVTS layout");

    const auto* slope = find(band, "band<N>Slope");
    check("eq", "band.Slope choice count == 9 (6..96 dB/oct)",
          slope && slope->choices.size() == 9, "vs kSlopeChoices");
    check("eq", "band.Slope default index == 1 (12 dB/oct)",
          slope && nearlyEqual(slope->defaultVal, 1.0) && slope->choices[1] == "12",
          "vs kDefaultSlopeIndex");

    // Cross-check against SvfBandNode::BandParams defaults for the fields
    // that are meant to agree (type ordinal 0 = Bell, Q default).
    const SvfBandNode::BandParams nodeDefaults{};
    check("eq", "node type default is Bell", nodeDefaults.type == SvfBandNode::Type::Bell, "");
    check("eq", "node q default matches schema",
          nearlyEqual(nodeDefaults.q, q->defaultVal), "");

    // --- Dynamic-EQ params (transcribed from PluginProcessor.cpp) ---
    const auto* dynOn = find(band, "band<N>DynOn");
    check("eq", "band.DynOn is bool, default off",
          dynOn && dynOn->type == paramschema::ParamType::Bool &&
              nearlyEqual(dynOn->defaultVal, 0.0),
          "vs APVTS layout");

    const auto* dynRange = find(band, "band<N>DynRange");
    check("eq", "band.DynRange range/default (signed +/-30 dB, 0)",
          dynRange && nearlyEqual(dynRange->min, -30.0) && nearlyEqual(dynRange->max, 30.0) &&
              nearlyEqual(dynRange->defaultVal, 0.0),
          "vs APVTS layout");

    const auto* dynThresh = find(band, "band<N>DynThresh");
    check("eq", "band.DynThresh range/default (-60..0 dB, -24)",
          dynThresh && nearlyEqual(dynThresh->min, -60.0) && nearlyEqual(dynThresh->max, 0.0) &&
              nearlyEqual(dynThresh->defaultVal, -24.0),
          "vs APVTS layout");

    const auto* dynAtt = find(band, "band<N>DynAttack");
    check("eq", "band.DynAttack range/default (0.05..100 ms, 2)",
          dynAtt && nearlyEqual(dynAtt->min, 0.05) && nearlyEqual(dynAtt->max, 100.0) &&
              nearlyEqual(dynAtt->defaultVal, 2.0),
          "vs APVTS layout");

    const auto* dynRel = find(band, "band<N>DynRelease");
    check("eq", "band.DynRelease range/default (5..1000 ms, 80)",
          dynRel && nearlyEqual(dynRel->min, 5.0) && nearlyEqual(dynRel->max, 1000.0) &&
              nearlyEqual(dynRel->defaultVal, 80.0),
          "vs APVTS layout");

    // Node BandParams defaults must agree with the schema defaults.
    const SvfBandNode::BandParams dynDefaults{};
    check("eq", "node dyn defaults match schema",
          dynDefaults.dynEnabled == false && nearlyEqual(dynDefaults.dynRangeDb, 0.0) &&
              nearlyEqual(dynDefaults.dynThresholdDb, -24.0) &&
              nearlyEqual(dynDefaults.dynAttackMs, 2.0) &&
              nearlyEqual(dynDefaults.dynReleaseMs, 80.0),
          "vs SvfBandNode::BandParams");

    // bandParamId() placeholder expansion sanity.
    check("eq", "bandParamId expands <N> correctly",
          eqschema::bandParamId("band<N>Freq", 3) == "band3Freq", "");
}

// ============================================================
// Spectral: schema vs. an independently transcribed copy of
// PluginProcessor.cpp's createParameterLayout(), and vs. SpectralNode's own
// Params defaults (they must agree — the plugin passes them straight
// through with the same numeric defaults).
// ============================================================
void verifySpectralSchema() {
    const auto schema = spectralschema::spectralSchema();

    struct Expected {
        const char* id;
        double min, max, defaultVal;
    };
    // Transcribed independently from src/plugins/spectral/PluginProcessor.cpp.
    const Expected expected[] = {
        {"depth", 0.0, 10.0, 5.0},
        {"detail", 0.0, 10.0, 5.0},
        {"attack", 1.0, 250.0, 27.0},
        {"release", 5.0, 1000.0, 50.0},
        {"mix", 0.0, 100.0, 100.0},
        {"output", -24.0, 12.0, 0.0},
    };

    check("spectral", "schema has exactly 8 params", schema.size() == 8,
          ("count=" + std::to_string(schema.size())).c_str());

    for (const auto& e : expected) {
        const auto* d = find(schema, e.id);
        std::string tag = std::string("spectral.") + e.id;
        if (d == nullptr) {
            check("spectral", tag.c_str(), false, "missing from schema");
            continue;
        }
        check("spectral", (tag + ".min").c_str(), nearlyEqual(d->min, e.min), "vs APVTS layout");
        check("spectral", (tag + ".max").c_str(), nearlyEqual(d->max, e.max), "vs APVTS layout");
        check("spectral", (tag + ".default").c_str(), nearlyEqual(d->defaultVal, e.defaultVal),
              "vs APVTS layout");
        check("spectral", (tag + ".type").c_str(), d->type == paramschema::ParamType::Float,
              "vs APVTS layout");
    }

    // Bool params (bypass, delta) added for the Stage-5 editor. Transcribed
    // independently from PluginProcessor.cpp: both AudioParameterBool default false.
    for (const char* id : {"bypass", "delta"}) {
        const auto* d = find(schema, id);
        std::string tag = std::string("spectral.") + id;
        if (d == nullptr) {
            check("spectral", tag.c_str(), false, "missing from schema");
            continue;
        }
        check("spectral", (tag + ".type").c_str(), d->type == paramschema::ParamType::Bool,
              "vs APVTS layout (AudioParameterBool)");
        check("spectral", (tag + ".default").c_str(), nearlyEqual(d->defaultVal, 0.0),
              "vs APVTS layout (default false)");
    }

    // Cross-check against SpectralNode::Params defaults directly.
    const SpectralNode::Params defaults{};
    check("spectral", "node depth default matches schema",
          nearlyEqual(find(schema, "depth")->defaultVal, defaults.depth), "");
    check("spectral", "node detail default matches schema",
          nearlyEqual(find(schema, "detail")->defaultVal, defaults.detail), "");
    check("spectral", "node attack default matches schema",
          nearlyEqual(find(schema, "attack")->defaultVal, defaults.attackMs), "");
    check("spectral", "node release default matches schema",
          nearlyEqual(find(schema, "release")->defaultVal, defaults.releaseMs), "");
}

}  // namespace

int main() {
    verifyCompressorSchema();
    verifyEqSchema();
    verifySpectralSchema();

    std::printf("\n=== compressor schema JSON ===\n%s\n",
                compressorschema::compressorSchemaJson().c_str());
    std::printf("\n=== eq schema JSON ===\n%s\n", eqschema::eqSchemaJson().c_str());
    std::printf("\n=== spectral schema JSON ===\n%s\n",
                spectralschema::spectralSchemaJson().c_str());

    std::printf("\n%s\n", gAllPass ? "ALL PARAM SCHEMA CHECKS PASSED" : "PARAM SCHEMA CHECKS FAILED");
    return gAllPass ? 0 : 1;
}

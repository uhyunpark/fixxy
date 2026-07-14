#pragma once

#include <sstream>
#include <string>
#include <vector>

/**
    Machine-readable parameter schema — the foundation of the future LLM
    control layer: the LLM emits parameter JSON, a node's Params struct
    consumes it. JUCE-free, header-only, no external JSON dependency.

    DESIGN PRINCIPLE: the LLM already knows standard audio semantics. This
    schema does NOT explain what "threshold" or "Q" means. It carries only
    what the LLM cannot infer by itself:
      - the exact parameter id (the plugin's real APVTS id, not the C++
        member name)
      - type, range, unit, default, and a scale hint (log/skewed) where the
        control isn't a plain linear knob
      - choice labels, where relevant
      - a one-line `note`, ONLY where our implementation deviates from
        common convention or is otherwise ambiguous. Most params have none.

    Per-effect schema tables (compressorSchema(), eqBandSchema(), ...) live
    next to their node headers (see dsp/CompressorSchema.h, dsp/EqSchema.h),
    not here — this file is only the descriptor type + serializer.
*/
namespace paramschema {

enum class ParamType { Float, Bool, Choice };

// How knob/automation position maps to value, mirroring the JUCE
// NormalisableRange shape actually used in the APVTS layout:
//   Linear  - juce::NormalisableRange(min, max, step)
//   Log     - a range that behaves like a log/frequency skew
//   Skewed  - NormalisableRange::setSkewForCentre(x), or a raw skew factor;
//             `skewValue` carries whichever the plugin actually uses (the
//             descriptor's `note` says which, when it isn't obvious)
enum class Scale { Linear, Log, Skewed };

struct ParamDescriptor {
    std::string id;
    ParamType type = ParamType::Float;
    double min = 0.0;
    double max = 0.0;
    std::string unit;               // "" = unitless
    double defaultVal = 0.0;
    Scale scale = Scale::Linear;
    double skewValue = 0.0;         // meaningful only when scale != Linear
    std::string note;               // "" = nothing non-obvious to say
    std::vector<std::string> choices;      // ParamType::Choice only
    // Optional: set when the default genuinely varies per instance (e.g. a
    // per-band center frequency) rather than being one fixed value. Index 0
    // = instance 1. Empty everywhere else; `defaultVal` then still holds a
    // single representative value.
    std::vector<double> defaultsByIndex;
};

// ---- minimal hand-rolled JSON writer (no dependency) ----

inline std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            default: out += c;
        }
    }
    return out;
}

// Integers print without a trailing ".0"; everything else gets enough
// digits to round-trip typical audio ranges.
inline std::string numJson(double v) {
    std::ostringstream os;
    if (v == static_cast<double>(static_cast<long long>(v))) {
        os << static_cast<long long>(v);
    } else {
        os.precision(10);
        os << v;
    }
    return os.str();
}

inline const char* typeName(ParamType t) {
    switch (t) {
        case ParamType::Float: return "float";
        case ParamType::Bool: return "bool";
        case ParamType::Choice: return "choice";
    }
    return "float";
}

inline const char* scaleName(Scale s) {
    switch (s) {
        case Scale::Linear: return "linear";
        case Scale::Log: return "log";
        case Scale::Skewed: return "skewed";
    }
    return "linear";
}

/** Serializes one descriptor as a JSON object. `indent` = spaces before the
    opening brace; fields are indented two further. */
inline std::string toJson(const ParamDescriptor& p, int indent = 0) {
    const std::string pad(static_cast<std::size_t>(indent), ' ');
    const std::string pad2(static_cast<std::size_t>(indent + 2), ' ');
    std::ostringstream os;
    os << pad << "{\n";
    os << pad2 << "\"id\": \"" << jsonEscape(p.id) << "\",\n";
    os << pad2 << "\"type\": \"" << typeName(p.type) << "\",\n";
    if (p.type == ParamType::Float) {
        // Bool is on/off by construction; Choice's domain is `choices`
        // (index 0..choices.size()-1) — min/max would be redundant noise.
        os << pad2 << "\"min\": " << numJson(p.min) << ",\n";
        os << pad2 << "\"max\": " << numJson(p.max) << ",\n";
    }
    if (!p.unit.empty())
        os << pad2 << "\"unit\": \"" << jsonEscape(p.unit) << "\",\n";
    os << pad2 << "\"default\": " << numJson(p.defaultVal) << ",\n";
    if (p.scale != Scale::Linear) {
        os << pad2 << "\"scale\": \"" << scaleName(p.scale) << "\",\n";
        os << pad2 << "\"skew\": " << numJson(p.skewValue) << ",\n";
    }
    if (!p.defaultsByIndex.empty()) {
        os << pad2 << "\"defaultsByIndex\": [";
        for (std::size_t i = 0; i < p.defaultsByIndex.size(); ++i) {
            os << numJson(p.defaultsByIndex[i]);
            if (i + 1 < p.defaultsByIndex.size()) os << ", ";
        }
        os << "],\n";
    }
    if (p.type == ParamType::Choice) {
        os << pad2 << "\"choices\": [";
        for (std::size_t i = 0; i < p.choices.size(); ++i) {
            os << "\"" << jsonEscape(p.choices[i]) << "\"";
            if (i + 1 < p.choices.size()) os << ", ";
        }
        os << "],\n";
    }
    if (!p.note.empty())
        os << pad2 << "\"note\": \"" << jsonEscape(p.note) << "\",\n";

    std::string body = os.str();
    // Drop the trailing ",\n" left by the last emitted field.
    if (body.size() >= 2 && body[body.size() - 2] == ',' && body.back() == '\n')
        body.erase(body.size() - 2, 1);
    return body + pad + "}";
}

/** Serializes a list of descriptors as a JSON array. */
inline std::string toJson(const std::vector<ParamDescriptor>& params, int indent = 0) {
    const std::string pad(static_cast<std::size_t>(indent), ' ');
    std::ostringstream os;
    os << pad << "[\n";
    for (std::size_t i = 0; i < params.size(); ++i) {
        os << toJson(params[i], indent + 2);
        if (i + 1 < params.size()) os << ",";
        os << "\n";
    }
    os << pad << "]";
    return os.str();
}

}  // namespace paramschema

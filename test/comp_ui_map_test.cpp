// Offline verification of the Fixxy Comp editor's UI-side math seams:
//
//   1. Meter scale mappers (CompMeterMap.h) — the ANALYZER LESSON applied to
//      meters: gainReductionDb is a POSITIVE magnitude on its own 0..30 dB
//      top-down scale; IN/OUT peaks are NEGATIVE dBFS on a -60..0 bottom-up
//      scale. These asserts pin the seam: GR 0 -> empty, GR 30 -> full,
//      IN -60 -> bottom (0), IN 0 -> top (1). Feeding one quantity through the
//      other's mapper is exactly the class of bug that produced the blank EQ
//      analyzer (docs/architecture.md 함정, 이음새 버그) — a meter pinned always-empty /
//      always-full while every component test passes.
//
//   2. CompCurveMap round-trips (dbToX/xToDb, dbToY/yToDb) — the transfer
//      curve's square linear -60..0 x -60..0 plane.
//
//   3. CompressorNode::staticCurve() refactor equivalence — the pure static
//      the UI evaluates must match hand-computed closed-form values (constants
//      from compressor_node_test.cpp's T=-20/R=4/W=6 setup) AND be identical
//      to the instance path staticCurveDb() that the audio kernel's
//      gainComputer() delegates to (single-source rule).
//
// Plain C++ console target, no JUCE — CompMeterMap.h / CompCurveMap.h are
// deliberately JUCE-free so this test stays a bare add_executable.

#include <cmath>
#include <cstdio>

#include "dsp/CompressorNode.h"
#include "plugins/comp/ui/CompCurveMap.h"
#include "plugins/comp/ui/CompMeterMap.h"

namespace {

bool gAllPass = true;

void check(const char* tag, const char* name, bool cond, const char* detail) {
    if (!cond) gAllPass = false;
    std::printf("[%s] %s: %s -- %s\n", tag, cond ? "PASS" : "FAIL", name, detail);
}

void checkNear(const char* tag, const char* name, double got, double want, double tol) {
    char d[128];
    std::snprintf(d, sizeof d, "got %.9f, want %.9f (tol %g)", got, want, tol);
    check(tag, name, std::fabs(got - want) < tol, d);
}

// ============================================================
// 1: meter scale mappers — the seam asserts
// ============================================================
void verifyMeterMaps() {
    using compui::GrMeterMap;
    using compui::LevelMeterMap;

    // GR: positive magnitude, 0..30, top-down fill.
    checkNear("meter", "GR 0 dB -> bar empty (fraction 0)", GrMeterMap::fraction(0.0f), 0.0, 1e-9);
    checkNear("meter", "GR 30 dB -> bar full (fraction 1)", GrMeterMap::fraction(30.0f), 1.0, 1e-9);
    checkNear("meter", "GR 15 dB -> half", GrMeterMap::fraction(15.0f), 0.5, 1e-9);
    checkNear("meter", "GR clamps above 30", GrMeterMap::fraction(96.0f), 1.0, 1e-9);
    checkNear("meter", "GR clamps below 0", GrMeterMap::fraction(-3.0f), 0.0, 1e-9);

    // IN/OUT: negative dBFS, -60..0, bottom-up fill.
    checkNear("meter", "IN -60 dBFS -> bottom (fraction 0)", LevelMeterMap::fraction(-60.0f), 0.0, 1e-9);
    checkNear("meter", "IN 0 dBFS -> top (fraction 1)", LevelMeterMap::fraction(0.0f), 1.0, 1e-9);
    checkNear("meter", "IN -30 dBFS -> half", LevelMeterMap::fraction(-30.0f), 0.5, 1e-9);
    checkNear("meter", "IN clamps below floor (-100 silent publish)",
              LevelMeterMap::fraction(-100.0f), 0.0, 1e-9);
    checkNear("meter", "IN clamps above 0", LevelMeterMap::fraction(3.0f), 1.0, 1e-9);

    // THE seam assert: a positive GR magnitude fed into the LEVEL mapper pins
    // to full — i.e. the two scales are NOT interchangeable. If someone unifies
    // the mappers, this documents what breaks.
    checkNear("meter", "cross-feed check: +6 dB GR through level map pins to 1 (scales differ)",
              LevelMeterMap::fraction(6.0f), 1.0, 1e-9);
}

// ============================================================
// 2: CompCurveMap round-trips
// ============================================================
void verifyCurveMap() {
    compui::CompCurveMap map;
    map.setBounds(13.0f, 7.0f, 411.0f, 385.0f);  // deliberately awkward bounds

    // Corners: -60 in at left edge, 0 at right; -60 out at bottom, 0 at top.
    checkNear("curvemap", "in -60 dB -> left edge", map.dbToX(-60.0f), 13.0, 1e-4);
    checkNear("curvemap", "in 0 dB -> right edge", map.dbToX(0.0f), 13.0 + 411.0, 1e-4);
    checkNear("curvemap", "out -60 dB -> bottom edge", map.dbToY(-60.0f), 7.0 + 385.0, 1e-4);
    checkNear("curvemap", "out 0 dB -> top edge", map.dbToY(0.0f), 7.0, 1e-4);

    double maxErrX = 0.0, maxErrY = 0.0;
    for (int db = -60; db <= 0; ++db) {
        const auto f = static_cast<float>(db);
        maxErrX = std::fmax(maxErrX, std::fabs(static_cast<double>(map.xToDb(map.dbToX(f))) - db));
        maxErrY = std::fmax(maxErrY, std::fabs(static_cast<double>(map.yToDb(map.dbToY(f))) - db));
    }
    char d[128];
    std::snprintf(d, sizeof d, "max |err| x=%.6f dB, y=%.6f dB over -60..0", maxErrX, maxErrY);
    check("curvemap", "round-trip db->px->db both axes", maxErrX < 1e-3 && maxErrY < 1e-3, d);
}

// ============================================================
// 3: staticCurve refactor equivalence
// ============================================================
void verifyStaticCurve() {
    // Hand-computed closed-form values for the CompressorNodeTest constants
    // T=-20, R=4, W=6 (full knee width; knee spans [-23, -17]):
    //   below knee:  y = x
    //   in knee:     y = x + (1/R - 1) * (x - T + W/2)^2 / (2W)
    //   above knee:  y = T + (x - T)/R
    struct Case { double in, t, r, w, want; };
    const Case cases[] = {
        {-40.0, -20.0, 4.0, 6.0, -40.0},                        // below knee: unity
        {-23.0, -20.0, 4.0, 6.0, -23.0},                        // knee lower edge
        {-22.0, -20.0, 4.0, 6.0, -22.0 - 0.75 * 1.0 / 12.0},    // in knee: -22.0625
        {-20.0, -20.0, 4.0, 6.0, -20.0 - 0.75 * 9.0 / 12.0},    // at T: -20.5625
        {-17.0, -20.0, 4.0, 6.0, -20.0 + 3.0 / 4.0},            // knee upper edge: -19.25
        {0.0, -20.0, 4.0, 6.0, -15.0},                          // above knee
        {-10.0, -20.0, 2.0, 0.0, -15.0},                        // hard knee, above T
        {-30.0, -20.0, 2.0, 0.0, -30.0},                        // hard knee, below T
        {0.0, -30.0, 20.0, 12.0, -30.0 + 30.0 / 20.0},          // high ratio: -28.5
    };
    for (const auto& c : cases) {
        char name[96];
        std::snprintf(name, sizeof name, "staticCurve(%.1f, T=%.0f, R=%.0f, W=%.0f)",
                      c.in, c.t, c.r, c.w);
        checkNear("static", name, CompressorNode::staticCurve(c.in, c.t, c.r, c.w), c.want, 1e-12);
    }

    // Single-source proof: the instance path (staticCurveDb -> gainComputer)
    // must be BIT-IDENTICAL to the static the UI calls, across the whole axis.
    CompressorNode node;
    node.prepare(44100.0, 512, 1);
    CompressorNode::Params p;
    p.thresholdDb = -20.0;
    p.ratio = 4.0;
    p.kneeDb = 6.0;
    node.setParams(p);
    bool identical = true;
    for (int i = 0; i <= 600; ++i) {
        const double x = -60.0 + 0.1 * i;
        if (node.staticCurveDb(x) != CompressorNode::staticCurve(x, -20.0, 4.0, 6.0)) {
            identical = false;
            break;
        }
    }
    check("static", "instance staticCurveDb == static staticCurve (bit-identical, 601 pts)",
          identical, "member gainComputer delegates to the promoted static");
}

}  // namespace

int main() {
    verifyMeterMaps();
    verifyCurveMap();
    verifyStaticCurve();
    std::printf("\n%s\n", gAllPass ? "ALL PASS" : "FAILURES PRESENT");
    return gAllPass ? 0 : 1;
}

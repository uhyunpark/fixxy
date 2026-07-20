#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

// =============================================================================
//  LpngLookAndFeel — shared visual identity for the Lpng plugin family
// =============================================================================
//
//  DESIGN SPEC (this block is the contract every Lpng editor follows)
//  -----------------------------------------------------------------------------
//  Direction: dark, flat, single warm accent. Modern developer-tool / analyzer
//  aesthetic (ZL Audio / FabFilter class), NOT skeuomorphic. No gradients, no
//  bevels, no drop shadows, no image assets. Chrome is monochrome so the DATA is
//  the only thing that carries colour, and the ACCENT is scarce enough to mean
//  something. LpngEQ and LpngComp must read as one product.
//
//  THE ONE RULE — colour encodes meaning, not decoration:
//    * Signal / data      -> COOL blues & greys  (analyzer, response curve,
//                            spectrum, meters showing level). See palette::data*.
//    * Dynamics / action  -> the WARM accent     (gain-reduction, threshold,
//                            compression amount, anything the processor is
//                            *doing* to the signal). See palette::accent.
//    * Chrome             -> MONOCHROME greys     (backgrounds, grids, ticks,
//                            borders, labels, knob tracks). Never coloured.
//  Amber vs blue is the family's whole story: if you ever can't decide, ask
//  "is this the signal, or the plugin acting on the signal?" Keep the accent
//  rare — a screen with amber everywhere has lost the signal.
//
//  BAND HUE convention (EQ, and any multi-band comp): per-band identity colour
//  is HSV(i/N, 0.65, 0.95) via palette::bandColour(). This is the ONE sanctioned
//  exception to "chrome is monochrome" — a band's hue is its identity, reused
//  for its handle, its curve, and its On-toggle tick. Do not invent per-band
//  colours anywhere else.
//
//  PALETTE (ARGB hex)                         ROLE
//    window        0xff141417   editor backdrop (darkest layer)
//    panel         0xff1c1c21   control strips / side panels (mid layer)
//    displayWell   0xff1a1a1e   graph & meter wells (the inset "screen")
//    gridMinor     0xff28282e   minor gridlines
//    gridMajor     0xff3a3a40   major gridlines / knob track
//    axis          0xff45454e   axis emphasis (0 dB), panel borders
//    tick          0xff5a5a62   tick marks, faint dividers
//    textPrimary   0xffe6e6ea   values, active labels
//    textSecondary 0xff9a9aa4   control labels
//    textMuted     0xff707078   axis numerals
//    textFaint     0xff606068   secondary axis numerals (analyzer dB)
//    dataPre       0xff6b7a90   spectrum "before" / reference signal (cool)
//    dataPost      0xff8fd0ff   spectrum "after" / live signal (cool, bright)
//    dataSum       0xfff2f4f8   summed response curve (near-white)
//    controlFill   0xff6fa8d6   default knob value arc (cool "control" blue)
//    accent        0xffe0783c   dynamics / gain-reduction (warm amber) <- SCARCE
//    accentBright  0xfff0954a   accent hover / emphasis
//    accentDim     0xff8a4e2a   accent, disabled / at-rest
//
//  SPACING & METRIC RULES:
//    * One spacing unit = kSpacing (6 px). Pad and gap in multiples of it.
//    * Knob diameters: kKnobS / kKnobM / kKnobL. Pick by importance, not by
//      whatever fits — a section's primary control is L, trims are S.
//    * Corner radius kCorner (3 px) on every rounded rect. Flat, slightly
//      softened — never fully square, never pill-round except toggles.
//    * Fonts: labelFont() for labels, valueFont() (monospaced) for numeric
//      readouts, headerFont() for section titles. Monospaced values are a
//      deliberate signature — they read as "instrument", and columns of dB/Hz
//      line up on the decimal.
//
//  DO
//    * Let the accent appear only for dynamics/GR. Let cool blues own the signal.
//    * Set Slider::rotarySliderFillColourId to palette::accent on dynamics
//      knobs (threshold, ratio, range) so the knob agrees with its meter.
//    * Reuse palette::bandColour() for anything per-band.
//  DON'T
//    * Don't add gradients, shadows, textures, or image assets.
//    * Don't colour chrome, and don't spend the accent on ordinary controls.
//    * Don't hardcode hex in a component — pull from palette:: so the family
//      restyles in one place.
//    * Don't re-derive band hues; call palette::bandColour().
// =============================================================================

namespace lpng {

// -----------------------------------------------------------------------------
//  Palette — every colour a named constexpr ARGB with a one-line role.
//  Stored as juce::uint32 (JUCE 8's juce::Colour ctor is not constexpr); wrap in
//  juce::Colour(...) at the use site, or via the col() helper below.
// -----------------------------------------------------------------------------
namespace palette {
    // --- background layers (darkest -> lifted) ---
    inline constexpr juce::uint32 window        = 0xff141417;  // editor backdrop
    inline constexpr juce::uint32 panel         = 0xff1c1c21;  // control strips / side panels
    inline constexpr juce::uint32 displayWell   = 0xff1a1a1e;  // graph & meter wells (the "screen")

    // --- chrome greys (monochrome — structure only, never meaning) ---
    inline constexpr juce::uint32 gridMinor     = 0xff28282e;  // minor gridlines
    inline constexpr juce::uint32 gridMajor     = 0xff3a3a40;  // major gridlines / knob track
    inline constexpr juce::uint32 axis          = 0xff45454e;  // axis emphasis (0 dB) / borders
    inline constexpr juce::uint32 tick          = 0xff5a5a62;  // tick marks / faint dividers

    // --- text hierarchy (bright -> faint) ---
    inline constexpr juce::uint32 textPrimary   = 0xffe6e6ea;  // values, active labels
    inline constexpr juce::uint32 textSecondary = 0xff9a9aa4;  // control labels
    inline constexpr juce::uint32 textMuted     = 0xff707078;  // axis numerals
    inline constexpr juce::uint32 textFaint     = 0xff606068;  // secondary axis numerals

    // --- data colours (COOL — the signal itself) ---
    inline constexpr juce::uint32 dataPre       = 0xff6b7a90;  // spectrum before / reference
    inline constexpr juce::uint32 dataPost      = 0xff8fd0ff;  // spectrum after / live
    inline constexpr juce::uint32 dataSum       = 0xfff2f4f8;  // summed response curve
    inline constexpr juce::uint32 controlFill   = 0xff6fa8d6;  // default knob value arc

    // --- accent (WARM — dynamics / gain-reduction; keep it scarce) ---
    inline constexpr juce::uint32 accent        = 0xffe0783c;  // GR / dynamics action
    inline constexpr juce::uint32 accentBright  = 0xfff0954a;  // accent hover / emphasis
    inline constexpr juce::uint32 accentDim     = 0xff8a4e2a;  // accent at-rest / disabled

    /** Convenience: uint32 ARGB -> juce::Colour. */
    inline juce::Colour col(juce::uint32 argb) noexcept { return juce::Colour(argb); }

    /** Per-band identity hue — the single sanctioned coloured-chrome exception.
        Matches the EQ's historical HSV(i/N, 0.65, 0.95) so existing band
        colours are unchanged; share this so both plugins agree. */
    inline juce::Colour bandColour(int index, int numBands) noexcept {
        const auto n = numBands > 0 ? numBands : 1;
        return juce::Colour::fromHSV(static_cast<float>(index) / static_cast<float>(n),
                                     0.65f, 0.95f, 1.0f);
    }
}  // namespace palette

// -----------------------------------------------------------------------------
//  Typography + metric constants.
// -----------------------------------------------------------------------------
namespace metrics {
    inline constexpr int   kSpacing     = 6;    // base spacing unit (px)
    inline constexpr float kCorner      = 3.0f; // rounded-rect radius

    inline constexpr int   kKnobS       = 36;   // trims / secondary controls
    inline constexpr int   kKnobM       = 52;   // standard control
    inline constexpr int   kKnobL       = 72;   // section-primary control

    inline constexpr float kFontLabel   = 12.0f; // control labels
    inline constexpr float kFontSmall    = 11.0f; // axis numerals / dense labels
    inline constexpr float kFontValue   = 12.0f; // numeric readouts (monospaced)
    inline constexpr float kFontHeader  = 13.0f; // section titles
}  // namespace metrics

/** Sans label font at the family's standard label size. */
inline juce::Font labelFont(float size = metrics::kFontLabel) {
    return juce::Font(juce::FontOptions(size));
}

/** Monospaced numeric-readout font — the family signature for dB/Hz/Q values. */
inline juce::Font valueFont(float size = metrics::kFontValue) {
    return juce::Font(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(),
                                        size, juce::Font::plain));
}

/** Bold sans section-header font. */
inline juce::Font headerFont(float size = metrics::kFontHeader) {
    return juce::Font(juce::FontOptions(size).withStyle("Bold"));
}

// -----------------------------------------------------------------------------
//  LpngLookAndFeel — put one on the editor and every child renders on-family.
// -----------------------------------------------------------------------------
class LpngLookAndFeel : public juce::LookAndFeel_V4 {
public:
    LpngLookAndFeel() {
        using namespace palette;

        // Any component under this L&F picks up family colours by default.
        setColour(juce::ResizableWindow::backgroundColourId, col(window));
        setColour(juce::DocumentWindow::backgroundColourId,  col(window));

        setColour(juce::Label::textColourId,       col(textSecondary));
        setColour(juce::Label::backgroundColourId, juce::Colours::transparentBlack);

        // Sliders: cool "control" fill by default; dynamics knobs override
        // rotarySliderFillColourId to palette::accent (see spec DO list).
        setColour(juce::Slider::rotarySliderFillColourId,    col(controlFill));
        setColour(juce::Slider::rotarySliderOutlineColourId, col(gridMajor));
        setColour(juce::Slider::thumbColourId,               col(textPrimary));
        setColour(juce::Slider::textBoxTextColourId,         col(textPrimary));
        setColour(juce::Slider::textBoxBackgroundColourId,   juce::Colours::transparentBlack);
        setColour(juce::Slider::textBoxOutlineColourId,      juce::Colours::transparentBlack);
        setColour(juce::Slider::trackColourId,               col(controlFill));
        setColour(juce::Slider::backgroundColourId,          col(gridMajor));

        setColour(juce::ComboBox::backgroundColourId, col(panel));
        setColour(juce::ComboBox::textColourId,       col(textPrimary));
        setColour(juce::ComboBox::outlineColourId,    col(axis));
        setColour(juce::ComboBox::arrowColourId,      col(textMuted));
        setColour(juce::ComboBox::buttonColourId,     col(panel));

        setColour(juce::PopupMenu::backgroundColourId,            col(panel));
        setColour(juce::PopupMenu::textColourId,                  col(textPrimary));
        setColour(juce::PopupMenu::highlightedBackgroundColourId, col(axis));
        setColour(juce::PopupMenu::highlightedTextColourId,       col(textPrimary));

        // ToggleButton: default tick = cool control colour. The EQ overrides
        // tickColourId per band (band-hue On-checkbox) and that keeps working —
        // drawToggleButton() below honours whatever tickColourId is set.
        setColour(juce::ToggleButton::textColourId,        col(textSecondary));
        setColour(juce::ToggleButton::tickColourId,        col(controlFill));
        setColour(juce::ToggleButton::tickDisabledColourId, col(gridMajor));

        setColour(juce::TextButton::buttonColourId,   col(panel));
        setColour(juce::TextButton::textColourOffId,  col(textSecondary));
        setColour(juce::TextButton::textColourOnId,   col(textPrimary));
    }

    // --- slider textboxes are numeric readouts -> the family's monospaced
    //     value font (spec: "Monospaced values are a deliberate signature").
    //     Other labels keep whatever font they were given. ---
    juce::Font getLabelFont(juce::Label& label) override {
        if (dynamic_cast<juce::Slider*>(label.getParentComponent()) != nullptr)
            return valueFont();
        return LookAndFeel_V4::getLabelFont(label);
    }

    // --- flat rotary: background arc + value arc + pointer line, no gradients ---
    void drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height,
                          float sliderPos, float rotaryStartAngle, float rotaryEndAngle,
                          juce::Slider& slider) override {
        const auto bounds = juce::Rectangle<int>(x, y, width, height).toFloat();
        const float diameter = juce::jmin(bounds.getWidth(), bounds.getHeight());
        const float radius = diameter * 0.5f;
        const auto centre = bounds.getCentre();

        // Ring thickness scales gently with size but stays crisp on small knobs.
        const float thickness = juce::jmax(3.0f, radius * 0.16f);
        const float arcRadius = radius - thickness * 0.5f - 1.0f;
        const float angle = rotaryStartAngle + sliderPos * (rotaryEndAngle - rotaryStartAngle);

        const bool enabled = slider.isEnabled();
        auto fill = slider.findColour(juce::Slider::rotarySliderFillColourId);
        auto track = slider.findColour(juce::Slider::rotarySliderOutlineColourId);
        if (!enabled) {
            fill = fill.withMultipliedAlpha(0.35f);
            track = track.withMultipliedAlpha(0.5f);
        }

        // Background track (full sweep).
        juce::Path bg;
        bg.addCentredArc(centre.x, centre.y, arcRadius, arcRadius, 0.0f,
                         rotaryStartAngle, rotaryEndAngle, true);
        g.setColour(track);
        g.strokePath(bg, juce::PathStrokeType(thickness, juce::PathStrokeType::curved,
                                              juce::PathStrokeType::rounded));

        // Value arc (start -> current).
        if (sliderPos > 0.0f) {
            juce::Path val;
            val.addCentredArc(centre.x, centre.y, arcRadius, arcRadius, 0.0f,
                              rotaryStartAngle, angle, true);
            g.setColour(fill);
            g.strokePath(val, juce::PathStrokeType(thickness, juce::PathStrokeType::curved,
                                                   juce::PathStrokeType::rounded));
        }

        // Pointer line from an inner radius to just under the ring.
        const float pointerInner = arcRadius - thickness * 0.5f - 2.0f;
        const float pointerOuter = arcRadius + thickness * 0.5f;
        const juce::Point<float> pa(centre.x + pointerInner * std::cos(angle - juce::MathConstants<float>::halfPi),
                                    centre.y + pointerInner * std::sin(angle - juce::MathConstants<float>::halfPi));
        const juce::Point<float> pb(centre.x + pointerOuter * std::cos(angle - juce::MathConstants<float>::halfPi),
                                    centre.y + pointerOuter * std::sin(angle - juce::MathConstants<float>::halfPi));
        g.setColour(enabled ? palette::col(palette::textPrimary)
                            : palette::col(palette::textPrimary).withMultipliedAlpha(0.4f));
        g.drawLine({pa, pb}, juce::jmax(1.5f, thickness * 0.35f));
    }

    // --- compact labelled check: rounded box (fills with tickColour when on) + text ---
    void drawToggleButton(juce::Graphics& g, juce::ToggleButton& button,
                          bool shouldDrawButtonAsHighlighted, bool /*shouldDrawButtonAsDown*/) override {
        const auto bounds = button.getLocalBounds().toFloat();
        const bool on = button.getToggleState();
        const bool enabled = button.isEnabled();

        const float boxSize = juce::jmin(16.0f, bounds.getHeight() - 2.0f);
        juce::Rectangle<float> box(bounds.getX() + 1.0f,
                                   bounds.getCentreY() - boxSize * 0.5f,
                                   boxSize, boxSize);

        // On -> fill with the button's tick colour (band hue in the EQ);
        // off -> hollow with a chrome border. Highlight lifts the border.
        auto tick = button.findColour(on ? juce::ToggleButton::tickColourId
                                         : juce::ToggleButton::tickDisabledColourId);
        auto border = shouldDrawButtonAsHighlighted ? palette::col(palette::tick)
                                                    : palette::col(palette::axis);
        if (!enabled) {
            tick = tick.withMultipliedAlpha(0.4f);
            border = border.withMultipliedAlpha(0.5f);
        }

        if (on) {
            g.setColour(tick);
            g.fillRoundedRectangle(box, metrics::kCorner);
        }
        g.setColour(on ? tick.brighter(0.2f) : border);
        g.drawRoundedRectangle(box.reduced(0.5f), metrics::kCorner, 1.2f);

        // Label to the right of the box.
        const auto text = button.getButtonText();
        if (text.isNotEmpty()) {
            auto textColour = button.findColour(juce::ToggleButton::textColourId);
            if (!enabled) textColour = textColour.withMultipliedAlpha(0.5f);
            g.setColour(textColour);
            g.setFont(labelFont());
            auto textArea = bounds.withLeft(box.getRight() + static_cast<float>(metrics::kSpacing));
            g.drawText(text, textArea, juce::Justification::centredLeft, true);
        }
    }
};

}  // namespace lpng

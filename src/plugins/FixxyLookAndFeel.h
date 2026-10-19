#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

// =============================================================================
//  FixxyLookAndFeel — shared visual identity for the fixxy plugin family
// =============================================================================
//
//  DESIGN SPEC (this block is the contract every fixxy editor follows)
//  -----------------------------------------------------------------------------
//  Direction: dark, flat, single warm accent. Modern developer-tool / analyzer
//  aesthetic (ZL Audio / FabFilter class), NOT skeuomorphic. No gradients, no
//  bevels, no drop shadows, no image assets. Chrome is monochrome so the DATA is
//  the only thing that carries colour, and the ACCENT is scarce enough to mean
//  something. fixxy EQ and fixxy Comp must read as one product.
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
//
//  LIGHT VARIANT — "Family Light" (namespace palettelight + FixxyLookAndFeelLight)
//  -----------------------------------------------------------------------------
//  Owner decision (2026-08-10, docs/issues.md DESIGN-DIRECTION): dark mode is
//  retired; the whole family moves to a warm-light "paper" language. fixxy Spectral
//  DEBUTS it; EQ/Comp migrate later (token swap — NOT in that task). The dark
//  palette above stays intact so those two editors are undisturbed until then.
//  The light language keeps THE ONE RULE verbatim — only the medium changes:
//    * canvas = warm cream #fbfaf9; surfaces = stone #f2f0ed / white #ffffff,
//      separated by 1px INSET HAIRLINES + tonal shifts, NEVER shadows (design
//      system rejects any elevation > 0.04 alpha).
//    * radius system: kCornerCard (10px) for cards/wells/rail, full pills for
//      toggle/buttons, kCornerChip (4px) for value chips.
//    * SIGNAL data -> deep azure #0b61c4 (light blue dies on paper); the
//      weighting/response curve -> ink #1b1b1b (highest-contrast mark).
//    * ACTION -> ember #ff3e00 (reduction ribbon, hero/action knob, active
//      delta) — this is exactly where design-reference's action accent and the
//      family's warm-action slot coincide, so the accent carries straight over.
//    * value readouts use a tabular SANS, not the dark mono (tabular figures are
//      the light stand-in for the mono signature).
//  When you migrate EQ/Comp: swap the namespace the editor pulls from
//  (palette:: -> palettelight::) and setLookAndFeel(&FixxyLookAndFeelLight), don't
//  hardcode. Re-verify the analyzer/meter data colours against the cream canvas.
// =============================================================================

namespace fixxy {

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
//  LIGHT palette — the "Family Light" language (design-reference.md, approved
//  2026-08-10, docs/mockups/spectral-ui-mockup.html variant B). SAME semantic
//  rule as the dark palette (colour = meaning, chrome monochrome), only the
//  medium changes: a warm CREAM-PAPER canvas, surfaces separated by hairline
//  INSET borders + tonal shifts instead of shadows (never a shadow > 0.04
//  alpha), and data colours re-tuned so they read on paper. The cool=signal /
//  warm=action rule is unchanged, and this is the exact point where the family's
//  action slot (warm) and design-reference's action accent (Ember #ff3e00)
//  coincide — so the accent is carried straight over.
//
//    * Signal / data      -> DEEP AZURE #0b61c4  (spectrum, response) — light
//                            blue dies on cream, so it is darkened & saturated.
//    * Weighting curve     -> INK #1b1b1b        (must be the highest-contrast
//                            mark on paper; white would vanish).
//    * Dynamics / action  -> EMBER #ff3e00       (reduction ribbon, hero knob,
//                            active delta) — scarce, exactly as amber was.
//    * Chrome             -> cream/stone/ink greys, hairline borders.
//
//  Names mirror namespace palette:: 1:1 so an editor swaps languages by swapping
//  the namespace it pulls from. Names that only exist in the light system (chip,
//  pill, knob face) are added at the end.
// -----------------------------------------------------------------------------
namespace palettelight {
    // --- surfaces (paper -> lifted / ink) ---
    inline constexpr juce::uint32 window        = 0xfffbfaf9;  // cream canvas (page backdrop)
    inline constexpr juce::uint32 panel         = 0xfff2f0ed;  // stone rail / secondary surface
    inline constexpr juce::uint32 displayWell   = 0xffffffff;  // pure-white plot well

    // --- chrome greys (structure only, never meaning) ---
    inline constexpr juce::uint32 gridMinor     = 0xffefece8;  // minor gridlines
    inline constexpr juce::uint32 gridMajor     = 0xffe3ded7;  // major gridlines / knob track
    inline constexpr juce::uint32 axis          = 0xffded7ce;  // axis emphasis / borders
    inline constexpr juce::uint32 tick          = 0xffcfc7bc;  // tick marks / faint dividers

    // --- text hierarchy (ink -> faint) ---
    inline constexpr juce::uint32 textPrimary   = 0xff121212;  // ink: headings, values
    inline constexpr juce::uint32 textSecondary = 0xff474645;  // body brown: labels
    inline constexpr juce::uint32 textMuted     = 0xff7e7e7d;  // helper / axis numerals
    inline constexpr juce::uint32 textFaint     = 0xff9a9793;  // secondary axis numerals

    // --- data colours (re-tuned for paper) ---
    inline constexpr juce::uint32 dataPre       = 0xff6d82a6;  // muted steel: reference signal
    inline constexpr juce::uint32 dataPost      = 0xff0b61c4;  // DEEP AZURE: live signal
    inline constexpr juce::uint32 dataSum       = 0xff1b1b1b;  // ink: weighting/response curve
    inline constexpr juce::uint32 controlFill   = 0xff0b61c4;  // default knob value arc (azure)

    // --- accent (WARM action = Ember; keep it scarce) ---
    inline constexpr juce::uint32 accent        = 0xffff3e00;  // reduction / action / hero knob
    inline constexpr juce::uint32 accentBright  = 0xffff5a2a;  // accent hover / emphasis
    inline constexpr juce::uint32 accentDim     = 0xfff2b7a6;  // accent at-rest / disabled

    // --- light-only tokens (chip / pill / knob face) ---
    inline constexpr juce::uint32 chipBg        = 0xfff6f4ef;  // sand value-chip fill
    inline constexpr juce::uint32 chipBorder    = 0xffece7e0;  // chip hairline
    inline constexpr juce::uint32 hairline      = 0xfff2f0ed;  // inset well border (stone)
    inline constexpr juce::uint32 pillTrack     = 0xffe7e2db;  // pill-toggle track
    inline constexpr juce::uint32 pillOnBg      = 0xff121212;  // pill selected fill (ink)
    inline constexpr juce::uint32 pillOnFg      = 0xfffbfaf9;  // pill selected text (cream)
    inline constexpr juce::uint32 pillFgOff     = 0xff7e7e7d;  // pill idle text
    inline constexpr juce::uint32 btnRing       = 0xffd8d1c8;  // idle pill / icon outline
    inline constexpr juce::uint32 knobFace      = 0xffffffff;  // white knob face
    inline constexpr juce::uint32 knobFaceEdge  = 0xffece7e0;  // knob face hairline
    inline constexpr juce::uint32 reduceFill    = 0x6bff3e00;  // ribbon fill (ember @ 0.42)

    inline juce::Colour col(juce::uint32 argb) noexcept { return juce::Colour(argb); }

    /** Per-band identity hue for the LIGHT family — 8 hues (fixxy Spectral's
        weight-curve bands, kMaxWeightBands) tuned for legibility on the cream/
        white surfaces: lower value than the dark palette::bandColour()'s
        HSV(i/n, 0.65, 0.95) so a hue like yellow doesn't wash out against
        near-white. Same sanctioned exception as palette::bandColour() above —
        a band's hue is its identity (dot handle, curve emphasis, On-toggle
        tick); do not invent per-band colours anywhere else. */
    inline juce::Colour bandColour(int index) noexcept {
        return juce::Colour::fromHSV(static_cast<float>(index) / 8.0f, 0.72f, 0.72f, 1.0f);
    }
}  // namespace palettelight

// -----------------------------------------------------------------------------
//  Typography + metric constants.
// -----------------------------------------------------------------------------
namespace metrics {
    inline constexpr int   kSpacing     = 6;    // base spacing unit (px)
    inline constexpr float kCorner      = 3.0f; // rounded-rect radius (dark family)
    inline constexpr float kCornerCard  = 10.0f; // LIGHT: cards / wells / rail (paper radius)
    inline constexpr float kCornerChip  = 4.0f;  // LIGHT: value chips

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
//  FixxyLookAndFeel — put one on the editor and every child renders on-family.
// -----------------------------------------------------------------------------
class FixxyLookAndFeel : public juce::LookAndFeel_V4 {
public:
    FixxyLookAndFeel() {
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

// -----------------------------------------------------------------------------
//  FixxyLookAndFeelLight — the "Family Light" skin. Debuts on fixxy Spectral; the
//  two shipping dark editors (EQ/Comp) keep using FixxyLookAndFeel and are
//  untouched. Subclasses the base only to reuse its colour-id plumbing surface;
//  the two data-carrying draws (rotary, toggle) are fully overridden for the
//  flat paper look: no gradients, no shadows, hairline outlines, white knob
//  face, value arc in the slider's fill colour (editor sets azure=signal /
//  ember=action per the family rule), pill toggles instead of dark checkboxes.
// -----------------------------------------------------------------------------
class FixxyLookAndFeelLight : public FixxyLookAndFeel {
public:
    FixxyLookAndFeelLight() {
        namespace pl = palettelight;
        using pl::col;

        setColour(juce::ResizableWindow::backgroundColourId, col(pl::window));
        setColour(juce::DocumentWindow::backgroundColourId,  col(pl::window));

        setColour(juce::Label::textColourId,       col(pl::textSecondary));
        setColour(juce::Label::backgroundColourId, juce::Colours::transparentBlack);

        setColour(juce::Slider::rotarySliderFillColourId,    col(pl::controlFill));
        setColour(juce::Slider::rotarySliderOutlineColourId, col(pl::gridMajor));
        setColour(juce::Slider::thumbColourId,               col(pl::textPrimary));
        setColour(juce::Slider::textBoxTextColourId,         col(pl::textPrimary));
        setColour(juce::Slider::textBoxBackgroundColourId,   juce::Colours::transparentBlack);
        setColour(juce::Slider::textBoxOutlineColourId,      juce::Colours::transparentBlack);

        setColour(juce::ComboBox::backgroundColourId, col(pl::displayWell));
        setColour(juce::ComboBox::textColourId,       col(pl::textPrimary));
        setColour(juce::ComboBox::outlineColourId,    col(pl::chipBorder));
        setColour(juce::ComboBox::arrowColourId,      col(pl::textMuted));

        setColour(juce::PopupMenu::backgroundColourId,            col(pl::displayWell));
        setColour(juce::PopupMenu::textColourId,                  col(pl::textPrimary));
        setColour(juce::PopupMenu::highlightedBackgroundColourId, col(pl::panel));
        setColour(juce::PopupMenu::highlightedTextColourId,       col(pl::textPrimary));

        // Toggle tick colour is the pill's ON fill; editor overrides per-button
        // (bypass -> ink, delta -> ember). Default: ink.
        setColour(juce::ToggleButton::textColourId,         col(pl::textSecondary));
        setColour(juce::ToggleButton::tickColourId,         col(pl::pillOnBg));
        setColour(juce::ToggleButton::tickDisabledColourId, col(pl::btnRing));
    }

    // Light readouts use a plain tabular sans, NOT the dark family's monospace
    // (design-reference: tabular figures stand in for the mono signature).
    juce::Font getLabelFont(juce::Label& label) override {
        if (dynamic_cast<juce::Slider*>(label.getParentComponent()) != nullptr)
            return labelFont();
        return LookAndFeel_V4::getLabelFont(label);
    }

    // --- flat light rotary: hairline track arc + value arc (fill colour) +
    //     white face with hairline edge + ink pointer. No gradients/shadows. ---
    void drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height,
                          float sliderPos, float rotaryStartAngle, float rotaryEndAngle,
                          juce::Slider& slider) override {
        namespace pl = palettelight;
        const auto bounds = juce::Rectangle<int>(x, y, width, height).toFloat();
        const float diameter = juce::jmin(bounds.getWidth(), bounds.getHeight());
        const float radius = diameter * 0.5f;
        const auto centre = bounds.getCentre();

        const float thickness = juce::jmax(3.0f, radius * 0.16f);
        const float arcRadius = radius - thickness * 0.5f - 1.0f;
        const float angle = rotaryStartAngle + sliderPos * (rotaryEndAngle - rotaryStartAngle);
        const float faceR = arcRadius - thickness * 0.5f - 2.5f;

        const bool enabled = slider.isEnabled();
        auto fill = slider.findColour(juce::Slider::rotarySliderFillColourId);
        auto track = slider.findColour(juce::Slider::rotarySliderOutlineColourId);
        if (!enabled) {
            fill = fill.withMultipliedAlpha(0.4f);
            track = track.withMultipliedAlpha(0.6f);
        }

        // Background track (full sweep).
        juce::Path bg;
        bg.addCentredArc(centre.x, centre.y, arcRadius, arcRadius, 0.0f,
                         rotaryStartAngle, rotaryEndAngle, true);
        g.setColour(track);
        g.strokePath(bg, juce::PathStrokeType(thickness, juce::PathStrokeType::curved,
                                              juce::PathStrokeType::rounded));

        // Value arc (start -> current) in the slider's fill colour.
        if (sliderPos > 0.0f) {
            juce::Path val;
            val.addCentredArc(centre.x, centre.y, arcRadius, arcRadius, 0.0f,
                              rotaryStartAngle, angle, true);
            g.setColour(fill);
            g.strokePath(val, juce::PathStrokeType(thickness, juce::PathStrokeType::curved,
                                                   juce::PathStrokeType::rounded));
        }

        // White knob face with a hairline edge (paper, not skeuomorphic).
        if (faceR > 2.0f) {
            g.setColour(pl::col(pl::knobFace).withMultipliedAlpha(enabled ? 1.0f : 0.6f));
            g.fillEllipse(centre.x - faceR, centre.y - faceR, faceR * 2.0f, faceR * 2.0f);
            g.setColour(pl::col(pl::knobFaceEdge));
            g.drawEllipse(centre.x - faceR, centre.y - faceR, faceR * 2.0f, faceR * 2.0f, 1.4f);
        }

        // Ink pointer from just inside the face to its edge.
        const float pointerInner = faceR * 0.42f;
        const float pointerOuter = faceR - 1.0f;
        const juce::Point<float> pa(centre.x + pointerInner * std::cos(angle - juce::MathConstants<float>::halfPi),
                                    centre.y + pointerInner * std::sin(angle - juce::MathConstants<float>::halfPi));
        const juce::Point<float> pb(centre.x + pointerOuter * std::cos(angle - juce::MathConstants<float>::halfPi),
                                    centre.y + pointerOuter * std::sin(angle - juce::MathConstants<float>::halfPi));
        g.setColour(pl::col(pl::textPrimary).withMultipliedAlpha(enabled ? 1.0f : 0.4f));
        g.drawLine({pa, pb}, juce::jmax(2.0f, thickness * 0.42f));
    }

    // --- a rotary slider's value TEXTBOX renders as a sand value chip (so a
    //     knob + its readout reads as the mockup's knob-over-chip). Non-slider
    //     labels fall through to the base. ---
    void drawLabel(juce::Graphics& g, juce::Label& label) override {
        namespace pl = palettelight;
        if (dynamic_cast<juce::Slider*>(label.getParentComponent()) != nullptr) {
            auto b = label.getLocalBounds().toFloat().reduced(0.75f);
            g.setColour(pl::col(pl::chipBg));
            g.fillRoundedRectangle(b, metrics::kCornerChip);
            g.setColour(pl::col(pl::chipBorder));
            g.drawRoundedRectangle(b, metrics::kCornerChip, 1.0f);
            g.setColour(pl::col(pl::textPrimary));
            g.setFont(getLabelFont(label));
            g.drawText(label.getText(), b, juce::Justification::centred, false);
            return;
        }
        LookAndFeel_V4::drawLabel(g, label);
    }

    // --- LinearBar as a value CHIP: sand rounded rect + hairline + centred
    //     tabular value text (ink). Draggable/typable like any bar slider, but
    //     reads as one of the mockup's value chips. Other linear styles fall
    //     through to the base. ---
    void drawLinearSlider(juce::Graphics& g, int x, int y, int width, int height,
                          float sliderPos, float minSliderPos, float maxSliderPos,
                          juce::Slider::SliderStyle style, juce::Slider& slider) override {
        namespace pl = palettelight;
        if (style == juce::Slider::LinearBar || style == juce::Slider::LinearBarVertical) {
            auto b = juce::Rectangle<int>(x, y, width, height).toFloat().reduced(0.75f);
            const bool enabled = slider.isEnabled();
            g.setColour(pl::col(pl::chipBg).withMultipliedAlpha(enabled ? 1.0f : 0.6f));
            g.fillRoundedRectangle(b, metrics::kCornerChip);
            g.setColour(pl::col(pl::chipBorder));
            g.drawRoundedRectangle(b, metrics::kCornerChip, 1.0f);
            g.setColour(pl::col(pl::textPrimary).withMultipliedAlpha(enabled ? 1.0f : 0.5f));
            g.setFont(labelFont());
            g.drawText(slider.getTextFromValue(slider.getValue()), b,
                       juce::Justification::centred, false);
            return;
        }
        LookAndFeel_V4::drawLinearSlider(g, x, y, width, height, sliderPos,
                                         minSliderPos, maxSliderPos, style, slider);
    }

    // --- light pill toggle: centred label in a full-height rounded pill. On ->
    //     filled with tickColour (ink for bypass / ember for delta), cream text;
    //     off -> hairline outline, muted text. ---
    void drawToggleButton(juce::Graphics& g, juce::ToggleButton& button,
                          bool shouldDrawButtonAsHighlighted, bool /*down*/) override {
        namespace pl = palettelight;
        const auto bounds = button.getLocalBounds().toFloat().reduced(0.75f);
        const bool on = button.getToggleState();
        const bool enabled = button.isEnabled();
        const float r = bounds.getHeight() * 0.5f;  // full pill

        auto onFill = button.findColour(juce::ToggleButton::tickColourId);
        if (!enabled) onFill = onFill.withMultipliedAlpha(0.4f);

        if (on) {
            g.setColour(onFill);
            g.fillRoundedRectangle(bounds, r);
        } else {
            auto ring = shouldDrawButtonAsHighlighted ? pl::col(pl::textMuted) : pl::col(pl::btnRing);
            if (!enabled) ring = ring.withMultipliedAlpha(0.5f);
            g.setColour(ring);
            g.drawRoundedRectangle(bounds, r, 1.2f);
        }

        const auto text = button.getButtonText();
        if (text.isNotEmpty()) {
            auto textColour = on ? pl::col(pl::pillOnFg)
                                 : (shouldDrawButtonAsHighlighted ? pl::col(pl::textPrimary)
                                                                  : pl::col(pl::pillFgOff));
            if (!enabled) textColour = textColour.withMultipliedAlpha(0.5f);
            g.setColour(textColour);
            g.setFont(labelFont());
            g.drawText(text, bounds, juce::Justification::centred, true);
        }
    }
};

}  // namespace fixxy

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "SpaceDustDither.h"

//==============================================================================
/**
    SpaceDustLookAndFeel
    
    Premium cosmic-themed LookAndFeel for Space Dust synthesizer.
    Enhances typography with shadows, glows, and cosmic color palette.
*/
class SpaceDustLookAndFeel : public juce::LookAndFeel_V4
{
public:
    SpaceDustLookAndFeel();
    ~SpaceDustLookAndFeel() override = default;

    /** THE red. Every element that turns red on clipping uses exactly this
        (Giuseppe, 2026-08-01) -- it is the meters' red, and the meters are the
        reference because they are the thing actually reading the level.

        Before this there were seven different reds in play (ff6666, dd3333, ff8888,
        ff5555, ff4444, dd2222, 991818), so the UI went red in mismatched shades
        depending on which element you looked at. Anything new that reacts to
        clipping must use this rather than picking its own. */
    static constexpr juce::uint32 kClipRed = 0xffff0000;

    /** When true, meter-linked glow (groups, knobs, EQ curve) uses red tones (matches peak clipping). */
    void setOutputMeterClipping(bool clipping) noexcept { outputMeterClipping = clipping; }
    bool isOutputMeterClipping() const noexcept { return outputMeterClipping; }

    /** Averaged output level [0,1], pushed once per UI frame by the editor's timer.
        The knob arcs fill by this, so every knob doubles as a meter. Uses the same
        snapshot every other glow site reads, so the whole UI moves off one value. */
    void setOutputMeterLevel(float level) noexcept
    {
        outputMeterLevel = juce::jlimit(0.0f, 1.0f, level);

        // A follower that trails the level, giving the arc head a "where it came
        // from" to smear back towards.
        //
        // This is why the knob smear needs no per-knob state, which matters when
        // there are fifty-odd of them: every arc is driven by the SAME level, so one
        // lagging copy of that level describes every head's travel at once. Each
        // knob just scales it by its own value.
        outputMeterLag += (outputMeterLevel - outputMeterLag) * kLagFollow;
    }
    float getOutputMeterLevel() const noexcept { return outputMeterLevel; }

    /** 0..1 bloom amount for EVERY element that glows (Giuseppe, 2026-08-01):
        louder output, more glow; silence, none at all.

        One function so the whole UI blooms by the same law -- knobs, meters, group
        outlines and the scopes all scale by this rather than each inventing its own
        response, which is how the old glow ended up inconsistent between elements.

        Curved below linear (^0.7) because glow is perceived closer to logarithmically
        than linearly: a straight mapping spends most of its travel invisible and then
        slams on near the top. Anything that glows should multiply its alpha by this
        and draw nothing when it is zero.

        NOTE: this is per-ELEMENT bloom. The old ambient glow (halos behind the group
        boxes, the arcade edge glow) stays off -- see kGlowEnabled in PluginEditor.cpp. */
    float getGlowAmount() const noexcept
    {
        return std::pow(juce::jlimit(0.0f, 1.0f, outputMeterLevel), 0.7f);
    }

    /** Two-pass bloom behind a stroked path. Widening low-alpha passes rather than a
        real blur -- cheap, and at these weights it reads the same. Draws nothing at
        silence. Public so components outside the LookAndFeel (the EQ curve, the close
        control) can bloom by the same law. */
    void glowPath(juce::Graphics& g, const juce::Path& p,
                  juce::Colour c, float baseThickness) const
    {
        const float glow = getGlowAmount();

        if (glow <= 0.01f)
            return;

        for (int pass = 0; pass < 2; ++pass)
        {
            g.setColour(c.withAlpha(glow * (pass == 0 ? 0.14f : 0.24f)));
            g.strokePath(p, juce::PathStrokeType(baseThickness * (pass == 0 ? 4.0f : 2.2f)));
        }
    }

    /** Two-pass bloom behind a filled rectangle. */
    void glowRect(juce::Graphics& g, juce::Rectangle<float> r, juce::Colour c) const
    {
        const float glow = getGlowAmount();

        if (glow <= 0.01f)
            return;

        for (int pass = 0; pass < 2; ++pass)
        {
            const float widen = (pass == 0) ? 6.0f : 3.0f;
            g.setColour(c.withAlpha(glow * (pass == 0 ? 0.14f : 0.24f)));
            g.fillRect(r.expanded(widen, widen * 0.5f));
        }
    }

    /** Bloom behind TEXT, by stamping it at diagonal offsets in the glow colour.

        Text has no path to widen, so a halo has to be built out of repeat draws --
        the expensive kind of glow. Kept to two rings of four, and gated a little
        higher than the others (0.05 rather than 0.01) because a barely-visible halo
        is not worth eight extra drawText calls on every label in the synth. */
    void glowText(juce::Graphics& g, const juce::String& text, juce::Rectangle<int> area,
                  juce::Justification just, juce::Colour c) const
    {
        const float glow = getGlowAmount();

        if (glow <= 0.05f || text.isEmpty())
            return;

        for (int ring = 2; ring >= 1; --ring)
        {
            g.setColour(c.withAlpha(glow * (ring == 2 ? 0.10f : 0.18f)));

            const int d = ring;

            g.drawText(text, area.translated(-d, -d), just, false);
            g.drawText(text, area.translated( d, -d), just, false);
            g.drawText(text, area.translated(-d,  d), just, false);
            g.drawText(text, area.translated( d,  d), just, false);
        }
    }

    /** Colours that track the meter red zone (for Final EQ and other custom paint). */
    juce::Colour getMeterResponsiveKnobArcColour() const;
    juce::Colour getMeterResponsiveKnobGlowColour() const;

    //==============================================================================
    // -- Typography Enhancements --
    
    void drawLabel(juce::Graphics& g, juce::Label& label) override;
    void drawGroupComponentOutline(juce::Graphics& g, int width, int height,
                                   const juce::String& text,
                                   const juce::Justification& position,
                                   juce::GroupComponent& group) override;
    void drawComboBox(juce::Graphics& g, int width, int height, bool isButtonDown,
                      int buttonX, int buttonY, int buttonW, int buttonH,
                      juce::ComboBox& box) override;
    void drawToggleButton(juce::Graphics& g, juce::ToggleButton& button,
                          bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;
    void drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height,
                          float sliderPos, float rotaryStartAngle, float rotaryEndAngle,
                          juce::Slider& slider) override;
    void drawTabbedButtonBarBackground(juce::TabbedButtonBar& bar, juce::Graphics& g) override;

    //==============================================================================
    // -- Custom Drawing Helpers --
    
    void drawTextWithShadow(juce::Graphics& g, const juce::String& text,
                           int x, int y, int width, int height,
                           juce::Justification justification,
                           juce::Colour textColour = juce::Colours::white,
                           float shadowOpacity = 0.3f,
                           int shadowOffset = 1);
    
    void drawTextWithGlow(juce::Graphics& g, const juce::String& text,
                         int x, int y, int width, int height,
                         juce::Justification justification,
                         juce::Colour textColour,
                         juce::Colour glowColour,
                         float glowIntensity = 0.5f);

    //==============================================================================
    // -- Title Font (Glitch Goblin) / Body Font (standardized) --
    juce::Font getTitleFont(float height) const;
    juce::Font getBodyFont(float height, bool bold = false) const;

    juce::Font getLabelFont(juce::Label&) override;
    juce::Font getComboBoxFont(juce::ComboBox&) override;
    juce::Font getTabButtonFont(juce::TabBarButton&, float height) override;

private:
    juce::Colour titleWhite = juce::Colour(0xffffffff);
    juce::Colour titleLightCyan = juce::Colour(0xffd0f4ff);
    juce::Colour groupTitleGrey = juce::Colour(0xffe0e0e0);
    juce::Colour groupTitleCyan = juce::Colour(0xffc0e0ff);
    juce::Colour labelCyan = juce::Colour(0xffa0d8ff);
    juce::Colour labelCyanAlt = juce::Colour(0xffb8e0ff);
    juce::Colour valueCyan = juce::Colour(0xff6dd5fa);
    juce::Colour valueCyanAlt = juce::Colour(0xff88e0ff);
    juce::Colour shadowBlack = juce::Colour(0x33000000);

    // Knob accent colours
    juce::Colour knobArcCyan    = juce::Colour(0xff00d4ff);
    juce::Colour knobGlowCyan   = juce::Colour(0xff00b4ff);
    juce::Colour knobBodyDark   = juce::Colour(0xff1a1a30);
    juce::Colour knobBodyLight  = juce::Colour(0xff2a2a48);
    juce::Colour knobRimDark    = juce::Colour(0xff303050);
    juce::Colour knobRimLight   = juce::Colour(0xff505078);

    bool  outputMeterClipping = false;
    float outputMeterLevel    = 0.0f;

    /** Trails outputMeterLevel; the gap between them is how far each arc head has
        just travelled, which is what the RGB smear is drawn along. Lower = longer,
        laggier tail. */
    float outputMeterLag = 0.0f;
    SpaceDustDither::TilesPtr ditherTiles;
    static constexpr float kLagFollow = 0.35f;

    juce::Typeface::Ptr glitchGoblinTypeface;
};



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

    /** Bloom behind a SCOPE TRACE -- the same law as glowPath, drawn tighter.

        glowPath's widest pass is 4x the line, which is right for a knob arc or the
        EQ curve: one slow shape whose halo has room to fall off. A scope trace is a
        long, fast, densely-folded line, and at 4x every fold's halo runs into its
        neighbour's; the separate glows merge into one slab of light that swamps the
        trace and looks like it has burst its box (Giuseppe, 2026-08-02). Half the
        spread and slightly lower alphas keep the bloom attached to the line.

        Used by both traces -- oscilloscope and Lissajous -- so they bloom alike.
        The spectrum is bars, not a trace, and keeps its own per-column bloom. */
    void glowTrace(juce::Graphics& g, const juce::Path& p,
                   juce::Colour c, float baseThickness) const
    {
        const float glow = getGlowAmount();

        if (glow <= 0.01f)
            return;

        for (int pass = 0; pass < 2; ++pass)
        {
            g.setColour(c.withAlpha(glow * (pass == 0 ? 0.10f : 0.18f)));
            g.strokePath(p, juce::PathStrokeType(baseThickness * (pass == 0 ? 2.0f : 1.4f)));
        }
    }

    /** Bloom thrown OUTWARD from a control's edge.

        Expanding rounded rects with the control's own footprint clipped out, so the
        light rings the control instead of tinting it -- and, crucially, it must be
        drawn AFTER the control has finished painting. Drawing it first is what hid
        it on the toggles: their lit state fills bounds.expanded(4), which painted
        straight over the glow underneath (Giuseppe, 2026-08-01). */
    void glowAround(juce::Graphics& g, juce::Rectangle<float> footprint,
                    float cornerSize, juce::Colour c) const
    {
        const float glow = getGlowAmount();

        if (glow <= 0.01f)
            return;

        // A REAL blur (juce::DropShadow), not stacked rectangles.
        //
        // The first version filled three expanding rounded rects with the control's
        // footprint clipped out. Two things were wrong with that: three bands is not
        // a falloff, it is three visible edges, and the clip hole was a plain
        // RECTANGLE while the fill was rounded -- so the ring had square inner
        // corners and read as a hard box around the control. In red, on a clipping
        // synth, that was Giuseppe's "red squares" (2026-08-01).
        //
        // DropShadow blurs the actual rounded path, so the light falls off smoothly
        // and follows the control's real shape. Drawn BEFORE the control paints: its
        // own opaque fill then covers the inner half, leaving only the outward spill.
        // Which is also why the radius has to exceed however far the control's own
        // fills reach, or there is nothing left to see.
        juce::Path shape;
        shape.addRoundedRectangle(footprint, cornerSize);

        juce::DropShadow shadow(c.withAlpha(juce::jlimit(0.0f, 1.0f, glow * 0.85f)),
                                14,             // radius, comfortably past the lit state's 4px
                                { 0, 0 });      // centred: a glow, not a shadow

        shadow.drawForPath(g, shape);
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

    /** The toggles' look, with the ToggleButton taken out of it.

        Everything drawToggleButton used to do inline. It is public and takes a
        plain rectangle so that a control which is NOT a toggle can wear the same
        skin -- the Edit buttons beside the waveform menus do, and they sit among
        toggles, where JUCE's default flat button box was the one thing on the
        panel with no bloom on it.

        One definition rather than a second copy of these fills, so the two can
        never drift apart. isLit is the toggle's "on" state: a real toggle passes
        its toggle state, a momentary button passes whether it is pressed. */
    void drawToggleStyleButton(juce::Graphics& g, juce::Rectangle<float> bounds,
                               const juce::String& text, bool isLit);
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
    // Also the colour of the always-on value arc under the level fill: that arc leaves
    // the bottom of the dial in the pointer's own hue and fades from there, so it is
    // held apart from the fill by alpha rather than by being a different blue.
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

//==============================================================================
/**
    A momentary button drawn exactly like the panel's toggles.

    The five Edit buttons beside the waveform menus sit in among toggles and were
    the only controls there drawn by JUCE's default button painter -- a flat blue
    box, with none of the bloom every neighbour has (Giuseppe, 2026-08-11).

    Not a second copy of that look: it calls the same drawToggleStyleButton the
    toggles themselves are drawn by, so the two cannot drift apart.

    A toggle is lit when it is on. This has no on, so it lights while the pointer
    is over it or it is held down -- which gives a momentary button the press
    feedback it needs, in the panel's own language.
*/
class SpaceDustToggleStyleButton : public juce::Button
{
public:
    explicit SpaceDustToggleStyleButton(const juce::String& componentName = {})
        : juce::Button(componentName) {}

    void paintButton(juce::Graphics& g, bool shouldDrawButtonAsHighlighted,
                     bool shouldDrawButtonAsDown) override
    {
        const bool lit = shouldDrawButtonAsHighlighted || shouldDrawButtonAsDown;

        if (auto* spaceDust = dynamic_cast<SpaceDustLookAndFeel*>(&getLookAndFeel()))
        {
            spaceDust->drawToggleStyleButton(g, getLocalBounds().toFloat(),
                                             getButtonText(), lit);
            return;
        }

        // Every panel that holds one of these sets the LookAndFeel above, so this
        // is unreachable in the plugin. It exists so that a button dropped into a
        // bare component is still visible rather than invisible.
        g.setColour(juce::Colour(lit ? 0xff1a4a5f : 0xff1a1a2f));
        g.fillRoundedRectangle(getLocalBounds().toFloat(), 3.0f);
        g.setColour(juce::Colour(0xffa0d8ff));
        g.drawText(getButtonText(), getLocalBounds(), juce::Justification::centred, false);
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SpaceDustToggleStyleButton)
};



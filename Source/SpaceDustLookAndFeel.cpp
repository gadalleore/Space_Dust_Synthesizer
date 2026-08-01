#include "SpaceDustLookAndFeel.h"
#include "BinaryData.h"
#include "SpaceDustDither.h"
#include <juce_graphics/juce_graphics.h>

//==============================================================================
// -- Constructor --

SpaceDustLookAndFeel::SpaceDustLookAndFeel()
{
    setColour(juce::TextButton::textColourOffId, labelCyan);
    setColour(juce::ComboBox::textColourId, labelCyan);
    setColour(juce::ComboBox::backgroundColourId, juce::Colour(0xff1a1a2f));
    setColour(juce::ComboBox::outlineColourId, juce::Colour(0xff3a3a5f));
    setColour(juce::Slider::textBoxTextColourId, valueCyan);
    setColour(juce::Slider::textBoxBackgroundColourId, juce::Colour(0x33000000));
    setColour(juce::Slider::textBoxOutlineColourId, juce::Colour(0x22000000));

    glitchGoblinTypeface = juce::Typeface::createSystemTypefaceFor(
        BinaryData::GlitchGoblin_ttf, BinaryData::GlitchGoblin_ttfSize);

    // Force a single consistent sans-serif for all body UI (avoids pixelated/monospace fallbacks)
    // (Times New Roman was tried 2026-08-01 and rejected -- back to the sans stack.)
#if JUCE_WINDOWS
    setDefaultSansSerifTypefaceName("Segoe UI");
#elif JUCE_MAC
    setDefaultSansSerifTypefaceName("Lucida Grande");
#else
    setDefaultSansSerifTypefaceName("Sans");
#endif
}

juce::Colour SpaceDustLookAndFeel::getMeterResponsiveKnobArcColour() const
{
    return outputMeterClipping ? juce::Colour(kClipRed) : knobArcCyan;
}

juce::Colour SpaceDustLookAndFeel::getMeterResponsiveKnobGlowColour() const
{
    return outputMeterClipping ? juce::Colour(kClipRed) : knobGlowCyan;
}

//==============================================================================
// -- Tab Bar: Semi-transparent background so edge glow shows through --

void SpaceDustLookAndFeel::drawTabbedButtonBarBackground(juce::TabbedButtonBar&, juce::Graphics& g)
{
    // Mildly translucent (0x50 = ~31% opacity) so the parabolic glow shows at bottom of tabs
    g.fillAll(juce::Colour(0x500a0a1f));
}

//==============================================================================
// -- Label Drawing with Shadow --

void SpaceDustLookAndFeel::drawLabel(juce::Graphics& g, juce::Label& label)
{
    g.fillAll(label.findColour(juce::Label::backgroundColourId));

    if (!label.isBeingEdited())
    {
        auto alpha = label.isEnabled() ? 1.0f : 0.5f;
        
        // Cache label text once to avoid multiple getText() calls
        const juce::String labelText = label.getText();
        
        // Use consistent color and font for all labels (no content-based heuristics)
        juce::Colour textColour = labelCyan;
        juce::Font font = getBodyFont(12.0f, true);
        
        textColour = textColour.withAlpha(alpha);
        
        auto area = label.getLocalBounds();
        auto justification = label.getJustificationType();
        
        g.setFont(font);

        // Bloom behind the text, by the same meter-driven law as everything else.
        glowText(g, labelText, area, justification, textColour);

        // Draw subtle shadow (20% opacity, 1px offset)
        g.setColour(shadowBlack);
        g.drawText(labelText, area.translated(1, 1), justification, false);

        // Draw main text
        g.setColour(textColour);
        g.drawText(labelText, area, justification, false);
    }
    else
    {
        g.setColour(label.findColour(juce::Label::outlineColourId));
        g.drawRect(label.getLocalBounds());
    }
}

//==============================================================================
// -- GroupBox Drawing with Enhanced Title --

void SpaceDustLookAndFeel::drawGroupComponentOutline(juce::Graphics& g, int width, int height,
                                                     const juce::String& text,
                                                     const juce::Justification& position,
                                                     juce::GroupComponent& group)
{
    const float textH = 12.0f;  // Standardized body font size
    const float indent = 3.0f;
    const float textEdgeGap = 4.0f;
    const float titleInset = 6.0f;  // Title padding from box edge
    auto cs = 5.0f;

    juce::Font f = getBodyFont(textH, true);

    juce::Path p;
    auto x = indent;
    auto w = juce::jmax(0.0f, (float)width - x * 2.0f);
    auto h = juce::jmax(0.0f, (float)height - indent * 2.0f);  // Full height minus indent
    cs = juce::jmin(cs, w * 0.5f, h * 0.5f);

    auto textW = text.isEmpty() ? 0 : f.getStringWidth(text);
    auto textX = x + titleInset;
    auto textY = indent + titleInset;  // Title inside box, upper-left

    // Box encompasses full area - no external label
    p.addRoundedRectangle(x, indent, w, h, cs);

    const auto roundedStroke = [](float strokeW) {
        return juce::PathStrokeType(strokeW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
    };

    bool isActive = group.getProperties().getWithDefault("isActive", false);
    bool viewportGlow = group.getProperties().getWithDefault("viewportGlow", false);

    // Inward glow from border (more dramatic when effect/LFO enabled)
    if (viewportGlow || isActive)
    {
        const juce::Colour viewportHue = outputMeterClipping ? juce::Colour(kClipRed) : juce::Colour(0xff00b4ff);
        const float glowThick = isActive ? 14.0f : 6.0f;
        const int numBands = 8;
        const float alphaScale = isActive ? 2.0f : 1.0f;
        juce::Path glowPath;
        glowPath.setUsingNonZeroWinding(false);
        for (int i = 0; i < numBands; ++i)
        {
            const float outerInset = i * (glowThick / (float)numBands);
            const float innerInset = (i + 1) * (glowThick / (float)numBands);
            const float outerCs = juce::jmax(3.0f, cs - outerInset);
            const float innerCs = juce::jmax(3.0f, cs - innerInset);
            glowPath.clear();
            glowPath.addRoundedRectangle(x + outerInset, indent + outerInset,
                                         w - 2.0f * outerInset, h - 2.0f * outerInset, outerCs);
            glowPath.addRoundedRectangle(x + innerInset, indent + innerInset,
                                         w - 2.0f * innerInset, h - 2.0f * innerInset, innerCs);
            const float t = (float)i / (float)numBands;
            const int baseAlpha = isActive ? 55 : 38;
            juce::uint8 alpha = juce::uint8(juce::jlimit(0, 255, static_cast<int>(baseAlpha * alphaScale * (1.0f - t * t))));
            g.setColour(viewportHue.withAlpha(alpha));
            g.fillPath(glowPath);
        }
    }

    juce::Colour outlineCol = group.findColour(juce::GroupComponent::outlineColourId)
                              .withMultipliedAlpha(group.isEnabled() ? 1.0f : 0.5f);
    if (isActive)
        outlineCol = outputMeterClipping ? juce::Colour(kClipRed) : juce::Colour(0xff60d4ff);
    else if (viewportGlow)
        outlineCol = outputMeterClipping ? juce::Colour(kClipRed) : juce::Colour(0xff00b4ff);
    // Bloom on the box outline, by the same law as everything else.
    if (const float glow = getGlowAmount(); glow > 0.01f)
    {
        const float base = (viewportGlow || isActive) ? 2.0f : 1.5f;

        for (int pass = 0; pass < 2; ++pass)
        {
            g.setColour(outlineCol.withAlpha(glow * (pass == 0 ? 0.14f : 0.24f)));
            g.strokePath(p, roundedStroke(base * (pass == 0 ? 4.0f : 2.4f)));
        }
    }

    g.setColour(outlineCol);
    g.strokePath(p, roundedStroke((viewportGlow || isActive) ? 2.0f : 1.5f));

    // Title inside box, upper-left corner
    if (text.isNotEmpty())
    {
        g.setFont(f);
        auto textArea = juce::Rectangle<int>(static_cast<int>(textX),
                                            static_cast<int>(textY),
                                            static_cast<int>(textW + textEdgeGap * 2),
                                            static_cast<int>(textH));
        glowText(g, text, textArea, juce::Justification::centredLeft, labelCyan);

        drawTextWithShadow(g, text, textArea.getX(), textArea.getY(),
                          textArea.getWidth(), textArea.getHeight(),
                          juce::Justification::centredLeft,
                          labelCyan, 0.2f, 1);
    }
}

//==============================================================================
// -- ComboBox Drawing --

void SpaceDustLookAndFeel::drawComboBox(juce::Graphics& g, int width, int height, bool isButtonDown,
                                        int buttonX, int buttonY, int buttonW, int buttonH,
                                        juce::ComboBox& box)
{
    auto cornerSize = box.findColour(juce::ComboBox::outlineColourId).isTransparent() ? 0.0f : 3.0f;
    juce::Rectangle<int> boxBounds(0, 0, width, height);

    g.setColour(box.findColour(juce::ComboBox::backgroundColourId));
    g.fillRoundedRectangle(boxBounds.toFloat(), cornerSize);

    {
        juce::Path outline;
        outline.addRoundedRectangle(boxBounds.toFloat().reduced(0.5f, 0.5f), cornerSize);
        glowPath(g, outline, box.findColour(juce::ComboBox::outlineColourId), 1.0f);
    }

    g.setColour(box.findColour(juce::ComboBox::outlineColourId));
    g.drawRoundedRectangle(boxBounds.toFloat().reduced(0.5f, 0.5f), cornerSize, 1.0f);

    juce::Rectangle<int> arrowZone(width - 30, 0, 20, height);
    juce::Path path;
    path.startNewSubPath(arrowZone.getCentreX() - 4, arrowZone.getCentreY() - 2);
    path.lineTo(static_cast<float>(arrowZone.getCentreX()), static_cast<float>(arrowZone.getCentreY() + 2));
    path.lineTo(arrowZone.getCentreX() + 4, arrowZone.getCentreY() - 2);

    g.setColour(box.findColour(juce::ComboBox::arrowColourId).withAlpha((box.isEnabled() ? 0.9f : 0.2f)));
    g.strokePath(path, juce::PathStrokeType(2.0f));
}

//==============================================================================
// -- ToggleButton Drawing with Glow when Checked --

void SpaceDustLookAndFeel::drawToggleButton(juce::Graphics& g, juce::ToggleButton& button,
                                            bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown)
{
    auto bounds = button.getLocalBounds().toFloat();
    auto cornerSize = 3.0f;

    bool isToggled = button.getToggleState();

    // Meter-driven bloom around the button's edge, on top of the fixed lit state
    // below. The lit state says "this is on"; this says "this is how loud it is".
    {
        juce::Path outline;
        outline.addRoundedRectangle(bounds.reduced(0.5f), cornerSize);

        glowPath(g, outline,
                 outputMeterClipping ? juce::Colour(kClipRed)
                                     : juce::Colour(isToggled ? 0xff00b4ff : 0xff3a3a5f),
                 1.5f);
    }

    // Background: glow effect when checked (brighter blue/cyan, or red when meter clips)
    if (isToggled)
    {
        if (outputMeterClipping)
        {
            g.setColour(juce::Colour(kClipRed).withAlpha(0x55 / 255.0f));
            g.fillRoundedRectangle(bounds.expanded(4.0f), cornerSize + 2.0f);
            g.setColour(juce::Colour(kClipRed).withAlpha(0x44 / 255.0f));
            g.fillRoundedRectangle(bounds.expanded(2.0f), cornerSize + 1.0f);
            g.setColour(juce::Colour(0xff4a1a1a));
            g.fillRoundedRectangle(bounds, cornerSize);
            g.setColour(juce::Colour(kClipRed));
            g.drawRoundedRectangle(bounds.reduced(0.5f), cornerSize, 1.5f);
        }
        else
        {
            g.setColour(juce::Colour(0x5500aaff));  // ~33% opacity blue glow
            g.fillRoundedRectangle(bounds.expanded(4.0f), cornerSize + 2.0f);
            g.setColour(juce::Colour(0x4400d4ff));  // Inner glow
            g.fillRoundedRectangle(bounds.expanded(2.0f), cornerSize + 1.0f);
            g.setColour(juce::Colour(0xff1a4a5f));  // Dark cyan-blue background
            g.fillRoundedRectangle(bounds, cornerSize);
            g.setColour(juce::Colour(0xff00b4ff));  // Bright blue border
            g.drawRoundedRectangle(bounds.reduced(0.5f), cornerSize, 1.5f);
        }
    }
    else
    {
        // Normal state: dark background
        g.setColour(juce::Colour(0xff1a1a2f));  // Dark background
        g.fillRoundedRectangle(bounds, cornerSize);
        
        // Subtle border
        g.setColour(juce::Colour(0xff3a3a5f));  // Subtle border
        g.drawRoundedRectangle(bounds.reduced(0.5f), cornerSize, 1.0f);
    }
    
    // Draw text
    auto textArea = bounds.reduced(4.0f);
    auto textColour = isToggled ? labelCyan : labelCyan.withAlpha(0.8f);
    
    // Draw text with shadow
    g.setFont(getBodyFont(12.0f, true));
    drawTextWithShadow(g, button.getButtonText(),
                      static_cast<int>(textArea.getX()),
                      static_cast<int>(textArea.getY()),
                      static_cast<int>(textArea.getWidth()),
                      static_cast<int>(textArea.getHeight()),
                      juce::Justification::centred,
                      textColour,
                      isToggled ? 0.3f : 0.2f,
                      1);
}

//==============================================================================
// -- Rotary Slider Drawing (for value boxes) --

void SpaceDustLookAndFeel::drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height,
                                            float sliderPos, float rotaryStartAngle, float rotaryEndAngle,
                                            juce::Slider& slider)
{
    const float half     = (float)juce::jmin(width, height) * 0.5f;
    const float centreX  = (float)x + (float)width  * 0.5f;
    const float centreY  = (float)y + (float)height * 0.5f;
    const float angle    = rotaryStartAngle + sliderPos * (rotaryEndAngle - rotaryStartAngle);

    if (half < 8.0f) return;

    //==========================================================================
    // -- Metered arc knob (Giuseppe, 2026-08-01) --
    // Ported in spirit from Sol Voice Tuner's VolumeArc: no body, no rim, no bloom
    // and no centre dot -- just an arc and a pointer over the background.
    //
    // What differs from Sol, and it is the whole point of this version: Sol's arc
    // length IS the value. Here the value only sets how far the arc COULD reach, and
    // the OUTPUT LEVEL decides how much of that is actually drawn. So each knob doubles
    // as a meter bounded by its own setting -- a knob set low can only ever fill a
    // little, a knob set high has room to move. Nothing is drawn for the rest.
    //
    // The colour is the same blue the arcs have always used (knobArcCyan), and it
    // goes red on clipping through the same meter-responsive helper every other glow
    // site uses, so the whole UI flips together.
    const juce::Colour arcHue = getMeterResponsiveKnobArcColour();

    // Double weight (Giuseppe, 2026-08-01). Both the arc and the pointer, so the dial
    // keeps reading as one object drawn with a single line.
    //
    // The radius is derived FROM the thickness, not the other way round. A stroke is
    // centred on its path, so it reaches half its width beyond the radius -- the first
    // cut of this doubled the thickness while keeping the old radius, and the extra
    // weight promptly pushed the top of the arc outside the component and got clipped
    // flat. Sizing the radius so that (radius + thickness/2) lands inside the bounds is
    // what keeps the curve whole at any weight.
    const float arcThickness = juce::jmax(5.0f, half * 0.20f);
    const float arcRadius    = half - arcThickness * 0.5f - 1.0f;   // 1px breathing room

    // Below this the arc is shorter than it is thick, and drawing it just thickens the
    // pointer's root into a blob (Sol's kMinSweep, same reasoning).
    constexpr float kMinSweep = 0.06f;

    // NO empty track. Nothing is drawn for the unfilled part of the sweep, so at
    // silence the dial is just the pointer and the background shows straight through
    // (Giuseppe, 2026-08-01). The arc exists only as the level draws it in -- which is
    // also Sol's reasoning for having no track: an empty ring sitting there implies a
    // value that has not been set.

    // --- Fill: the output level, bounded by this knob's own value ---
    {
        const float levelAngle = rotaryStartAngle
                               + (angle - rotaryStartAngle) * outputMeterLevel;

        //----------------------------------------------------------------------
        // -- Motion smear on the arc head (Giuseppe, 2026-08-01) --
        // The same RGB dither the meters use, on the other thing in the UI that
        // moves by itself. Drawn BEFORE the arc so the arc lands on its own tail.
        //
        // The head travels along a curve, but the streak is a straight offset, so
        // this smears along the CHORD between where the head was and where it is.
        // Over one frame's travel the difference is not visible.
        //
        // Fewer steps than the meters use: this runs once per knob and there are a
        // lot of knobs, where the meters are two bars.
        {
            constexpr int   kKnobTrailSteps    = 5;
            constexpr float kKnobTrailAlpha    = 0.6f;
            constexpr float kKnobTrailMinSmear = 1.5f;   // px of travel before drawing

            const float lagAngle = rotaryStartAngle
                                 + (angle - rotaryStartAngle) * outputMeterLag;

            auto pointAt = [&](float a)
            {
                return juce::Point<float>(centreX + std::sin(a) * arcRadius,
                                          centreY - std::cos(a) * arcRadius);
            };

            const auto headP = pointAt(levelAngle);
            const auto fromP = pointAt(lagAngle);
            const auto disp  = fromP - headP;

            if (levelAngle - rotaryStartAngle > kMinSweep
                && disp.getDistanceFromOrigin() >= kKnobTrailMinSmear)
            {
                // Only the last stroke-width of arc smears, matching the meters
                // streaking just the top of the bar rather than the whole column.
                const float headSweep = juce::jmin(levelAngle - rotaryStartAngle,
                                                   arcThickness / juce::jmax(1.0f, arcRadius));

                juce::Path headArc;
                headArc.addCentredArc(centreX, centreY, arcRadius, arcRadius, 0.0f,
                                      levelAngle - headSweep, levelAngle, true);

                juce::Path headShape;
                juce::PathStrokeType(arcThickness, juce::PathStrokeType::curved,
                                     juce::PathStrokeType::butt)
                    .createStrokedPath(headShape, headArc);

                SpaceDustDither::streakRgb(g, headShape, disp,
                                           kKnobTrailSteps, kKnobTrailAlpha);
            }
        }

        if (levelAngle - rotaryStartAngle > kMinSweep)
        {
            juce::Path fill;
            fill.addCentredArc(centreX, centreY, arcRadius, arcRadius, 0.0f,
                               rotaryStartAngle, levelAngle, true);

            // Bloom behind the arc, scaled by the meter. Two widening passes at low
            // alpha rather than a real blur -- cheap, and at these stroke weights it
            // reads the same.
            if (const float glow = getGlowAmount(); glow > 0.01f)
            {
                for (int pass = 0; pass < 2; ++pass)
                {
                    const float widen = arcThickness * (pass == 0 ? 2.4f : 1.6f);
                    const float a     = glow * (pass == 0 ? 0.16f : 0.28f);

                    g.setColour(arcHue.withAlpha(a));
                    g.strokePath(fill, juce::PathStrokeType(widen, juce::PathStrokeType::curved,
                                                            juce::PathStrokeType::butt));
                }
            }

            g.setColour(arcHue);
            // Butt caps, not rounded (Giuseppe, 2026-08-01): the pointer is drawn with
            // drawLine, which is square-ended, so a rounded arc cap made the two read as
            // different tools. Flat ends match.
            g.strokePath(fill, juce::PathStrokeType(arcThickness, juce::PathStrokeType::curved,
                                                    juce::PathStrokeType::butt));
        }
    }

    // --- Pointer: centre out to the rim, so it reads as a hand on a dial ---
    // This is the only thing that shows the value when the synth is silent, so it is
    // drawn at full strength regardless of level.
    {
        // Exactly the arc's weight, not its own (Giuseppe, 2026-08-01) -- two
        // different widths made them read as two separate marks.
        const float pointerThickness = arcThickness;

        const float sinA = std::sin(angle);
        const float cosA = std::cos(angle);

        // Runs out to the arc stroke's OUTER edge, not its centreline. drawLine is
        // flat-ended, so landing the tip at (arcRadius + thickness/2) puts that flat
        // end exactly tangent to the outside of the arc's circle -- the two finish
        // flush instead of the pointer stopping half a stroke short.
        const float tipRadius = arcRadius + arcThickness * 0.5f;

        const juce::Line<float> pointerLine { centreX, centreY,
                                              centreX + sinA * tipRadius,
                                              centreY - cosA * tipRadius };

        // Same bloom law as the arc, so the two halves of the dial glow together.
        if (const float glow = getGlowAmount(); glow > 0.01f)
        {
            for (int pass = 0; pass < 2; ++pass)
            {
                g.setColour(arcHue.withAlpha(glow * (pass == 0 ? 0.16f : 0.28f)));
                g.drawLine(pointerLine, pointerThickness * (pass == 0 ? 2.4f : 1.6f));
            }
        }

        g.setColour(arcHue);
        g.drawLine(pointerLine, pointerThickness);
    }
}

//==============================================================================
// -- Custom Text Drawing Helpers --

void SpaceDustLookAndFeel::drawTextWithShadow(juce::Graphics& g, const juce::String& text,
                                              int x, int y, int width, int height,
                                              juce::Justification justification,
                                              juce::Colour textColour,
                                              float shadowOpacity,
                                              int shadowOffset)
{
    auto area = juce::Rectangle<int>(x, y, width, height);
    
    // Draw shadow
    g.setColour(shadowBlack.withAlpha(shadowOpacity));
    g.drawText(text, area.translated(shadowOffset, shadowOffset), justification, false);
    
    // Draw main text
    g.setColour(textColour);
    g.drawText(text, area, justification, false);
}

void SpaceDustLookAndFeel::drawTextWithGlow(juce::Graphics& g, const juce::String& text,
                                            int x, int y, int width, int height,
                                            juce::Justification justification,
                                            juce::Colour textColour,
                                            juce::Colour glowColour,
                                            float glowIntensity)
{
    // This used to ignore glowColour and glowIntensity entirely and just draw a
    // shadow -- the name promised a glow that was never implemented. It honours both
    // now, on top of the meter-driven amount, so callers can ask for a hotter or
    // cooler halo than the default without breaking the shared response.
    const juce::Rectangle<int> area(x, y, width, height);

    if (glowIntensity > 0.0f)
        glowText(g, text, area, justification,
                 glowColour.withMultipliedAlpha(juce::jlimit(0.0f, 4.0f, glowIntensity)));

    drawTextWithShadow(g, text, x, y, width, height, justification, textColour, 0.25f, 1);
}

//==============================================================================
// -- Title Font (Glitch Goblin) / Body Font (standardized 12pt) --

juce::Font SpaceDustLookAndFeel::getTitleFont(float height) const
{
    if (glitchGoblinTypeface != nullptr)
        return juce::Font(glitchGoblinTypeface).withHeight(height);
    return juce::Font(juce::FontOptions(height, juce::Font::bold));
}

juce::Font SpaceDustLookAndFeel::getBodyFont(float height, bool bold) const
{
    // Arial/Arial Bold are universally available and render a clear bold weight
    // (Times New Roman was tried 2026-08-01 and rejected.)
    const char* typeface = "Arial";
    return juce::Font(juce::FontOptions(typeface, height, bold ? juce::Font::bold : juce::Font::plain));
}

juce::Font SpaceDustLookAndFeel::getLabelFont(juce::Label&)
{
    return getBodyFont(12.0f, true);
}

juce::Font SpaceDustLookAndFeel::getComboBoxFont(juce::ComboBox&)
{
    return getBodyFont(12.0f, true);
}

juce::Font SpaceDustLookAndFeel::getTabButtonFont(juce::TabBarButton&, float height)
{
    return getBodyFont(height, true);
}


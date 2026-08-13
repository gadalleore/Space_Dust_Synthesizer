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

juce::Colour SpaceDustLookAndFeel::getMeterResponsiveTraceColour() const
{
    return outputMeterClipping ? juce::Colour(kClipRed) : juce::Colour(kTraceBlue);
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

    // Bloom outside the box, before its own fill goes down over the inner half.
    // Uses the OUTLINE colour rather than the fill, so the light looks like it comes
    // off the edge that is actually drawn.
    glowAround(g, boxBounds.toFloat(), cornerSize,
               outputMeterClipping ? juce::Colour(kClipRed)
                                   : box.findColour(juce::ComboBox::outlineColourId));

    g.setColour(box.findColour(juce::ComboBox::backgroundColourId));
    g.fillRoundedRectangle(boxBounds.toFloat(), cornerSize);

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
    juce::ignoreUnused(shouldDrawButtonAsHighlighted, shouldDrawButtonAsDown);

    drawToggleStyleButton(g, button.getLocalBounds().toFloat(), button.getButtonText(),
                          button.getToggleState());
}

void SpaceDustLookAndFeel::drawToggleStyleButton(juce::Graphics& g, juce::Rectangle<float> bounds,
                                                 const juce::String& text, bool isLit, bool isEnabled)
{
    auto cornerSize = 3.0f;

    // A button that cannot be pressed never lights, whatever the pointer is doing
    // over it.
    const bool isToggled = isLit && isEnabled;

    // Meter-driven bloom OUTSIDE the button, drawn first so the button's own fills
    // cover its inner half and only the outward spill survives. A dead control
    // does not bloom -- glow is the panel saying a thing is live.
    if (isEnabled)
        glowAround(g,
                   bounds.expanded(isToggled ? 4.0f : 0.5f),
                   cornerSize + (isToggled ? 2.0f : 0.0f),
                   outputMeterClipping ? juce::Colour(kClipRed)
                                       : juce::Colour(isToggled ? 0xff00d4ff : 0xff3a3a5f));

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

        // Subtle border, dropped back further again when the control is dead
        g.setColour(juce::Colour(0xff3a3a5f).withAlpha(isEnabled ? 1.0f : 0.45f));
        g.drawRoundedRectangle(bounds.reduced(0.5f), cornerSize, 1.0f);
    }

    // Draw text
    auto textArea = bounds.reduced(4.0f);
    auto textColour = isToggled ? labelCyan
                                : labelCyan.withAlpha(isEnabled ? 0.8f : 0.35f);
    
    // Draw text with shadow
    g.setFont(getBodyFont(12.0f, true));
    drawTextWithShadow(g, text,
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

    //==========================================================================
    // -- Value arc (Giuseppe, 2026-08-08) --
    // A light-blue arc that always runs from the start of the sweep to the pointer,
    // under the level fill below. Asked for as "a small arc gradient that is light blue
    // on each knob that follows the arc all the way up to the indicator".
    //
    // It stops SHORT of the pointer rather than meeting it -- three quarters of the way
    // along the sweep (Giuseppe, 2026-08-08), so there is always a gap of background
    // between the arc's head and the pointer and the whole dial reads more quietly.
    //
    // It cannot overrun the pointer at any value: the reach is a fraction OF the
    // pointer's own sweep, so the two are derived from one number rather than kept in
    // sync. Turn the knob down and the arc shortens with it, gap and all.
    //
    // This supersedes the old "no empty track" rule (2026-08-01), which left a silent
    // dial as nothing but a pointer. The track is not empty now -- it shows the value --
    // so the objection to it (an empty ring implying a value that was never set) does
    // not apply. The level fill still draws over the top, so each knob reads as a
    // setting with a meter running along inside it.
    //
    // The gradient runs between the two ENDS of the arc rather than across the knob's
    // bounds, so it always tracks the arc's own direction however far round it sweeps.
    // How far along the pointer's sweep the arc runs. 1.0 would touch the pointer; this
    // is the one number to change if the gap wants to be wider or narrower.
    constexpr float kValueArcReach = 0.75f;

    const float valueArcEnd = rotaryStartAngle
                            + (angle - rotaryStartAngle) * kValueArcReach;

    if (valueArcEnd - rotaryStartAngle > kMinSweep)
    {
        juce::Path valueArc;
        valueArc.addCentredArc(centreX, centreY, arcRadius, arcRadius, 0.0f,
                               rotaryStartAngle, valueArcEnd, true);

        auto onArc = [&](float a)
        {
            return juce::Point<float>(centreX + std::sin(a) * arcRadius,
                                      centreY - std::cos(a) * arcRadius);
        };

        // The gradient ends where the ARC does, not where the pointer is, so the fade
        // completes over the length actually drawn rather than being cut off mid-ramp.
        const auto tailP = onArc(rotaryStartAngle);
        const auto headP = onArc(valueArcEnd);

        // Brightest at the tail, fading out towards the pointer (Giuseppe, 2026-08-08:
        // "dark at the top and light at the bottom"). The sweep starts at the bottom
        // left and climbs over the top, so the light end is the bottom of the dial and
        // the arc dims as it rises -- the pointer alone marks the value, and the arc
        // falls away behind it instead of racing it to be the brightest thing.
        //
        // The far stop is FULLY TRANSPARENT, which is what makes the arc taper out
        // instead of stopping (Giuseppe, 2026-08-08). Ending on a visible alpha left a
        // butt cap sitting there as a hard little edge; at zero there is nothing to cap.
        //
        // The intermediate stops bend the falloff so it holds its brightness through the
        // first third and then drops away increasingly fast. A straight ramp to zero
        // reads as a uniform wash -- the acceleration is what makes it look like it is
        // dissolving rather than merely being dimmed.
        //
        // arcHue is the POINTER's own colour, so the arc leaves the bottom of the dial
        // as the same blue the pointer is drawn in and dissolves from there (Giuseppe,
        // 2026-08-08). Using the shared hue rather than a colour of its own also means
        // the arc follows the pointer red on clipping, so the dial still flips as one
        // object the way every other glow site does.
        //
        // Separation from the level fill is carried by ALPHA alone now, not by hue:
        // the fill is drawn at full strength over the top, this peaks at 0.85 and falls
        // away, so it stays the quiet layer without needing to be a different colour.
        juce::ColourGradient grad(arcHue.withAlpha(0.85f), tailP,
                                  arcHue.withAlpha(0.0f),  headP, false);
        grad.addColour(0.35, arcHue.withAlpha(0.55f));
        grad.addColour(0.65, arcHue.withAlpha(0.24f));
        grad.addColour(0.85, arcHue.withAlpha(0.07f));

        g.setGradientFill(grad);
        g.strokePath(valueArc, juce::PathStrokeType(arcThickness,
                                                    juce::PathStrokeType::curved,
                                                    juce::PathStrokeType::butt));
    }

    // --- Fill: the output level, bounded by this knob's own value ---
    {
        const float levelAngle = rotaryStartAngle
                               + (angle - rotaryStartAngle) * outputMeterLevel;

        //----------------------------------------------------------------------
        // -- Motion smear on the arc head (Giuseppe, 2026-08-01) --
        // The same RGB dither the meters use, on the other thing in the UI that
        // moves by itself. Drawn BEFORE the arc so the arc lands on its own tail.
        //
        // The head travels along a curve and the streak follows it, stamp by
        // stamp, about the dial's own centre.
        //
        // It used to smear along the CHORD between where the head was and where
        // it is, on the grounds that over one frame's travel the difference is
        // not visible. That holds only while the travel is short. The lag
        // follower can sit a long way behind the level when a note starts, and
        // the chord between two widely separated points on a circle passes close
        // to its centre -- so the smear was drawn straight across the face of the
        // dial, on every knob at once, since they all move off the one shared
        // level (Giuseppe, 2026-08-12).
        //
        // Fewer steps than the meters use: this runs once per knob and there are a
        // lot of knobs, where the meters are two bars.
        {
            // These match StereoLevelMeterComponent's kTrailSteps / kTrailAlpha exactly
            // (Giuseppe, 2026-08-08). They were 5 and 0.6 before, to save work: the
            // smear runs one time for each knob, and there are approximately 50 knobs,
            // where the meter has two bars. The result was that the arcs smeared more
            // weakly than the meters they sit beside. Keep these two numbers equal to
            // the meter's, or the two stop agreeing again.
            constexpr int   kKnobTrailSteps    = 9;
            constexpr float kKnobTrailAlpha    = 0.75f;

            // NOT matched to the meter's 2.5px, and this one is deliberate. The meter
            // bar travels the full height of the plate; an arc head travels the length
            // of one small curve. The same threshold in pixels is a much larger part of
            // an arc's possible travel, so it would cut off most knob smears.
            constexpr float kKnobTrailMinSmear = 1.5f;   // px of travel before drawing

            const float lagAngle = rotaryStartAngle
                                 + (angle - rotaryStartAngle) * outputMeterLag;

            // How far the head has travelled, measured ALONG the arc rather than
            // across the chord. The two agree for small movements and diverge for
            // large ones, and it is the arc that the stamps now follow, so the
            // arc is what the threshold has to be tested against.
            const float sweep     = lagAngle - levelAngle;
            const float travelPx  = std::abs(sweep) * arcRadius;

            if (levelAngle - rotaryStartAngle > kMinSweep
                && travelPx >= kKnobTrailMinSmear)
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

                SpaceDustDither::streakRgbArc(g, headShape, { centreX, centreY }, sweep,
                                              kKnobTrailSteps, kKnobTrailAlpha, *ditherTiles);
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

        // Runs out to the arc stroke's OUTER edge, not its centreline, so at full
        // value the tip lands exactly on the outer corner of the arc's flat end and
        // the two meet at a single point (Giuseppe, 2026-08-01).
        const float tipRadius = arcRadius + arcThickness * 0.5f;

        //----------------------------------------------------------------------
        // An OBELISK (Giuseppe, 2026-08-01), not a taper: a round bottom, two
        // PARALLEL sides, and a 45-45-90 triangle capping the top.
        //
        // A 90-degree apex fixes the cap's height: half-widths on both sides at
        // 45 degrees means the point stands exactly one half-width above the
        // shoulder. So with sides at +/-r the shoulder is at (tip - r), and there
        // is nothing to choose -- the shape falls out of the width.
        //
        // Built in the pointer's own frame and mapped out: `u` runs along the
        // pointer from the pivot, `v` across it. Much easier to reason about than
        // rotating six points by hand.
        const float r = pointerThickness * 0.5f;

        const float sinA = std::sin(angle);
        const float cosA = std::cos(angle);

        auto pt = [&](float u, float v)
        {
            return juce::Point<float>(centreX + sinA * u + cosA * v,
                                      centreY - cosA * u + sinA * v);
        };

        // Parameterised on where the apex lands, because the glow needs a SHORTER
        // obelisk than the fill -- see the glow passes below.
        auto buildNeedle = [&](float apexRadius)
        {
            juce::Path p;
            const float shoulder = apexRadius - r;

            if (shoulder <= 0.0f)
            {
                // Degenerate on a tiny knob: the cap alone would fill the whole length,
                // so there is no shaft to draw. Just the pivot dot.
                p.addEllipse(centreX - r, centreY - r, r * 2.0f, r * 2.0f);
                return p;
            }

            constexpr float halfPi = juce::MathConstants<float>::halfPi;

            p.startNewSubPath(pt(0.0f, r));

            // The round bottom: a half-turn behind the pivot, from one side of the
            // shaft to the other. The parallel sides are tangent to it at u = 0, so
            // the shaft meets the curve with no shoulder.
            p.addCentredArc(centreX, centreY, r, r, 0.0f,
                            angle + halfPi, angle + halfPi * 3.0f, false);

            p.lineTo(pt(shoulder,    -r));   // up one parallel side
            p.lineTo(pt(apexRadius, 0.0f));  // the point
            p.lineTo(pt(shoulder,     r));   // back down the other
            p.closeSubPath();
            return p;
        };

        // Same bloom law as the arc, so the two halves of the dial glow together.
        // Stroked around the filled shape rather than drawn as a fatter line, so the
        // halo follows the taper instead of squaring it off.
        if (const float glow = getGlowAmount(); glow > 0.01f)
        {
            for (int pass = 0; pass < 2; ++pass)
            {
                // Each pass is stroked on a needle whose apex is pulled back by exactly
                // half that pass's stroke width, so the stroke's OUTER boundary lands on
                // tipRadius rather than beyond it.
                //
                // Without this the arrow visibly overshot the arc (Giuseppe, 2026-08-02).
                // A stroke is centred on its path, and the rounded join at a 90-degree
                // apex is a half-disc centred on the point -- so the glow spilled
                // 0.7 * thickness straight out along the pointer's axis. The arc's glow
                // spreads the same distance but spreads it evenly along the whole curve,
                // where it reads as a halo; concentrated at a single point it read as the
                // tip sticking out past the rim.
                const float strokeWidth = pointerThickness * (pass == 0 ? 1.4f : 0.6f);

                g.setColour(arcHue.withAlpha(glow * (pass == 0 ? 0.16f : 0.28f)));
                g.strokePath(buildNeedle(tipRadius - strokeWidth * 0.5f),
                             juce::PathStrokeType(strokeWidth,
                                                  juce::PathStrokeType::curved,
                                                  juce::PathStrokeType::rounded));
            }
        }

        g.setColour(arcHue);
        g.fillPath(buildNeedle(tipRadius));
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


//==============================================================================
// -- Starfield --
//
// Lives here rather than in the editor because it is the plugin background, and
// more than one window needs it: the main panel paints it behind the controls,
// and the Waveforms window paints it behind its list. It was private to
// PluginEditor.cpp until that second window asked for it; the editor now calls
// through to this, so there is still one catalogue and one projection.
// Astronomically accurate starfield: Costa Mesa, CA (33.66Â°N, 117.90Â°W)
// March 29, 2026 at midnight PDT â€” commemorating the completion of Space Dust.
// Star positions from Yale Bright Star Catalog (J2000.0 epoch), projected via
// horizontal coordinate transform for the exact date/time/location.
// Bortle 7-8 sky (moderate light pollution): ~mag 4.0 naked-eye limit.
void SpaceDustLookAndFeel::drawStarfield(juce::Graphics& g, int w, int h, float meterLevel)
{
    if (w <= 0 || h <= 0) return;
    // Stars breathe with the audio: up to 15% brighter at full level
    const float meterBoost = 1.0f + 0.15f * juce::jlimit(0.0f, 1.0f, meterLevel);

    // Star catalog: {RA (degrees), Dec (degrees), apparent magnitude}
    struct CatStar { float ra, dec, mag; };
    static const CatStar catalog[] = {
        // -- Orion (setting in west) --
        {88.79f,   7.41f, 0.50f},   // Betelgeuse (Î± Ori)
        {78.63f,  -8.20f, 0.13f},   // Rigel (Î² Ori)
        {81.28f,   6.35f, 1.64f},   // Bellatrix (Î³ Ori)
        {83.00f,  -0.30f, 2.23f},   // Mintaka (Î´ Ori)
        {84.05f,  -1.20f, 1.69f},   // Alnilam (Îµ Ori)
        {85.19f,  -1.94f, 1.77f},   // Alnitak (Î¶ Ori)
        {86.94f,  -9.67f, 2.09f},   // Saiph (Îº Ori)
        // -- Canis Major --
        {101.29f,-16.72f,-1.46f},   // Sirius (Î± CMa)
        {95.68f, -17.96f, 1.98f},   // Mirzam (Î² CMa)
        {107.10f,-26.39f, 1.84f},   // Adhara (Îµ CMa)
        {104.66f,-28.97f, 1.50f},   // Wezen (Î´ CMa)
        // -- Canis Minor --
        {114.83f,  5.22f, 0.34f},   // Procyon (Î± CMi)
        {111.79f,  8.29f, 2.90f},   // Gomeisa (Î² CMi)
        // -- Gemini --
        {116.33f, 28.03f, 1.14f},   // Pollux (Î² Gem)
        {113.65f, 31.89f, 1.58f},   // Castor (Î± Gem)
        {99.43f,  16.40f, 1.93f},   // Alhena (Î³ Gem)
        {95.74f,  22.51f, 2.88f},   // Tejat (Î¼ Gem)
        {100.98f, 25.13f, 2.98f},   // Mebsuta (Îµ Gem)
        {110.03f, 21.98f, 3.53f},   // Wasat (Î´ Gem)
        // -- Auriga --
        {79.17f,  46.00f, 0.08f},   // Capella (Î± Aur)
        {89.88f,  44.95f, 1.90f},   // Menkalinan (Î² Aur)
        // -- Taurus --
        {68.98f,  16.51f, 0.85f},   // Aldebaran (Î± Tau)
        {81.57f,  28.61f, 1.65f},   // Elnath (Î² Tau)
        // -- Leo --
        {152.09f, 11.97f, 1.35f},   // Regulus (Î± Leo)
        {177.27f, 14.57f, 2.14f},   // Denebola (Î² Leo)
        {154.99f, 19.84f, 2.28f},   // Algieba (Î³ Leo)
        {168.53f, 20.52f, 2.56f},   // Zosma (Î´ Leo)
        {168.56f, 15.43f, 3.34f},   // Chertan (Î¸ Leo)
        {154.17f, 23.42f, 3.44f},   // Adhafera (Î¶ Leo)
        {151.83f, 16.76f, 3.52f},   // Î· Leo
        // -- Virgo --
        {201.30f,-11.16f, 0.97f},   // Spica (Î± Vir)
        {190.42f, -1.45f, 2.74f},   // Porrima (Î³ Vir)
        {195.54f, 10.96f, 2.83f},   // Vindemiatrix (Îµ Vir)
        // -- BoÃ¶tes --
        {213.92f, 19.18f,-0.05f},   // Arcturus (Î± Boo)
        {221.25f, 27.07f, 2.70f},   // Izar (Îµ Boo)
        {208.67f, 18.40f, 2.68f},   // Muphrid (Î· Boo)
        {218.02f, 38.31f, 3.03f},   // Seginus (Î³ Boo)
        {225.49f, 40.39f, 3.50f},   // Nekkar (Î² Boo)
        // -- Ursa Major (Big Dipper high overhead) --
        {165.93f, 61.75f, 1.79f},   // Dubhe (Î± UMa)
        {165.46f, 56.38f, 2.37f},   // Merak (Î² UMa)
        {178.46f, 53.69f, 2.44f},   // Phecda (Î³ UMa)
        {183.86f, 57.03f, 3.31f},   // Megrez (Î´ UMa)
        {193.51f, 55.96f, 1.77f},   // Alioth (Îµ UMa)
        {200.98f, 54.93f, 2.27f},   // Mizar (Î¶ UMa)
        {206.89f, 49.31f, 1.86f},   // Alkaid (Î· UMa)
        {155.58f, 41.50f, 3.05f},   // Tania Australis (Î¼ UMa)
        {154.27f, 42.91f, 3.45f},   // Tania Borealis (Î» UMa)
        {169.62f, 33.09f, 3.49f},   // Alula Borealis (Î½ UMa)
        {169.55f, 31.53f, 3.79f},   // Alula Australis (Î¾ UMa)
        {201.31f, 54.99f, 4.01f},   // Alcor (80 UMa)
        // -- Ursa Minor --
        {37.95f,  89.26f, 1.98f},   // Polaris (Î± UMi)
        {222.68f, 74.16f, 2.08f},   // Kochab (Î² UMi)
        {230.18f, 71.83f, 3.05f},   // Pherkad (Î³ UMi)
        // -- Hydra --
        {141.90f, -8.66f, 1.98f},   // Alphard (Î± Hya)
        // -- Cancer --
        {124.13f,  9.19f, 3.52f},   // Al Tarf (Î² Cnc)
        {131.17f, 18.15f, 3.94f},   // Asellus Australis (Î´ Cnc)
        // -- Corvus --
        {183.95f,-17.54f, 2.59f},   // Gienah (Î³ Crv)
        {188.60f,-23.40f, 2.65f},   // Kraz (Î² Crv)
        {187.47f,-16.52f, 2.95f},   // Algorab (Î´ Crv)
        {182.53f,-22.62f, 3.02f},   // Minkar (Îµ Crv)
        // -- Canes Venatici --
        {194.01f, 38.32f, 2.90f},   // Cor Caroli (Î± CVn)
        // -- Corona Borealis --
        {233.67f, 26.71f, 2.23f},   // Alphecca (Î± CrB)
        // -- Draco --
        {269.15f, 51.49f, 2.23f},   // Eltanin (Î³ Dra)
        {262.61f, 52.30f, 2.79f},   // Rastaban (Î² Dra)
        {211.10f, 64.38f, 3.65f},   // Thuban (Î± Dra)
        // -- Hercules --
        {247.55f, 21.49f, 2.77f},   // Kornephoros (Î² Her)
        {250.32f, 31.60f, 2.81f},   // Î¶ Her
        {258.76f, 36.81f, 3.16f},   // Ï€ Her
        {258.76f, 24.84f, 3.14f},   // Sarin (Î´ Her)
        {258.66f, 14.39f, 3.48f},   // Rasalgethi (Î± Her)
        {266.62f, 27.72f, 3.42f},   // Î¼ Her
        {250.72f, 38.92f, 3.53f},   // Î· Her
        // -- Serpens --
        {236.07f,  6.43f, 2.65f},   // Unukalhai (Î± Ser)
        // -- Libra --
        {222.72f,-16.04f, 2.75f},   // Zubenelgenubi (Î± Lib)
        {229.25f, -9.38f, 2.61f},   // Zubeneschamali (Î² Lib)
        // -- Lyra --
        {279.23f, 38.78f, 0.03f},   // Vega (Î± Lyr)
        // -- Cygnus --
        {310.36f, 45.28f, 1.25f},   // Deneb (Î± Cyg)
        // -- Centaurus --
        {211.67f,-36.37f, 2.06f},   // Menkent (Î¸ Cen)
        // -- Perseus (low NW) --
        {51.08f,  49.86f, 1.80f},   // Mirfak (Î± Per)
        // -- Lynx --
        {140.26f, 34.39f, 3.13f},   // Î± Lyn
        // -- Leo Minor --
        {163.33f, 34.22f, 3.83f},   // Praecipua (46 LMi)
        // -- Cepheus --
        {319.65f, 62.59f, 2.51f},   // Alderamin (Î± Cep)
        // -- Ophiuchus --
        {263.73f, 12.56f, 2.08f},   // Rasalhague (Î± Oph)
        // -- Monoceros --
        {107.99f, -0.49f, 3.93f},   // Î± Mon
        // -- Cassiopeia (low north) --
        {10.13f,  56.54f, 2.23f},   // Schedar (Î± Cas)
        {2.29f,   59.15f, 2.27f},   // Caph (Î² Cas)
    };
    static const int catalogSize = sizeof(catalog) / sizeof(catalog[0]);

    // Projected star cache (computed once, reused every paint)
    struct ProjStar { float nx, ny, brightness; };
    static ProjStar projected[200];
    static int projCount = 0;
    static bool computed = false;

    if (!computed)
    {
        computed = true;
        projCount = 0;

        // Costa Mesa, CA: 33.6646Â°N, 117.9034Â°W
        // March 29, 2026 midnight PDT (UTC-7) = 07:00 UTC
        // GMST at 0h UTC: 188.81Â° + 7h sidereal rotation (105.29Â°) = 294.10Â°
        // LST = GMST + longitude = 294.10 + (-117.90) = 176.20Â°
        constexpr float lat       = 33.6646f;
        constexpr float lst       = 176.20f;
        constexpr float degToRad  = 3.14159265f / 180.0f;
        constexpr float minAlt    = 5.0f;    // horizon cutoff (obstructions + haze)
        constexpr float maxMag    = 4.2f;    // Bortle 7-8 naked-eye limit
        constexpr float minMag    = -1.5f;
        const float sinLat = std::sin(lat * degToRad);
        const float cosLat = std::cos(lat * degToRad);
        const float sinMinAlt = std::sin(minAlt * degToRad);

        for (int i = 0; i < catalogSize && projCount < 200; ++i)
        {
            const auto& s = catalog[i];
            if (s.mag > maxMag) continue;

            float ha     = (lst - s.ra) * degToRad;
            float sinDec = std::sin(s.dec * degToRad);
            float cosDec = std::cos(s.dec * degToRad);

            // Altitude
            float sinAlt = sinDec * sinLat + cosDec * cosLat * std::cos(ha);
            if (sinAlt < sinMinAlt) continue;  // below horizon / obstructed

            float alt    = std::asin(sinAlt) / degToRad;
            float cosAlt = std::cos(alt * degToRad);
            if (cosAlt < 0.001f) cosAlt = 0.001f;  // zenith guard

            // Azimuth (0Â°=N, 90Â°=E, 180Â°=S, 270Â°=W)
            float sinAz = -cosDec * std::sin(ha) / cosAlt;
            float cosAz = (sinDec - sinLat * sinAlt) / (cosLat * cosAlt);
            float az    = std::atan2(sinAz, cosAz) / degToRad;
            if (az < 0.0f) az += 360.0f;

            // Cylindrical equidistant projection to normalized coords
            float nx = az / 360.0f;
            float ny = 1.0f - (alt / 90.0f);

            // Brightness from magnitude (linear in perceptual range)
            float brightness = (maxMag - s.mag) / (maxMag - minMag);
            brightness = juce::jlimit(0.0f, 1.0f, brightness);

            projected[projCount++] = { nx, ny, brightness };
        }
    }

    // Draw catalog stars
    for (int i = 0; i < projCount; ++i)
    {
        const auto& s = projected[i];
        float alpha  = (60.0f + 130.0f * s.brightness) * meterBoost;
        float radius = (0.6f + 1.5f * s.brightness) * meterBoost;
        g.setColour(juce::Colour(255, 255, 255).withAlpha(
            static_cast<juce::uint8>(juce::jlimit(0, 255, static_cast<int>(alpha)))));
        float px = s.nx * static_cast<float>(w);
        float py = s.ny * static_cast<float>(h);
        g.fillEllipse(px - radius, py - radius, radius * 2.0f, radius * 2.0f);
    }

    // Faint filler stars (threshold-of-visibility, deterministic positions)
    for (int i = 0; i < 35; ++i)
    {
        unsigned int hash = static_cast<unsigned int>((i + 500) * 2654435761u);
        float sx = static_cast<float>(hash % static_cast<unsigned int>(w));
        hash = (hash >> 13) ^ (hash * 1597334677u);
        float sy = static_cast<float>(hash % static_cast<unsigned int>(h));
        hash = (hash >> 7) ^ (hash * 2246822519u);
        juce::uint8 alpha = static_cast<juce::uint8>(juce::jlimit(0, 255,
            static_cast<int>((22 + (hash % 26)) * meterBoost)));
        g.setColour(juce::Colour(255, 255, 255).withAlpha(alpha));
        g.fillEllipse(sx - 0.5f, sy - 0.5f, 1.0f, 1.0f);
    }
}

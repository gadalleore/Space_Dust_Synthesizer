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
                                                 const juce::String& text, bool isLit)
{
    auto cornerSize = 3.0f;

    const bool isToggled = isLit;

    // Meter-driven bloom OUTSIDE the button, drawn first so the button's own fills
    // cover its inner half and only the outward spill survives.
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
        
        // Subtle border
        g.setColour(juce::Colour(0xff3a3a5f));  // Subtle border
        g.drawRoundedRectangle(bounds.reduced(0.5f), cornerSize, 1.0f);
    }
    
    // Draw text
    auto textArea = bounds.reduced(4.0f);
    auto textColour = isToggled ? labelCyan : labelCyan.withAlpha(0.8f);
    
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
        // The head travels along a curve, but the streak is a straight offset, so
        // this smears along the CHORD between where the head was and where it is.
        // Over one frame's travel the difference is not visible.
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


#include "SpaceDustLookAndFeel.h"
#include "BinaryData.h"
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
#if JUCE_WINDOWS
    setDefaultSansSerifTypefaceName("Segoe UI");
#elif JUCE_MAC
    setDefaultSansSerifTypefaceName("Lucida Grande");
#else
    setDefaultSansSerifTypefaceName("Sans");
#endif
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
        
        // Draw subtle shadow (20% opacity, 1px offset)
        g.setColour(shadowBlack);
        g.setFont(font);
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
        const juce::Colour viewportBlue(0xff00b4ff);
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
            g.setColour(viewportBlue.withAlpha(alpha));
            g.fillPath(glowPath);
        }
    }

    juce::Colour outlineCol = group.findColour(juce::GroupComponent::outlineColourId)
                              .withMultipliedAlpha(group.isEnabled() ? 1.0f : 0.5f);
    if (isActive)
        outlineCol = juce::Colour(0xff60d4ff);   // Brighter cyan when effect/LFO on
    else if (viewportGlow)
        outlineCol = juce::Colour(0xff00b4ff);  // Bright blue for viewport
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
    
    // Background: glow effect when checked (brighter blue/cyan)
    if (isToggled)
    {
        // Draw outer glow with bright blue
        g.setColour(juce::Colour(0x5500aaff));  // ~33% opacity blue glow
        g.fillRoundedRectangle(bounds.expanded(4.0f), cornerSize + 2.0f);
        g.setColour(juce::Colour(0x4400d4ff));  // Inner glow
        g.fillRoundedRectangle(bounds.expanded(2.0f), cornerSize + 1.0f);
        
        // Main background: brighter cyan when checked
        g.setColour(juce::Colour(0xff1a4a5f));  // Dark cyan-blue background
        g.fillRoundedRectangle(bounds, cornerSize);
        
        // Border: bright blue when checked
        g.setColour(juce::Colour(0xff00b4ff));  // Bright blue border
        g.drawRoundedRectangle(bounds.reduced(0.5f), cornerSize, 1.5f);
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
    const float radius   = (float)juce::jmin(width, height) * 0.5f - 4.0f;
    const float centreX  = (float)x + (float)width  * 0.5f;
    const float centreY  = (float)y + (float)height * 0.5f;
    const float angle    = rotaryStartAngle + sliderPos * (rotaryEndAngle - rotaryStartAngle);

    if (radius < 6.0f) return;

    // --- Outer glow (soft bloom behind the knob) ---
    {
        const float glowRadius = radius + 6.0f;
        juce::ColourGradient glow(knobGlowCyan.withAlpha((juce::uint8)30), centreX, centreY,
                                  knobGlowCyan.withAlpha((juce::uint8)0),  centreX, centreY - glowRadius, true);
        g.setGradientFill(glow);
        g.fillEllipse(centreX - glowRadius, centreY - glowRadius, glowRadius * 2.0f, glowRadius * 2.0f);
    }

    // --- Knob body (radial gradient for 3-D look) ---
    {
        juce::ColourGradient body(knobBodyLight, centreX, centreY - radius * 0.35f,
                                  knobBodyDark,  centreX, centreY + radius * 0.8f, false);
        g.setGradientFill(body);
        g.fillEllipse(centreX - radius, centreY - radius, radius * 2.0f, radius * 2.0f);
    }

    // --- Rim / bevel ring ---
    {
        const float rimThickness = juce::jmax(1.5f, radius * 0.06f);
        juce::ColourGradient rim(knobRimLight, centreX, centreY - radius,
                                 knobRimDark,  centreX, centreY + radius, false);
        g.setGradientFill(rim);
        g.drawEllipse(centreX - radius, centreY - radius, radius * 2.0f, radius * 2.0f, rimThickness);
    }

    // --- Value arc (glowing cyan sweep from start to current position) ---
    {
        const float arcRadius = radius + 2.0f;
        const float arcThickness = juce::jmax(2.5f, radius * 0.09f);

        // Glow layer behind the arc
        juce::Path glowArc;
        glowArc.addCentredArc(centreX, centreY, arcRadius, arcRadius, 0.0f,
                              rotaryStartAngle, angle, true);
        g.setColour(knobGlowCyan.withAlpha((juce::uint8)50));
        g.strokePath(glowArc, juce::PathStrokeType(arcThickness + 4.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        // Crisp arc on top
        juce::Path valueArc;
        valueArc.addCentredArc(centreX, centreY, arcRadius, arcRadius, 0.0f,
                               rotaryStartAngle, angle, true);
        g.setColour(knobArcCyan);
        g.strokePath(valueArc, juce::PathStrokeType(arcThickness, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    // --- Background track (faint arc for the unset portion) ---
    {
        const float trackRadius = radius + 2.0f;
        const float trackThickness = juce::jmax(1.5f, radius * 0.05f);
        juce::Path track;
        track.addCentredArc(centreX, centreY, trackRadius, trackRadius, 0.0f,
                            angle, rotaryEndAngle, true);
        g.setColour(knobRimDark.withAlpha(0.4f));
        g.strokePath(track, juce::PathStrokeType(trackThickness, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    // --- Pointer / indicator line ---
    {
        const float pointerLength  = radius * 0.55f;
        const float pointerThickness = juce::jmax(2.0f, radius * 0.07f);
        juce::Path pointer;
        pointer.addRoundedRectangle(-pointerThickness * 0.5f, -radius + 4.0f,
                                     pointerThickness, pointerLength, pointerThickness * 0.5f);
        pointer.applyTransform(juce::AffineTransform::rotation(angle).translated(centreX, centreY));
        g.setColour(juce::Colour(0xffe8f4ff));
        g.fillPath(pointer);
    }

    // --- Centre dot highlight ---
    {
        const float dotRadius = juce::jmax(2.0f, radius * 0.1f);
        juce::ColourGradient dot(juce::Colour(0xff4a4a6a), centreX, centreY - dotRadius * 0.5f,
                                 juce::Colour(0xff1a1a30), centreX, centreY + dotRadius, false);
        g.setGradientFill(dot);
        g.fillEllipse(centreX - dotRadius, centreY - dotRadius, dotRadius * 2.0f, dotRadius * 2.0f);
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


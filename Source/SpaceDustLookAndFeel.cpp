#include "SpaceDustLookAndFeel.h"
#include <juce_graphics/juce_graphics.h>

//==============================================================================
// -- Constructor --

SpaceDustLookAndFeel::SpaceDustLookAndFeel()
{
    // Set refined cosmic color scheme - unified palette
    setColour(juce::TextButton::textColourOffId, labelCyan);
    setColour(juce::ComboBox::textColourId, labelCyan);
    setColour(juce::ComboBox::backgroundColourId, juce::Colour(0xff1a1a2f));
    setColour(juce::ComboBox::outlineColourId, juce::Colour(0xff3a3a5f));
    setColour(juce::Slider::textBoxTextColourId, valueCyan);
    setColour(juce::Slider::textBoxBackgroundColourId, juce::Colour(0x33000000));  // Subtle dark background
    setColour(juce::Slider::textBoxOutlineColourId, juce::Colour(0x22000000));
}

//==============================================================================
// -- Label Drawing with Shadow --

void SpaceDustLookAndFeel::drawLabel(juce::Graphics& g, juce::Label& label)
{
    g.fillAll(label.findColour(juce::Label::backgroundColourId));

    if (!label.isBeingEdited())
    {
        auto alpha = label.isEnabled() ? 1.0f : 0.5f;
        const juce::Font font(getLabelFont(label));
        
        // Cache label text once to avoid multiple getText() calls
        // This prevents potential issues with string access during drawing
        const juce::String labelText = label.getText();
        
        // Use consistent color for all text (except title)
        juce::Colour textColour = labelCyan;  // Same color for all labels
        bool isValueBox = (labelText.contains("st") || labelText.contains("Hz") ||
                          labelText.contains("s") || labelText.contains("ct") ||
                          labelText.contains("."));  // Numeric values
        
        if (isValueBox)
        {
            // Value readouts: same color, monospace font
            g.setFont(juce::Font(juce::FontOptions("Consolas", 12.0f, juce::Font::plain)));
        }
        else
        {
            // Parameter labels: same color, bold font
            g.setFont(juce::Font(juce::FontOptions(14.0f, juce::Font::bold)));
        }
        
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
    const float textH = 17.0f;  // 16-18pt for group titles
    const float indent = 3.0f;
    const float textEdgeGap = 4.0f;
    auto cs = 5.0f;

    juce::Font f(juce::FontOptions(textH, juce::Font::bold));

    juce::Path p;
    auto x = indent;
    // Position the line below the title text
    auto lineY = f.getAscent() + 2.0f;  // Line is below the text baseline
    auto w = juce::jmax(0.0f, (float)width - x * 2.0f);
    auto h = juce::jmax(0.0f, (float)height - lineY - indent);
    cs = juce::jmin(cs, w * 0.5f, h * 0.5f);

    auto textW = text.isEmpty() ? 0 : f.getStringWidth(text);
    auto textX = x + textEdgeGap;
    auto textY = 0.0f;  // Title is at the top, above the line

    // Use addRoundedRectangle for perfect rounded corners - no arc connection issues
    p.addRoundedRectangle(x, lineY, w, h, cs);

    const auto roundedStroke = [](float strokeW) {
        return juce::PathStrokeType(strokeW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
    };

    bool isActive = group.getProperties().getWithDefault("isActive", false);

    // Blue glow when effect/LFO is enabled - inward-only, follows border curve, thick
    if (isActive)
    {
        g.saveState();
        g.excludeClipRegion(juce::Rectangle<int>(0, 0, width, static_cast<int>(lineY)));
        const float glowThick = 18.0f;  // Thick glow band (pixels inward)
        const int numBands = 18;         // Smooth fade
        juce::Path glowPath;
        glowPath.setUsingNonZeroWinding(false);  // Even-odd for rings
        for (int i = 0; i < numBands; ++i)
        {
            const float outerInset = i * (glowThick / (float)numBands);
            const float innerInset = (i + 1) * (glowThick / (float)numBands);
            // Keep BOTH edges rounded - min 3px prevents sharp corners on outer and inner band edges
            const float outerCs = juce::jmax(3.0f, cs - outerInset);
            const float innerCs = juce::jmax(3.0f, cs - innerInset);
            glowPath.clear();
            glowPath.addRoundedRectangle(x + outerInset, lineY + outerInset,
                                         w - 2.0f * outerInset, h - 2.0f * outerInset, outerCs);
            glowPath.addRoundedRectangle(x + innerInset, lineY + innerInset,
                                         w - 2.0f * innerInset, h - 2.0f * innerInset, innerCs);
            const float t = (float)i / (float)numBands;
            const juce::uint8 alpha = juce::uint8(90 * (1.0f - t * t));  // Fade inward
            g.setColour(juce::Colour(alpha << 24 | 0x00b4ff));
            g.fillPath(glowPath);
        }
        g.restoreState();
    }

    juce::Colour outlineCol = group.findColour(juce::GroupComponent::outlineColourId)
                              .withMultipliedAlpha(group.isEnabled() ? 1.0f : 0.5f);
    if (isActive)
        outlineCol = juce::Colour(0xff00b4ff);  // Bright blue when active
    g.setColour(outlineCol);

    // Exclude label area for outline too, so top border doesn't draw through text
    if (textW > 0)
    {
        g.saveState();
        g.excludeClipRegion(juce::Rectangle<int>(0, 0, width, static_cast<int>(lineY)));
        g.strokePath(p, roundedStroke(isActive ? 2.5f : 2.0f));
        g.restoreState();
    }
    else
        g.strokePath(p, roundedStroke(isActive ? 2.5f : 2.0f));

    // Draw enhanced title text with subtle shadow (above the line)
    if (text.isNotEmpty())
    {
        auto textArea = juce::Rectangle<int>(static_cast<int>(textX),
                                            static_cast<int>(textY),
                                            static_cast<int>(textW + textEdgeGap * 2),
                                            static_cast<int>(textH));
        
        // Draw group title with subtle shadow only (no heavy glow) - same color as all other text
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
    // Use default rotary slider drawing
    LookAndFeel_V4::drawRotarySlider(g, x, y, width, height, sliderPos, rotaryStartAngle, rotaryEndAngle, slider);
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
    // Simplified: just use subtle shadow instead of heavy glow
    drawTextWithShadow(g, text, x, y, width, height, justification, textColour, 0.25f, 1);
}


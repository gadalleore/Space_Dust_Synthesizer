#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

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

private:
    // Refined cosmic color palette - unified and elegant
    juce::Colour titleWhite = juce::Colour(0xffffffff);           // Pure white for title
    juce::Colour titleLightCyan = juce::Colour(0xffd0f4ff);       // Very light cyan for title (alternative)
    juce::Colour groupTitleGrey = juce::Colour(0xffe0e0e0);       // Light grey-white for group titles
    juce::Colour groupTitleCyan = juce::Colour(0xffc0e0ff);      // Light cyan for group titles (alternative)
    juce::Colour labelCyan = juce::Colour(0xffa0d8ff);            // Light cyan-white for parameter labels
    juce::Colour labelCyanAlt = juce::Colour(0xffb8e0ff);         // Alternative label color
    juce::Colour valueCyan = juce::Colour(0xff6dd5fa);            // Bright but soft cyan for value readouts
    juce::Colour valueCyanAlt = juce::Colour(0xff88e0ff);         // Alternative value color
    juce::Colour shadowBlack = juce::Colour(0x33000000);          // 20% opacity black for subtle shadows
};



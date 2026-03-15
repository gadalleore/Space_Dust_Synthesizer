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

    juce::Typeface::Ptr glitchGoblinTypeface;
};



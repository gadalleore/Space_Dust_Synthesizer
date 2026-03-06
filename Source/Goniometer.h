#pragma once

#include <array>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_gui_basics/juce_gui_basics.h>

// Goniometer constants (adapted from MultiMeter)
static constexpr float GONIO_NEGATIVE_INFINITY = -120.0f;
static constexpr float GONIO_MAX_DECIBELS = 12.0f;

//==============================================================================
/**
    Goniometer - Lissajous stereo display (Mid/Side -> XY)
    
    Uses the classic technique: Mid (L+R) on Y, Side (L-R) on X.
    The Lissajous figure stays centered and shows phase/stereo width
    like iZotope Ozone or hardware goniometers.
    
    Adapted from MultiMeter by Zhiyu Alex Zhang.
*/
struct Goniometer : juce::Component
{
    Goniometer() = default;

    void paint(juce::Graphics& g) override;
    void resized() override;

    /** Update visualization with new stereo audio data (copies into internal buffer) */
    void update(const juce::AudioBuffer<float>& buffer);

    /** Update scaling coefficient (dB) for display gain */
    void updateCoeff(float new_db);

private:
    void drawBackground(juce::Graphics& g);

    juce::AudioBuffer<float> internalBuffer;
    juce::Path p;
    int w = 0, h = 0;
    juce::Point<int> center;
    std::array<juce::String, 5> chars { "+S", "-S", "L", "M", "R" };
    float scale = 1.0f;

    // Space Dust cosmic theme colours
    juce::Colour edgeColour       { 0xff4a6fa5 };
    juce::Colour pathColourInside { 0xff6ba3d0 };
    juce::Colour pathColourOutside{ 0xff48bde8 };
    juce::Colour baseColour       { 0xff0a0a1f };
    juce::Colour ellipseFillColour{ 0xff151530 };  // Slightly lighter so Lissajous area is visible
};

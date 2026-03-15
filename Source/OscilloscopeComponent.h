#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_gui_basics/juce_gui_basics.h>

//==============================================================================
/**
    Oscilloscope - time-domain waveform display of L/R audio.
*/
class OscilloscopeComponent : public juce::Component
{
public:
    OscilloscopeComponent() = default;

    void paint(juce::Graphics& g) override;
    void resized() override;

    /** Update with new stereo audio (copies into internal buffer).
        validSamples: actual number of audio samples (may be less than buffer size). */
    void update(const juce::AudioBuffer<float>& buffer, int validSamples = -1);

    void setClipping(bool isClipping)
    {
        traceColour = isClipping ? juce::Colour(0xffdd2222) : juce::Colour(0xff48bde8);
    }

private:
    void drawBackground(juce::Graphics& g);

    juce::AudioBuffer<float> internalBuffer;
    juce::Colour traceColour   { 0xff48bde8 };
    juce::Colour gridColour    { 0xff4a6fa5 };
    juce::Colour bgColour      { 0xff0a0a1f };
};

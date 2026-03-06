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

    /** Update with new stereo audio (copies into internal buffer) */
    void update(const juce::AudioBuffer<float>& buffer);

private:
    void drawBackground(juce::Graphics& g);

    juce::AudioBuffer<float> internalBuffer;
    juce::Colour traceColour   { 0xff48bde8 };
    juce::Colour gridColour    { 0xff4a6fa5 };
    juce::Colour bgColour      { 0xff0a0a1f };
};

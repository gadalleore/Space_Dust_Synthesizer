#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <vector>

//==============================================================================
/**
    SpectrumAnalyser - FFT-based frequency magnitude display.
*/
class SpectrumAnalyserComponent : public juce::Component
{
public:
    SpectrumAnalyserComponent();
    ~SpectrumAnalyserComponent() override = default;

    void paint(juce::Graphics& g) override;
    void resized() override;

    /** Update with new stereo audio (uses L channel for FFT) */
    void update(const juce::AudioBuffer<float>& buffer);

private:
    void drawBackground(juce::Graphics& g);
    void pushNextBlock(const float* src, int numSamples);

    static constexpr int fftOrder = 9;
    static constexpr int fftSize = 1 << fftOrder;

    std::unique_ptr<juce::dsp::FFT> forwardFFT;
    std::unique_ptr<juce::dsp::WindowingFunction<float>> window;
    std::vector<float> fftData;
    std::vector<float> fifo;
    std::vector<float> displayMagnitudes;
    int fifoIndex = 0;
    bool nextFFTBlockReady = false;

    juce::Colour fillColour   { 0xff48bde8 };
    juce::Colour lineColour   { 0xff6ba3d0 };
    juce::Colour gridColour   { 0xff4a6fa5 };
    juce::Colour bgColour     { 0xff0a0a1f };
};

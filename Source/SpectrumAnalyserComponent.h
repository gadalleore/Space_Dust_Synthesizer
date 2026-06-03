#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <vector>

//==============================================================================
/**
    SpectrumAnalyser - FFT-based frequency magnitude display.
*/
class SpectrumAnalyserComponent : public juce::Component,
                                  private juce::Timer
{
public:
    SpectrumAnalyserComponent();
    ~SpectrumAnalyserComponent() override = default;

    void paint(juce::Graphics& g) override;
    void resized() override;

    /** Begin self-driven, high-frame-rate repaint (decoupled from the slow shared editor timer). */
    void start() { startTimerHz(60); }

    /** Editor sets this to fill `dest` with the most-recent `numSamples` of continuous audio. */
    std::function<void(float* dest, int numSamples)> fillSamplesCallback;

    /** Sample rate is needed to map FFT bins onto the log-frequency axis. */
    void setSampleRate(double newSampleRate)
    {
        if (newSampleRate > 0.0)
            sampleRate = static_cast<float>(newSampleRate);
    }

    void setClipping(bool isClipping)
    {
        fillColour = isClipping ? juce::Colour(0xffdd2222) : juce::Colour(0xff48bde8);
        lineColour = isClipping ? juce::Colour(0xffdd3333) : juce::Colour(0xff6ba3d0);
    }

private:
    void timerCallback() override;
    void drawBackground(juce::Graphics& g);
    void computeSpectrum(const float* samples);

    static constexpr int fftOrder = 11;            // 2048-pt FFT: ~21 Hz/bin, resolves low harmonics
    static constexpr int fftSize = 1 << fftOrder;

    // Log-frequency display range (SPAN-style). Each octave gets equal width.
    static constexpr float minFreq = 20.0f;
    static constexpr float maxFreq = 20000.0f;
    float sampleRate = 44100.0f;

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

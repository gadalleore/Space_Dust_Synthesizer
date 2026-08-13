#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <vector>

#include "SpaceDustDither.h"

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

    //==========================================================================
    // -- Embedding the analyser inside another display --
    // The Final EQ draws this same spectrum behind its response curve. There it is
    // a backdrop, not a panel of its own: it must not paint a background over the
    // EQ's, it must span the full width it is given (the EQ maps 20 Hz-20 kHz edge
    // to edge, so any inset here would slide the bars off their frequencies), and
    // it must sit back far enough for the curve and the band dots to stay legible.

    /** false = paint the bars only, leaving whatever is behind them showing. */
    void setDrawBackground(bool shouldDraw) { drawBackgroundEnabled = shouldDraw; }

    /** Inset, in pixels, between the component edge and the drawn spectrum. */
    void setEdgePadding(float newPadding) { edgePad = juce::jmax(0.0f, newPadding); }

    /** Multiplies the bar/cap opacity. 1.0 is the standalone look. */
    void setDisplayAlpha(float newAlpha) { displayAlpha = juce::jlimit(0.0f, 1.0f, newAlpha); }

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

    //==========================================================================
    // -- Motion dither --
    // The bars themselves are a picket fence and smearing every one of them would
    // just fog the panel. What actually reads as movement is the OUTLINE the tops
    // trace, so that is what gets ghosted -- the last few frames of the curve,
    // each in a different channel, drawn behind the live bars.
    static constexpr int   kHistoryLength = 4;
    static constexpr float kTrailSpread   = 3.0f;
    static constexpr float kTrailAlpha    = 0.5f;

    std::vector<juce::Path> outlineHistory;
    SpaceDustDither::TilesPtr ditherTiles;

    /** Set when a new FFT frame has been folded in, cleared once the trail has
        advanced on it. Gates the history so repaints that carry no new audio do
        not flush the ghosts out with copies of the same curve. */
    bool spectrumMoved = false;

    bool  drawBackgroundEnabled = true;
    float edgePad      = 8.0f;
    float displayAlpha = 1.0f;

    juce::Colour fillColour   { 0xff48bde8 };
    juce::Colour lineColour   { 0xff6ba3d0 };
    juce::Colour gridColour   { 0xff4a6fa5 };
    juce::Colour bgColour     { 0xff0a0a1f };
};

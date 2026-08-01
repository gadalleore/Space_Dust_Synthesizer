#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <vector>

#include "SpaceDustDither.h"

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

    //==========================================================================
    // -- Time window (Giuseppe, 2026-08-01) --
    // The scope used to draw exactly the block the processor handed it -- around
    // 441 samples at 44.1k, which is a single cycle of a 100Hz note and far too
    // close in to read as a waveform. Incoming blocks are now appended into a ring
    // and the whole ring is drawn, so the view spans a fixed span of TIME instead
    // of one host block, and it no longer changes zoom when the buffer size does.
    //
    // 4096 samples is ~93ms at 44.1k: roughly nine cycles of a 100Hz note.
    static constexpr int kWindowSamples = 4096;

    juce::AudioBuffer<float> historyBuffer;   // ring, kWindowSamples long
    int  writePos    = 0;
    bool ringPrimed  = false;                 // false until it has filled once

    //==========================================================================
    // -- Motion dither --
    // Recent traces, oldest first, ghosted behind the live one. Built in update()
    // rather than paint() so the history advances with the AUDIO, not with however
    // often the component happens to be repainted.
    static constexpr int   kHistoryLength = 4;
    static constexpr float kTrailSpread   = 3.0f;
    static constexpr float kTrailAlpha    = 0.55f;

    std::vector<juce::Path> traceHistory;

    /** Builds the two-channel trace at the current size. */
    juce::Path buildTrace() const;

    juce::AudioBuffer<float> internalBuffer;
    juce::Colour traceColour   { 0xff48bde8 };
    juce::Colour gridColour    { 0xff4a6fa5 };
    juce::Colour bgColour      { 0xff0a0a1f };
};

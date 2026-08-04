#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <vector>
#include <functional>

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

    /** Fills `dest` with the most recent `numSamples` of CONTINUOUS mono output.

        This is how the scope gets its long view, and it has to come from a single
        gap-free history in the processor. The first attempt appended the per-block
        goniometer snapshots into a ring here instead, which looked fine at a glance
        and was wrong: the UI polls at 20Hz while blocks arrive ~86 times a second, so
        roughly three blocks in four were never seen and the ring held non-adjacent
        slices of audio butted together. The result was a waveform with a
        discontinuity at every join -- visible as the scope "not working properly",
        and worse in FL, whose buffer size makes the joins land differently.
        (Giuseppe, 2026-08-01.) */
    std::function<void(float* destL, float* destR, int numSamples)> fillSamplesCallback;

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
    // 8192 samples is ~186ms at 44.1k: roughly eighteen cycles of a 100Hz note, and
    // the full depth of the processor's gap-free history (spectrumFifoSize). Asking
    // for more than that would silently get clamped back to it, so this is as far out
    // as the scope can go without adding a longer history on the processor side.
    static constexpr int kWindowSamples = 8192;

    juce::AudioBuffer<float> historyBuffer;   // the window, filled wholesale each update

    bool ringPrimed  = false;                 // false until it has filled once

    //==========================================================================
    // -- Motion dither --
    // Recent traces, oldest first, ghosted behind the live one. Built in update()
    // rather than paint() so the history advances with the AUDIO, not with however
    // often the component happens to be repainted.
    static constexpr int   kHistoryLength = 4;
    static constexpr float kTrailSpread   = 3.0f;
    static constexpr float kTrailAlpha    = 0.55f;

    // How thick the trace is drawn. buildTrace() needs it too: the two lanes are
    // spaced so their clip edges sit exactly this far apart, which only lands right
    // if the number the geometry uses is the number the stroke uses.
    static constexpr float kTraceThickness = 2.5f;

    std::vector<juce::Path> traceHistory;
    SpaceDustDither::TilesPtr ditherTiles;

    /** Builds the two-channel trace at the current size. */
    juce::Path buildTrace() const;

    juce::AudioBuffer<float> internalBuffer;
    juce::Colour traceColour   { 0xff48bde8 };
    juce::Colour gridColour    { 0xff4a6fa5 };
    juce::Colour bgColour      { 0xff0a0a1f };
};

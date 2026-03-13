#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>

//==============================================================================
/**
    SpaceDust Compressor - Multi-type compressor with SSL G-Bus emulation.

    Type 0 (SSL G-Bus 4000G):
    - Feed-forward VCA design, hard-knee
    - Peak envelope detector with attack/release
    - Auto-release: program-dependent (fast for transients, slow for sustain)
    - Built-in output soft clipper (gentle tanh saturation)
    - Parallel mix for NY-style compression

    Extensible: add new types via the Type enum and processSSL-like methods.
*/
class SpaceDustCompressor
{
public:
    struct Parameters
    {
        bool enabled = false;
        int type = 0;               // 0 = SSL (extensible)
        float thresholdDb = -12.0f; // -60 to 0 dB
        float ratio = 4.0f;        // 1.0 to 20.0
        float attackMs = 3.0f;     // 0.1 to 80 ms
        float releaseMs = 100.0f;  // 5 to 1200 ms
        float makeupGainDb = 0.0f; // 0 to 24 dB
        float mix = 1.0f;          // 0 to 1 (dry/wet)
        bool autoRelease = false;  // program-dependent release
        bool softClip = false;     // output stage soft clipper
    };

    SpaceDustCompressor() = default;
    ~SpaceDustCompressor() = default;

    void prepare(const juce::dsp::ProcessSpec& spec);
    void reset();
    void setParameters(const Parameters& p);
    void process(juce::AudioBuffer<float>& buffer);

    /** Current gain reduction in dB (for future metering, thread-safe read). */
    float getGainReductionDb() const { return gainReductionDb_.load(std::memory_order_relaxed); }

private:
    juce::dsp::ProcessSpec spec_{};
    Parameters params_;

    // Envelope follower state (per-channel)
    float envelopeL_{0.0f};
    float envelopeR_{0.0f};

    // Auto-release adaptive state
    float autoReleaseEnvL_{0.0f};
    float autoReleaseEnvR_{0.0f};

    // Smoothed parameters (zipper-free)
    juce::SmoothedValue<float> smoothedThreshold_{-12.0f};
    juce::SmoothedValue<float> smoothedRatio_{4.0f};
    juce::SmoothedValue<float> smoothedMakeup_{0.0f};
    juce::SmoothedValue<float> smoothedMix_{1.0f};

    // Gain reduction for display (atomic for thread safety)
    std::atomic<float> gainReductionDb_{0.0f};

    // -- SSL-specific DSP helpers --

    /** Compute attack/release coefficients from time in ms. */
    float msToCoeff(float ms) const;

    /** SSL hard-knee gain computer (dB domain). Returns gain in dB. */
    float computeGainDb(float inputDb, float threshDb, float ratio) const;

    /** Gentle tanh soft clip for output stage. */
    static float softClipSample(float x);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SpaceDustCompressor)
};

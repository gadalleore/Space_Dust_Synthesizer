#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>

//==============================================================================
/**
    SpaceDust Trance Gate - Tempo-synced rhythmic volume gating effect.
    
    Chops audio into rhythmic patterns using an 8-step sequencer. Supports:
    - 4, 8, or 16 steps (16 = double-speed over 8 steps)
    - Tempo sync or free-running (Hz) rate
    - Per-step on/off pattern
    - Attack/release smoothing for click-free gating
    - Dry/wet mix
    
    Based on GATE-12 and Kilohearts Trance Gate references.
*/
class SpaceDustTranceGate
{
public:
    static constexpr int kMaxSteps = 16;

    struct Parameters
    {
        bool enabled = false;
        int numSteps = 8;           // 4, 8, or 16
        bool sync = true;           // Tempo sync vs free Hz
        float rate = 4.0f;          // 0-12: sync index or free Hz (log scale)
        float attackMs = 2.0f;      // Attack time ms (0.1-50)
        float releaseMs = 5.0f;     // Release time ms (0.1-50)
        float mix = 1.0f;           // Wet mix 0-1
        bool stepOn[16] = { true, false, true, false, true, false, true, false,
                            true, false, true, false, true, false, true, false };
    };

    SpaceDustTranceGate() = default;

    void prepare(const juce::dsp::ProcessSpec& spec);
    void reset();
    void setParameters(const Parameters& p);
    void process(juce::AudioBuffer<float>& buffer, double sampleRate,
                 juce::AudioPlayHead* playHead);

private:
    float getStepValue(float phase) const;
    float smoothEnvelope(float raw, float current, bool rising) const;

    juce::dsp::ProcessSpec spec_;
    Parameters params_;

    double phase_ = 0.0;            // 0-1 position in pattern cycle
    float smoothedEnv_ = 0.0f;      // One-pole smoothed envelope value

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SpaceDustTranceGate)
};

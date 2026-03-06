#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>

//==============================================================================
/**
    SpaceDust Flanger - Classic modulated delay-line flanger.
    
    Modeled on the feedforward/feedback comb filter approach from DSP literature
    (Physical Audio Signal Processing, JUCE tutorials). Uses interpolated delay
    line modulated by LFO to create sweeping comb-filter notches.
    
    Algorithm:
    - Short modulated delay (1-15 ms typical) via juce::dsp::DelayLine (linear interpolation)
    - Feedforward comb: out = dry + depth * delayed
    - Optional feedback: delay input += feedback * delayed output (tanh for stability)
    - LFO (sine) modulates delay time: delay_ms = base + depth * LFO()
    
    Parameters: Rate, Depth (sweep), Feedback, Mix
*/
class SpaceDustFlanger
{
public:
    struct Parameters
    {
        bool enabled = false;
        float rateHz = 0.5f;       // LFO speed 0.05-200 Hz
        float depth = 0.5f;        // Sweep amount (modulation depth) 0-1
        float feedback = 0.0f;     // Feedback -1 to 1 (0 = none)
        float width = 0.5f;        // Stereo width 0-1 (0=mono, 1=full stereo L/R offset)
        float mix = 0.5f;          // Dry/wet 0-1
    };

    SpaceDustFlanger() = default;

    void prepare(const juce::dsp::ProcessSpec& spec);
    void reset();
    void setParameters(const Parameters& p);
    void process(juce::AudioBuffer<float>& buffer);

private:
    static constexpr float kMinDelayMs = 0.5f;   // Min delay (ms)
    static constexpr float kMaxDelayMs = 15.0f;  // Max delay (ms) - classic flanger range
    static constexpr float kBaseDelayMs = 3.0f;  // Center/average delay
    static constexpr int kMaxDelaySamples = 1024;  // ~23 ms at 44.1 kHz

    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear> delayLineL_{kMaxDelaySamples};
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear> delayLineR_{kMaxDelaySamples};

    juce::dsp::ProcessSpec spec_;
    Parameters params_;

    float lfoPhaseL_{0.0f};
    float lfoPhaseR_{0.0f};

    juce::SmoothedValue<float> smoothedMix_{0.5f};
    juce::SmoothedValue<float> smoothedDepth_{0.5f};
    juce::SmoothedValue<float> smoothedFeedback_{0.0f};
    juce::SmoothedValue<float> smoothedWidth_{0.5f};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SpaceDustFlanger)
};

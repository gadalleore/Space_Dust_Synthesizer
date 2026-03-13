#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>

//==============================================================================
/**
    SpaceDust Lo-Fi - RC-20 Retro Color inspired lo-fi processor.

    Single "amount" macro knob that progressively engages:
    - High-frequency rolloff (LP filter sweep 20kHz → 2kHz)
    - Bit depth reduction (16-bit → 6-bit)
    - Sample rate reduction (subtle downsampling)
    - Vinyl noise / tape hiss
    - Tape wow & flutter (subtle pitch modulation)
    - Gentle saturation warmth
*/
class SpaceDustLofi
{
public:
    struct Parameters
    {
        bool enabled = false;
        float amount = 0.0f; // 0.0 = clean, 1.0 = full lo-fi
    };

    SpaceDustLofi() = default;
    ~SpaceDustLofi() = default;

    void prepare(const juce::dsp::ProcessSpec& spec);
    void reset();
    void setParameters(const Parameters& p);
    void process(juce::AudioBuffer<float>& buffer);

private:
    juce::dsp::ProcessSpec spec_{};
    Parameters params_;

    // Smoothed amount (zipper-free)
    juce::SmoothedValue<float> smoothedAmount_{0.0f};

    // LP filter for high-frequency rolloff
    juce::dsp::StateVariableTPTFilter<float> lpFilterL_;
    juce::dsp::StateVariableTPTFilter<float> lpFilterR_;

    // Tape wow & flutter LFO state
    float flutterPhase_{0.0f};
    float wowPhase_{0.0f};

    // Sample-and-hold state for sample rate reduction
    float holdL_{0.0f};
    float holdR_{0.0f};
    float holdCounter_{0.0f};

    // Noise generator
    juce::Random noiseRng_;

    // Envelope follower for ADSR-tracking tape hiss
    float envelopeLevel_{0.0f};
    float envAttackCoeff_{0.0f};
    float envReleaseCoeff_{0.0f};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SpaceDustLofi)
};

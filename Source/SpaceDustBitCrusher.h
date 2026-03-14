#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include <random>

//==============================================================================
/**
    SpaceDust Bit Crusher - Aggressive lo-fi bit reduction + rate crushing.

    - Bit depth: Continuous from clean (4096 levels) to 1-bit (2 levels)
    - Exponential Amount curve: most knob travel in the crunchy sweet spot
    - Rate: Sample-and-hold downsampling (1-8 kHz effective), no anti-aliasing
    - Optional phase jitter for less robotic, more "broken" character
    - Mix: dry/wet balance
*/
class SpaceDustBitCrusher
{
public:
    struct Parameters
    {
        bool enabled = false;
        float amount = 0.5f;   // 0 = clean (4096 levels), 1 = 1-bit crush (2 levels)
        float rate = 0.0f;     // 0 = full rate, 1 = ~1 kHz (heavy downsample)
        float mix = 0.5f;      // Dry/wet 0-1
    };

    SpaceDustBitCrusher() = default;

    void prepare(const juce::dsp::ProcessSpec& spec);
    void reset();
    void setParameters(const Parameters& p);
    void process(juce::AudioBuffer<float>& buffer);

private:
    juce::dsp::ProcessSpec spec_;
    Parameters params_;

    juce::SmoothedValue<float> smoothedMix_{0.5f};
    juce::SmoothedValue<float> smoothedAmount_{0.5f};
    juce::SmoothedValue<float> smoothedRate_{0.0f};

    // Sample-and-hold state per channel
    float holdSampleL_{0.0f};
    float holdSampleR_{0.0f};
    float phaseL_{0.0f};
    float phaseR_{0.0f};

    std::mt19937 rng_{static_cast<unsigned>(juce::Time::getMillisecondCounter())};
    std::uniform_real_distribution<float> jitterDist_{-0.015f, 0.015f};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SpaceDustBitCrusher)
};

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>

//==============================================================================
/**
    Grain Delay processor for Space Dust.
    Portal-style granular delay: buffer-based grain extraction, overlapping grains,
    pitch shift, randomization (jitter), and feedback.
*/
class SpaceDustGrainDelay
{
public:
    struct Parameters
    {
        bool enabled = false;
        float delayMs = 200.0f;      // Delay time in ms (20-2000)
        float grainSizeMs = 50.0f;  // Grain length in ms (10-500)
        float pitchSemitones = 0.0f;// Pitch shift (-12 to +12)
        float mix = 0.5f;           // Wet/dry mix (0-1)
        float decay = 0.0f;         // Feedback decay (0-1)
        float density = 1.0f;       // Overlapping grains (1-8)
        float jitter = 0.0f;        // Random pitch/position variation (0-1)
        bool pingPong = false;       // Alternate grains L/R
        bool filterOn = false;       // HP/LP filter on wet signal
        float hpCutoffHz = 100.0f;
        float lpCutoffHz = 4000.0f;
        float hpRes = 0.5f;         // 0-1
        float lpRes = 0.5f;          // 0-1
        bool warmSaturation = false;
    };

    SpaceDustGrainDelay() = default;

    void prepare(const juce::dsp::ProcessSpec& spec);
    void reset();
    void setParameters(const Parameters& p);
    void process(juce::AudioBuffer<float>& buffer);

private:
    static constexpr int kMaxGrains = 8;

    struct Grain
    {
        float readIdxL_{0.0f};
        float readIdxR_{0.0f};
        float phase{0.0f};
        int durationSamples{0};
        float pitchRatio{1.0f};
        float panL{0.5f};  // 0=full L, 1=full R
        float panR{0.5f};
        bool active{false};
    };

    juce::dsp::ProcessSpec spec_;
    Parameters params_;

    static constexpr int maxDelaySamples = 88200;  // ~2s at 44.1 kHz
    juce::AudioBuffer<float> bufferL_;
    juce::AudioBuffer<float> bufferR_;

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> smoothedDelayTime_{1000.0f};
    juce::SmoothedValue<float> smoothedMix_{0.0f};
    juce::SmoothedValue<float> smoothedPitchRatio_{1.0f};
    juce::SmoothedValue<float> smoothedDecay_{0.0f};

    int writeIdx_{0};
    Grain grains_[kMaxGrains];
    float spawnCounter_{0.0f};
    int pingPongIndex_{0};
    juce::Random random_{static_cast<int>(juce::Time::getMillisecondCounter())};

    juce::dsp::StateVariableTPTFilter<float> filterHP_;
    juce::dsp::StateVariableTPTFilter<float> filterLP_;
    juce::dsp::StateVariableTPTFilter<float> filterHPFb_;
    juce::dsp::StateVariableTPTFilter<float> filterLPFb_;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> smoothedHPCutoff_{1000.0f};
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> smoothedLPCutoff_{4000.0f};
    juce::SmoothedValue<float> smoothedHPQ_{0.707f};
    juce::SmoothedValue<float> smoothedLPQ_{0.707f};

    float hanningWindow(float phase) const;
    float readBuffer(const juce::AudioBuffer<float>& buf, int channel, float index) const;
    void spawnGrain(int bufSize, float delaySamples, float grainSizeSamples,
                    float basePitch, float jitterAmount, bool pingPong);
    float nextFloat() { return static_cast<float>(random_.nextFloat()); }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SpaceDustGrainDelay)
};

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include <vector>

//==============================================================================
/**
    "Sexicon take an L" - Dattorro-style plate reverb inspired by Lexicon 480L.
    Based on Jon Dattorro's Effect Design Part 1 (Griesinger topology).
    Figure-8 feedback tank with input diffusion, modulated allpasses, and
    multi-tap output for lush, dense tails.
*/
class SexiconReverb
{
public:
    SexiconReverb() = default;

    void prepare(double sampleRate, int maxBlockSize);
    void reset();
    void setDecay(float decayTimeSeconds);
    void setSampleRate(double sampleRate);

    /** Process mono input, output stereo. Call once per sample. */
    void processSample(float input, float& outputL, float& outputR);

private:
    static constexpr double kOriginalFs = 29761.0;

    int scaleDelay(int samplesAt29761) const;

    // Input diffusion (4 allpass)
    struct AllpassState
    {
        std::vector<float> buffer;
        int writeIndex = 0;
    };
    float processAllpass(AllpassState& state, int delaySamples, float input, float diffusion);
    AllpassState inputAp1_, inputAp2_, inputAp3_, inputAp4_;

    // Tank (figure-8): 8 delay lines, cross-feedback
    std::vector<float> tank_[8];
    int tankWriteIndex_[8] = {0};
    int tankLengths_[8] = {0};

    // Damping state (one-pole LP per tank half)
    float dampState1_ = 0.0f;
    float dampState2_ = 0.0f;

    // Bandwidth filter (input)
    float bandwidthState_ = 0.0f;

    // DC blocker (prevents low-freq buildup / buzz)
    float dcState_ = 0.0f;
    float dcPrevIn_ = 0.0f;

    // Parameters
    double sampleRate_ = 44100.0;
    float decay_ = 0.5f;
    float decayDiffusion1_ = 0.7f;
    float decayDiffusion2_ = 0.5f;
    float inputDiffusion1_ = 0.75f;
    float inputDiffusion2_ = 0.625f;
    float bandwidth_ = 0.97f;   // Slight input rolloff (was 0.9995 - too bright)
    float damping_ = 0.65f;    // Strong HF damping - kills resonant/periodic buzz

    // Modulation disabled - was causing periodic artifact; tank still sounds lush

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SexiconReverb)
};

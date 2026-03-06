//==============================================================================
// SpaceDust Flanger - Modulated delay line flanger
//
// Classic feedforward/feedback comb filter with LFO-modulated delay.
// delay_ms = base + depth * LFO(); out = dry + wet * delayed; feedback path.
//==============================================================================

#include "SpaceDustFlanger.h"
#include <cmath>

#ifndef juce_PI
#define juce_PI 3.14159265358979323846
#endif

namespace
{
    constexpr float kDenormThreshold = 1e-15f;
}

//==============================================================================
void SpaceDustFlanger::prepare(const juce::dsp::ProcessSpec& spec)
{
    spec_ = spec;
    reset();

    delayLineL_.prepare(spec);
    delayLineR_.prepare(spec);

    const double rampSec = 0.02;
    smoothedMix_.reset(spec.sampleRate, rampSec);
    smoothedDepth_.reset(spec.sampleRate, rampSec);
    smoothedFeedback_.reset(spec.sampleRate, rampSec);
    smoothedWidth_.reset(spec.sampleRate, rampSec);

    smoothedMix_.setCurrentAndTargetValue(params_.mix);
    smoothedDepth_.setCurrentAndTargetValue(params_.depth);
    smoothedFeedback_.setCurrentAndTargetValue(params_.feedback);
    smoothedWidth_.setCurrentAndTargetValue(params_.width);
}

void SpaceDustFlanger::reset()
{
    delayLineL_.reset();
    delayLineR_.reset();
    lfoPhaseL_ = 0.0f;
    lfoPhaseR_ = 0.0f;
}

void SpaceDustFlanger::setParameters(const Parameters& p)
{
    params_ = p;
    smoothedMix_.setTargetValue(p.mix);
    smoothedDepth_.setTargetValue(p.depth);
    smoothedFeedback_.setTargetValue(p.feedback);
    smoothedWidth_.setTargetValue(p.width);
}

//==============================================================================
void SpaceDustFlanger::process(juce::AudioBuffer<float>& buffer)
{
    if (!params_.enabled || buffer.getNumSamples() == 0)
        return;

    const int numChannels = juce::jmin(2, buffer.getNumChannels());
    const int numSamples = buffer.getNumSamples();
    const float sr = static_cast<float>(spec_.sampleRate);
    const float lfoInc = params_.rateHz / sr;

    // Base delay ~3 ms, sweep ± (depth * ~6 ms) for 0-15 ms range
    const float baseSamples = kBaseDelayMs * sr * 0.001f;
    const float maxSweepSamples = (kMaxDelayMs - kMinDelayMs) * sr * 0.001f;

    auto* left = buffer.getWritePointer(0);
    auto* right = numChannels > 1 ? buffer.getWritePointer(1) : left;

    for (int i = 0; i < numSamples; ++i)
    {
        const float mix = smoothedMix_.getNextValue();
        const float depth = smoothedDepth_.getNextValue();
        const float fb = juce::jlimit(-0.99f, 0.99f, smoothedFeedback_.getNextValue());
        const float width = smoothedWidth_.getNextValue();  // 0=mono, 1=full stereo (180° offset)

        // LFO: sine 0..1, mapped to delay time; width controls stereo phase offset (0-0.5 = 0°-180°)
        const float lfoL = 0.5f + 0.5f * std::sin(static_cast<float>(juce_PI) * 2.0f * lfoPhaseL_);
        const float lfoR = 0.5f + 0.5f * std::sin(static_cast<float>(juce_PI) * 2.0f * (lfoPhaseR_ + width * 0.5f));

        const float delaySamplesL = juce::jlimit(1.0f, static_cast<float>(kMaxDelaySamples - 1),
            baseSamples + (lfoL - 0.5f) * 2.0f * maxSweepSamples * depth);
        const float delaySamplesR = juce::jlimit(1.0f, static_cast<float>(kMaxDelaySamples - 1),
            baseSamples + (lfoR - 0.5f) * 2.0f * maxSweepSamples * depth);

        lfoPhaseL_ += lfoInc;
        lfoPhaseR_ += lfoInc;
        if (lfoPhaseL_ >= 1.0f) lfoPhaseL_ -= 1.0f;
        if (lfoPhaseR_ >= 1.0f) lfoPhaseR_ -= 1.0f;

        const float lIn = left[i];
        const float rIn = right[i];

        float lDelayed = delayLineL_.popSample(0, delaySamplesL, true);
        float rDelayed = delayLineR_.popSample(0, delaySamplesR, true);

        float lOut = (1.0f - mix) * lIn + mix * lDelayed;
        float rOut = (1.0f - mix) * rIn + mix * rDelayed;

        float lFb = std::tanh(juce::jlimit(-2.0f, 2.0f, lIn + fb * lDelayed));
        float rFb = std::tanh(juce::jlimit(-2.0f, 2.0f, rIn + fb * rDelayed));

        delayLineL_.pushSample(0, lFb);
        delayLineR_.pushSample(0, rFb);

        if (std::abs(lOut) < kDenormThreshold) lOut = 0.0f;
        if (std::abs(rOut) < kDenormThreshold) rOut = 0.0f;
        left[i] = lOut;
        right[i] = rOut;
    }
}

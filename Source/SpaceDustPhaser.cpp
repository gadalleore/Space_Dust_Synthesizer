//==============================================================================
// SpaceDust Phaser - MXR Phase 90-inspired stereo phaser
//
// All-pass cascade: 4 stages (Phase 90) or 6 stages (deeper swirl).
// LFO modulates the first-order all-pass coefficients to create sweeping notches.
// Feedback: Script mode = none (clean). Block Logo mode = adjustable (mid-hump).
//==============================================================================

#include "SpaceDustPhaser.h"
#include <cmath>

#ifndef juce_PI
#define juce_PI 3.14159265358979323846
#endif

namespace
{
    // Denorm protection threshold
    constexpr float kDenormThreshold = 1e-15f;
}

//==============================================================================
void SpaceDustPhaser::prepare(const juce::dsp::ProcessSpec& spec)
{
    spec_ = spec;
    reset();

    const double rampSec = 0.02;
    smoothedMix_.reset(spec.sampleRate, rampSec);
    smoothedDepth_.reset(spec.sampleRate, rampSec);
    smoothedFeedback_.reset(spec.sampleRate, rampSec);
    smoothedCentre_.reset(spec.sampleRate, rampSec);

    smoothedMix_.setCurrentAndTargetValue(params_.mix);
    smoothedDepth_.setCurrentAndTargetValue(params_.depth);
    smoothedFeedback_.setCurrentAndTargetValue(params_.feedback);
    smoothedCentre_.setCurrentAndTargetValue(params_.centreHz);
}

void SpaceDustPhaser::reset()
{
    for (int ch = 0; ch < 2; ++ch)
        for (int s = 0; s < kMaxStages; ++s)
            allPass_[ch][s].reset();
    lfoPhaseL_ = 0.0f;
}

void SpaceDustPhaser::setParameters(const Parameters& p)
{
    params_ = p;
}

//==============================================================================
// All-pass coefficient from frequency.
// First-order all-pass: H(z) = (a1 + z^-1)/(1 + a1*z^-1)
// Coefficient a1 = (tan(pi*f/sr) - 1) / (tan(pi*f/sr) + 1)
// Sweeps phase from -180° to 0° as f goes from 0 to Nyquist.
// This creates the notch when mixed with dry; modulating f sweeps the notch.
void SpaceDustPhaser::updateAllPassCoefficients(int channel, float modLfo,
                                                float depth, float centre)
{
    const float sr = static_cast<float>(spec_.sampleRate);
    const int nStages = juce::jlimit(2, kMaxStages, params_.numStages);

    // MXR Phase 90 base notch frequencies ~58 Hz and ~341 Hz; we use centre as sweep center
    // Sweep range: min = centre/(1+depth), max = centre*(1+depth)
    const float minF = juce::jlimit(20.0f, 18000.0f, centre / (1.0f + depth * 2.0f));
    const float maxF = juce::jlimit(20.0f, 18000.0f, centre * (1.0f + depth * 2.0f));
    const float freq = minF + (maxF - minF) * modLfo;

    const float w = static_cast<float>(juce_PI) * freq / sr;
    const float tanW = std::tan(w);
    const float a1 = (tanW - 1.0f) / (tanW + 1.0f);

    for (int s = 0; s < nStages; ++s)
        allPass_[channel][s].setCoefficient(a1);
}

//==============================================================================
void SpaceDustPhaser::process(juce::AudioBuffer<float>& buffer)
{
    if (!params_.enabled || buffer.getNumSamples() == 0)
        return;

    const int numChannels = juce::jmin(buffer.getNumChannels(), 2);
    const int numSamples = buffer.getNumSamples();
    const float sr = static_cast<float>(spec_.sampleRate);
    const int nStages = juce::jlimit(2, kMaxStages, params_.numStages);

    // LFO rate in Hz, clamp to avoid instability
    const float rateHz = juce::jlimit(0.05f, 200.0f, params_.rateHz);
    const float phaseIncrement = rateHz / sr;

    for (int i = 0; i < numSamples; ++i)
    {
        // Smoothed parameters (advance once per sample)
        const float mix = smoothedMix_.getNextValue();
        const float depth = smoothedDepth_.getNextValue();
        const float centre = smoothedCentre_.getNextValue();
        const float feedback = smoothedFeedback_.getNextValue();
        const float width = juce::jlimit(0.0f, 1.0f, params_.stereoOffset);

        // LFO: sine or triangle, with optional vintage JFET-like shaping
        auto lfoValue = [](float phase, bool vintage) -> float
        {
            float raw;
            if (vintage)
            {
                // Triangle wave (vintage Phase 90): 0->1->0 in one period
                float t = phase;
                if (t > 0.5f) t = 1.0f - t;
                raw = t * 2.0f;  // 0 to 1
                // JFET-like soft clip on modulation for analog feel
                raw = std::tanh(raw * 1.5f) / std::tanh(1.5f);
            }
            else
            {
                raw = 0.5f + 0.5f * std::sin(phase * 2.0f * static_cast<float>(juce_PI));
            }
            return juce::jlimit(0.0f, 1.0f, raw);
        };

        // Advance LFO; R channel uses fixed phase offset from L (matches Flanger: param 0–1 → 0–180°)
        lfoPhaseL_ += phaseIncrement;
        if (lfoPhaseL_ >= 1.0f) lfoPhaseL_ -= 1.0f;

        float phaseR = lfoPhaseL_ + width * 0.5f;
        phaseR = std::fmod(phaseR, 1.0f);
        if (phaseR < 0.0f) phaseR += 1.0f;

        const float lfoL = lfoValue(lfoPhaseL_, params_.vintageMode);
        const float lfoR = lfoValue(phaseR, params_.vintageMode);

        // Update all-pass coefficients for both channels (different LFO = stereo width)
        updateAllPassCoefficients(0, lfoL, depth, centre);
        updateAllPassCoefficients(1, lfoR, depth, centre);

        const float lIn = buffer.getSample(0, i);
        const float rIn = numChannels > 1 ? buffer.getSample(1, i) : lIn;

        for (int ch = 0; ch < numChannels; ++ch)
        {
            auto* data = buffer.getWritePointer(ch);
            const float dry = (ch == 0) ? lIn : rIn;

            // Feedback (Block Logo): feedback from last stage output to first stage input
            // Creates resonance/mid-hump like Phase 90 Block Logo. Script = no feedback.
            const float fbPrev = (ch == 0) ? prevWetL_ : prevWetR_;
            const float chainInput = dry + feedback * fbPrev;

            // Cascade through all-pass stages
            float wet = chainInput;
            for (int s = 0; s < nStages; ++s)
                wet = allPass_[ch][s].process(wet);

            // Saturate feedback path (prevents runaway, emulates analog limiting)
            wet = std::tanh(juce::jlimit(-4.0f, 4.0f, wet));

            if (ch == 0)
                prevWetL_ = wet;
            else
                prevWetR_ = wet;

            // Dry/wet mix (Phase 90 output mixer ~50% typical)
            data[i] = dry * (1.0f - mix) + wet * mix;

            // Denorm protection
            if (std::abs(data[i]) < kDenormThreshold)
                data[i] = 0.0f;
        }
    }

    // Set targets for next block (values read at start of process)
    smoothedMix_.setTargetValue(params_.mix);
    smoothedDepth_.setTargetValue(params_.depth);
    smoothedFeedback_.setTargetValue(params_.feedback);
    smoothedCentre_.setTargetValue(params_.centreHz);
}

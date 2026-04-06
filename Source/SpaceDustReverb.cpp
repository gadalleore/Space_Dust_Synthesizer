#include "SpaceDustReverb.h"

//==============================================================================
void SpaceDustReverb::prepare(const juce::dsp::ProcessSpec& spec)
{
    spec_ = spec;
    const double sr = spec.sampleRate;

    freeverb_.setSampleRate(sr);
    freeverb_.reset();
    sexicon_.prepare(sr, spec.maximumBlockSize);

    juce::dsp::ProcessSpec filterSpec;
    filterSpec.sampleRate = sr;
    filterSpec.maximumBlockSize = spec.maximumBlockSize;
    filterSpec.numChannels = 2;
    filterHP_.prepare(filterSpec);
    filterLP_.prepare(filterSpec);
    filterHP_.setType(juce::dsp::StateVariableTPTFilterType::highpass);
    filterLP_.setType(juce::dsp::StateVariableTPTFilterType::lowpass);
    filterHP_.reset();
    filterLP_.reset();

    smoothedWet_.reset(sr, 0.02);
    smoothedDry_.reset(sr, 0.02);
}

void SpaceDustReverb::reset()
{
    freeverb_.reset();
    sexicon_.reset();
    filterHP_.reset();
    filterLP_.reset();
}

void SpaceDustReverb::setParameters(const Parameters& p)
{
    params_ = p;
    smoothedWet_.setTargetValue(params_.wetMix);
    smoothedDry_.setTargetValue(1.0f - params_.wetMix);

    juce::Reverb::Parameters rp;
    // Match prior 0.8–640 s curve above 0.8 s; ramp room size from 0 at t=0 so minimum decay is inaudible.
    const float rt = juce::jmax(0.0f, params_.decayTime);
    if (rt <= 0.0f)
        rp.roomSize = 0.0f;
    else if (rt < 0.8f)
        rp.roomSize = (rt / 0.8f) * 0.5f;
    else
        rp.roomSize = 0.5f + 0.5f * juce::jlimit(0.0f, 1.0f, (rt - 0.8f) / (640.0f - 0.8f));
    rp.damping = 0.5f;
    rp.wetLevel = 0.5f;   // Tame level - we do our own mix
    rp.dryLevel = 0.0f;   // Pure reverb output, we add dry ourselves
    rp.width = 0.9f;      // Slight crossfeed for L/R balance (was right-biased at 1.0)
    rp.freezeMode = 0.0f;
    freeverb_.setParameters(rp);
    sexicon_.setDecay(params_.decayTime);
}

void SpaceDustReverb::process(juce::AudioBuffer<float>& buffer)
{
    const int numSamples = buffer.getNumSamples();
    const int numChannels = buffer.getNumChannels();
    if (numSamples == 0 || numChannels < 2) return;

    float* left = buffer.getWritePointer(0);
    float* right = buffer.getWritePointer(1);

    juce::AudioBuffer<float> dryBuffer(2, numSamples);
    dryBuffer.copyFrom(0, 0, left, numSamples);
    dryBuffer.copyFrom(1, 0, right, numSamples);

    if (params_.type == SexiconTakeAnL)
    {
        sexicon_.setSampleRate(spec_.sampleRate);
        for (int i = 0; i < numSamples; ++i)
        {
            const float monoIn = (left[i] + right[i]) * 0.5f;
            float outL, outR;
            sexicon_.processSample(monoIn, outL, outR);
            left[i] = outL;
            right[i] = outR;
        }
    }
    else
    {
        freeverb_.processStereo(left, right, numSamples);
    }

    const float hpCut = juce::jlimit(20.0f, 20000.0f, params_.filterHPCutoff);
    const float lpCut = juce::jlimit(20.0f, 20000.0f, params_.filterLPCutoff);
    const float hpQ = juce::jlimit(0.1f, 5.0f, 0.1f + params_.filterHPResonance * 4.9f);
    const float lpQ = juce::jlimit(0.1f, 5.0f, 0.1f + params_.filterLPResonance * 4.9f);

    for (int i = 0; i < numSamples; ++i)
    {
        float wet = smoothedWet_.getNextValue();
        float dry = smoothedDry_.getNextValue();
        float lWet = left[i];
        float rWet = right[i];

        if (params_.filterOn)
        {
            filterHP_.setCutoffFrequency(hpCut);
            filterHP_.setResonance(hpQ);
            filterLP_.setCutoffFrequency(lpCut);
            filterLP_.setResonance(lpQ);
            lWet = filterLP_.processSample(0, filterHP_.processSample(0, lWet));
            rWet = filterLP_.processSample(1, filterHP_.processSample(1, rWet));
        }

        if (params_.filterWarmSaturation)
        {
            float drive = 1.6f + (hpQ + lpQ) * 0.25f;  // Stronger than delay - reverb level is lower
            lWet = std::tanh(juce::jlimit(-2.0f, 2.0f, lWet) * drive);
            rWet = std::tanh(juce::jlimit(-2.0f, 2.0f, rWet) * drive);
        }

        float lDry = dryBuffer.getSample(0, i);
        float rDry = dryBuffer.getSample(1, i);
        // Slight L/R balance correction (reverb tends right-biased)
        lWet *= 1.03f;
        rWet *= 0.97f;
        left[i] = lWet * wet + lDry * dry;
        right[i] = rWet * wet + rDry * dry;
    }
}

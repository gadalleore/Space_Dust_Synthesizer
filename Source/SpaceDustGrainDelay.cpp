#include "SpaceDustGrainDelay.h"

//==============================================================================
float SpaceDustGrainDelay::hanningWindow(float phase) const
{
    if (phase <= 0.0f || phase >= 1.0f) return 0.0f;
    return 0.5f * (1.0f - std::cos(2.0f * juce::MathConstants<float>::pi * phase));
}

float SpaceDustGrainDelay::readBuffer(const juce::AudioBuffer<float>& buf, int channel, float index) const
{
    const int size = buf.getNumSamples();
    if (size == 0) return 0.0f;
    index = std::fmod(index, static_cast<float>(size));
    if (index < 0.0f) index += size;
    int i0 = static_cast<int>(index) % size;
    int i1 = (i0 + 1) % size;
    float frac = index - std::floor(index);
    const float* data = buf.getReadPointer(channel);
    return data[i0] * (1.0f - frac) + data[i1] * frac;
}

void SpaceDustGrainDelay::spawnGrain(int bufSize, float delaySamples, float grainSizeSamples,
                                     float basePitch, float jitterAmount, bool pingPong)
{
    // Find inactive grain
    for (int g = 0; g < kMaxGrains; ++g)
    {
        if (!grains_[g].active)
        {
            int base = (writeIdx_ - static_cast<int>(delaySamples) + bufSize) % bufSize;
            if (base < 0) base += bufSize;

            // Position jitter: random offset within ±jitterAmount * grainSize
            float posJitter = (nextFloat() - 0.5f) * 2.0f * jitterAmount * grainSizeSamples * 0.5f;
            float readStart = static_cast<float>(base) + posJitter;

            // Pitch jitter: ±jitterAmount * 6 semitones
            float pitchJitter = 1.0f;
            if (jitterAmount > 0.001f)
                pitchJitter = std::pow(2.0f, (nextFloat() - 0.5f) * 2.0f * jitterAmount * 6.0f / 12.0f);

            grains_[g].readIdxL_ = readStart;
            grains_[g].readIdxR_ = readStart;
            grains_[g].phase = 0.0f;
            grains_[g].durationSamples = static_cast<int>(grainSizeSamples);
            grains_[g].pitchRatio = basePitch * pitchJitter;

            // Ping-pong: alternate grains L/R; otherwise use jitter pan
            if (pingPong)
            {
                bool toLeft = (pingPongIndex_ & 1) == 0;
                grains_[g].panL = toLeft ? 1.0f : 0.0f;
                grains_[g].panR = toLeft ? 0.0f : 1.0f;
                pingPongIndex_++;
            }
            else
            {
                float pan = 0.5f + (nextFloat() - 0.5f) * jitterAmount * 0.5f;
                pan = juce::jlimit(0.0f, 1.0f, pan);
                grains_[g].panL = 1.0f - pan;
                grains_[g].panR = pan;
            }
            grains_[g].active = true;
            return;
        }
    }
}

void SpaceDustGrainDelay::prepare(const juce::dsp::ProcessSpec& spec)
{
    spec_ = spec;
    bufferL_.setSize(1, maxDelaySamples);
    bufferR_.setSize(1, maxDelaySamples);
    bufferL_.clear();
    bufferR_.clear();

    smoothedDelayTime_.reset(static_cast<float>(spec.sampleRate), 0.05);
    smoothedDelayTime_.setCurrentAndTargetValue(200.0f * static_cast<float>(spec.sampleRate) / 1000.0f);
    smoothedMix_.reset(static_cast<float>(spec.sampleRate), 0.02);
    smoothedMix_.setCurrentAndTargetValue(0.0f);
    smoothedPitchRatio_.reset(static_cast<float>(spec.sampleRate), 0.02);
    smoothedPitchRatio_.setCurrentAndTargetValue(1.0f);
    smoothedDecay_.reset(static_cast<float>(spec.sampleRate), 0.02);
    smoothedDecay_.setCurrentAndTargetValue(0.0f);
    smoothedHPCutoff_.reset(static_cast<float>(spec.sampleRate), 0.02);
    smoothedHPCutoff_.setCurrentAndTargetValue(100.0f);
    smoothedLPCutoff_.reset(static_cast<float>(spec.sampleRate), 0.02);
    smoothedLPCutoff_.setCurrentAndTargetValue(4000.0f);
    smoothedHPQ_.reset(static_cast<float>(spec.sampleRate), 0.02);
    smoothedHPQ_.setCurrentAndTargetValue(0.707f);
    smoothedLPQ_.reset(static_cast<float>(spec.sampleRate), 0.02);
    smoothedLPQ_.setCurrentAndTargetValue(0.707f);

    filterHP_.reset();
    filterLP_.reset();
    filterHPFb_.reset();
    filterLPFb_.reset();
    filterHP_.prepare(spec);
    filterLP_.prepare(spec);
    filterHPFb_.prepare(spec);
    filterLPFb_.prepare(spec);

    writeIdx_ = 0;
    spawnCounter_ = 0.0f;
    for (int g = 0; g < kMaxGrains; ++g)
        grains_[g].active = false;
}

void SpaceDustGrainDelay::reset()
{
    bufferL_.clear();
    bufferR_.clear();
    filterHP_.reset();
    filterLP_.reset();
    filterHPFb_.reset();
    filterLPFb_.reset();
    writeIdx_ = 0;
    spawnCounter_ = 0.0f;
    for (int g = 0; g < kMaxGrains; ++g)
        grains_[g].active = false;
}

void SpaceDustGrainDelay::setParameters(const Parameters& p)
{
    params_ = p;
}

void SpaceDustGrainDelay::process(juce::AudioBuffer<float>& buffer)
{
    if (!params_.enabled || buffer.getNumChannels() < 2)
        return;

    const int numSamples = buffer.getNumSamples();
    const int bufSize = bufferL_.getNumSamples();
    const float sampleRate = static_cast<float>(spec_.sampleRate);
    const float delaySamples = juce::jlimit(1.0f, static_cast<float>(maxDelaySamples - 1),
        params_.delayMs * sampleRate / 1000.0f);
    const float grainSizeSamples = juce::jlimit(4.0f, 22050.0f,
        params_.grainSizeMs * sampleRate / 1000.0f);
    const float pitchRatio = std::pow(2.0f, params_.pitchSemitones / 12.0f);
    const float mix = juce::jlimit(0.0f, 1.0f, params_.mix);
    const float decay = juce::jlimit(0.0f, 0.999f, params_.decay);  // Longer decay (up to 150%)
    const float density = juce::jlimit(1.0f, static_cast<float>(kMaxGrains), params_.density);
    const float jitter = juce::jlimit(0.0f, 1.0f, params_.jitter);
    const bool filterOn = params_.filterOn;
    const float hpCutoff = juce::jlimit(20.0f, 20000.0f, params_.hpCutoffHz);
    const float lpCutoff = juce::jlimit(20.0f, 20000.0f, params_.lpCutoffHz);
    const float hpQ = juce::jlimit(0.1f, 5.0f, 0.1f + params_.hpRes * 4.9f);
    const float lpQ = juce::jlimit(0.1f, 5.0f, 0.1f + params_.lpRes * 4.9f);

    smoothedDelayTime_.setTargetValue(delaySamples);
    smoothedMix_.setTargetValue(mix);
    smoothedPitchRatio_.setTargetValue(pitchRatio);
    smoothedDecay_.setTargetValue(decay);
    smoothedHPCutoff_.setTargetValue(hpCutoff);
    smoothedLPCutoff_.setTargetValue(lpCutoff);
    smoothedHPQ_.setTargetValue(hpQ);
    smoothedLPQ_.setTargetValue(lpQ);

    if (filterOn)
    {
        filterHP_.setType(juce::dsp::StateVariableTPTFilterType::highpass);
        filterLP_.setType(juce::dsp::StateVariableTPTFilterType::lowpass);
        filterHPFb_.setType(juce::dsp::StateVariableTPTFilterType::highpass);
        filterLPFb_.setType(juce::dsp::StateVariableTPTFilterType::lowpass);
    }

    const float spawnInterval = grainSizeSamples / density;
    const float grainAdvance = 1.0f / grainSizeSamples;

    auto* left = buffer.getWritePointer(0);
    auto* right = buffer.getWritePointer(1);
    float* bufL = bufferL_.getWritePointer(0);
    float* bufR = bufferR_.getWritePointer(0);

    for (int i = 0; i < numSamples; ++i)
    {
        float dryL = left[i];
        float dryR = right[i];

        float currentDelay = juce::jmax(1.0f, smoothedDelayTime_.getNextValue());
        float wetMix = smoothedMix_.getNextValue();
        float basePitch = smoothedPitchRatio_.getNextValue();
        float fbDecay = smoothedDecay_.getNextValue();

        // Spawn grains based on density (Portal-style)
        spawnCounter_ += 1.0f;
        if (spawnCounter_ >= spawnInterval)
        {
            spawnCounter_ -= spawnInterval;
            spawnGrain(bufSize, currentDelay, grainSizeSamples, basePitch, jitter, params_.pingPong);
        }

        // Accumulate output from all active grains
        float wetL = 0.0f;
        float wetR = 0.0f;
        float fbL = 0.0f;
        float fbR = 0.0f;

        for (int g = 0; g < kMaxGrains; ++g)
        {
            if (!grains_[g].active) continue;

            float delayedL = readBuffer(bufferL_, 0, grains_[g].readIdxL_);
            float delayedR = readBuffer(bufferR_, 0, grains_[g].readIdxR_);
            float window = hanningWindow(grains_[g].phase);
            float samp = (delayedL + delayedR) * 0.5f * window;  // Mono grain, stereo pan

            wetL += samp * grains_[g].panL;
            wetR += samp * grains_[g].panR;
            fbL += samp * grains_[g].panL;
            fbR += samp * grains_[g].panR;

            // Advance grain
            grains_[g].phase += grainAdvance;
            grains_[g].readIdxL_ += grains_[g].pitchRatio;
            grains_[g].readIdxR_ += grains_[g].pitchRatio;

            if (grains_[g].phase >= 1.0f)
                grains_[g].active = false;
        }

        // Normalize by sqrt of active grains to avoid level explosion
        int activeCount = 0;
        for (int g = 0; g < kMaxGrains; ++g) if (grains_[g].active) ++activeCount;
        float norm = (activeCount > 0) ? 1.0f / std::sqrt(static_cast<float>(activeCount)) : 1.0f;
        wetL *= norm;
        wetR *= norm;
        fbL *= norm;
        fbR *= norm;

        // Apply filter to wet and feedback (like Delay)
        if (filterOn)
        {
            float hpC = smoothedHPCutoff_.getNextValue();
            float lpC = smoothedLPCutoff_.getNextValue();
            filterHP_.setCutoffFrequency(hpC);
            filterHP_.setResonance(smoothedHPQ_.getNextValue());
            filterLP_.setCutoffFrequency(lpC);
            filterLP_.setResonance(smoothedLPQ_.getNextValue());
            filterHPFb_.setCutoffFrequency(hpC);
            filterHPFb_.setResonance(0.707f);
            filterLPFb_.setCutoffFrequency(lpC);
            filterLPFb_.setResonance(0.707f);
            wetL = filterLP_.processSample(0, filterHP_.processSample(0, wetL));
            wetR = filterLP_.processSample(1, filterHP_.processSample(1, wetR));
            fbL = filterLPFb_.processSample(0, filterHPFb_.processSample(0, fbL));
            fbR = filterLPFb_.processSample(1, filterHPFb_.processSample(1, fbR));
        }
        if (params_.warmSaturation)
        {
            float drive = 1.0f + (hpQ + lpQ) * 0.15f;
            wetL = std::tanh(juce::jlimit(-1.5f, 1.5f, wetL) * drive);
            wetR = std::tanh(juce::jlimit(-1.5f, 1.5f, wetR) * drive);
            fbL = std::tanh(juce::jlimit(-1.5f, 1.5f, fbL) * drive);
            fbR = std::tanh(juce::jlimit(-1.5f, 1.5f, fbR) * drive);
        }

        left[i] = dryL * (1.0f - wetMix) + wetL * wetMix;
        right[i] = dryR * (1.0f - wetMix) + wetR * wetMix;

        // Write to buffer: input + feedback (Portal-style)
        bufL[writeIdx_] = dryL + fbDecay * fbL;
        bufR[writeIdx_] = dryR + fbDecay * fbR;

        writeIdx_ = (writeIdx_ + 1) % bufSize;
    }
}

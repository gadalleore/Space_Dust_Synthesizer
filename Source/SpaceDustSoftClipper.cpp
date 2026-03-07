//==============================================================================
// SpaceDust Soft Clipper - KClip 3-style mastering clipper
//
// Modes: Smooth (tanh), Crisp (cubic), Tube (even harmonics), Tape (roll-off),
//       Guitar (asymmetric). Oversampling 2x-16x to reduce aliasing.
//==============================================================================

#include "SpaceDustSoftClipper.h"
#include <cmath>

namespace
{

    // Fast tanh approximation (rational, avoids std::tanh for speed)
    inline float fastTanh(float x)
    {
        x = juce::jlimit(-9.0f, 9.0f, x);
        const float x2 = x * x;
        return x * (27.0f + x2) / (27.0f + 9.0f * x2);
    }
}

//==============================================================================
float SpaceDustSoftClipper::clipSmooth(float x, float k, float t) const
{
    // tanh-based: y = tanh(k*x), symmetric, tube-like
    (void)t;
    return fastTanh(k * juce::jlimit(-2.0f, 2.0f, x));
}

float SpaceDustSoftClipper::clipCrisp(float x, float k, float t) const
{
    // Cubic: 1.5*x - 0.5*x^3 for |x|<1, low-order odd harmonics
    (void)t;
    x = k * juce::jlimit(-2.0f, 2.0f, x);
    if (std::abs(x) >= 1.0f)
        return (x > 0.0f) ? 1.0f : -1.0f;
    return 1.5f * x - 0.5f * x * x * x;
}

float SpaceDustSoftClipper::clipTube(float x, float k, float t) const
{
    // Asymmetric: different curves for +/-, adds even harmonics
    (void)t;
    x = k * juce::jlimit(-2.0f, 2.0f, x);
    if (x >= 0.0f)
        return fastTanh(x);
    // Softer on negative half for tube asymmetry
    return -fastTanh(-x * 0.85f);
}

float SpaceDustSoftClipper::clipTape(float x, float k, float t) const
{
    // Soft saturation: x / (1 + |x|^n), tape-like compression
    (void)t;
    x = k * juce::jlimit(-2.0f, 2.0f, x);
    const float ax = std::abs(x);
    return x / (1.0f + ax * ax);
}

float SpaceDustSoftClipper::clipGuitar(float x, float k, float t) const
{
    // Asymmetric diode: sign(x) * (1 - exp(-k*|x|))
    (void)t;
    x = k * juce::jlimit(-2.0f, 2.0f, x);
    const float ax = std::abs(x);
    float y;
    if (x >= 0.0f)
        y = 1.0f - std::exp(-ax);
    else
        y = -(1.0f - std::exp(-ax)) * 0.92f;  // Slightly asymmetric
    return juce::jlimit(-1.0f, 1.0f, y);
}

//==============================================================================
void SpaceDustSoftClipper::ensureOversampler(int factor)
{
    if (oversampler_ != nullptr && currentOversampleFactor_ == factor)
        return;

    currentOversampleFactor_ = factor;
    oversampler_.reset();

    if (factor <= 1)
        return;

    // JUCE: factor is exponent -> 2^factor. So 2x=1, 4x=2, 8x=3, 16x=4
    int exp = 1;
    if (factor >= 16) exp = 4;
    else if (factor >= 8) exp = 3;
    else if (factor >= 4) exp = 2;
    else if (factor >= 2) exp = 1;

    oversampler_ = std::make_unique<Oversampler>(
        static_cast<size_t>(spec_.numChannels),
        static_cast<size_t>(exp),
        Oversampler::filterHalfBandPolyphaseIIR,
        true,
        false);

    oversampler_->initProcessing(static_cast<size_t>(spec_.maximumBlockSize));
    oversampler_->reset();
}

//==============================================================================
void SpaceDustSoftClipper::prepare(const juce::dsp::ProcessSpec& spec)
{
    spec_ = spec;
    reset();

    const double rampSec = 0.02;
    smoothedDrive_.reset(spec.sampleRate, rampSec);
    smoothedKnee_.reset(spec.sampleRate, rampSec);
    smoothedMix_.reset(spec.sampleRate, rampSec);

    smoothedDrive_.setCurrentAndTargetValue(params_.drive);
    smoothedKnee_.setCurrentAndTargetValue(params_.knee);
    smoothedMix_.setCurrentAndTargetValue(params_.mix);

    tapeFilterL_.reset();
    tapeFilterR_.reset();
    juce::dsp::ProcessSpec filterSpec;
    filterSpec.sampleRate = spec.sampleRate;
    filterSpec.maximumBlockSize = spec.maximumBlockSize;
    filterSpec.numChannels = 1;
    tapeFilterL_.prepare(filterSpec);
    tapeFilterR_.prepare(filterSpec);
    tapeFilterL_.setCutoffFrequency(8000.0f);
    tapeFilterR_.setCutoffFrequency(8000.0f);
    tapeFilterL_.setType(juce::dsp::StateVariableTPTFilterType::lowpass);
    tapeFilterR_.setType(juce::dsp::StateVariableTPTFilterType::lowpass);

    ensureOversampler(params_.oversample);
}

void SpaceDustSoftClipper::reset()
{
    if (oversampler_ != nullptr)
        oversampler_->reset();
    tapeFilterL_.reset();
    tapeFilterR_.reset();
}

void SpaceDustSoftClipper::setParameters(const Parameters& p)
{
    params_ = p;
    smoothedDrive_.setTargetValue(p.drive);
    smoothedKnee_.setTargetValue(p.knee);
    smoothedMix_.setTargetValue(p.mix);
}

//==============================================================================
void SpaceDustSoftClipper::process(juce::AudioBuffer<float>& buffer)
{
    if (!params_.enabled || buffer.getNumSamples() == 0)
        return;

    const int numCh = buffer.getNumChannels();
    const int numSamples = buffer.getNumSamples();
    const int mode = juce::jlimit(0, 4, params_.mode);

    // Drive: 0.5-5.0
    const float driveMin = 0.5f;
    const float driveMax = 5.0f;
    const float kneeMin = 0.3f;
    const float kneeMax = 1.0f;

    juce::AudioBuffer<float> dryBuffer;
    const bool needDry = (params_.mix < 0.999f);
    if (needDry)
        dryBuffer.makeCopyOf(buffer);

    ensureOversampler(params_.oversample);

    if (oversampler_ != nullptr && params_.oversample >= 2)
    {
        juce::dsp::AudioBlock<float> block(buffer);
        auto blockUp = oversampler_->processSamplesUp(block);

        const size_t osNumSamples = blockUp.getNumSamples();
        const size_t osFactor = oversampler_->getOversamplingFactor();

        for (size_t i = 0; i < osNumSamples; ++i)
        {
            const int origIdx = static_cast<int>(i / osFactor);
            float drv, kn;
            if (origIdx == 0 || i % osFactor == 0)
            {
                drv = driveMin + (driveMax - driveMin) * smoothedDrive_.getNextValue();
                kn = kneeMin + (kneeMax - kneeMin) * smoothedKnee_.getNextValue();
            }
            else
            {
                drv = driveMin + (driveMax - driveMin) * smoothedDrive_.getCurrentValue();
                kn = kneeMin + (kneeMax - kneeMin) * smoothedKnee_.getCurrentValue();
            }

            for (size_t ch = 0; ch < blockUp.getNumChannels(); ++ch)
            {
                float* ptr = blockUp.getChannelPointer(ch);
                float x = ptr[i];
                float y;
                switch (mode)
                {
                    case 0: y = clipSmooth(x, drv, kn); break;
                    case 1: y = clipCrisp(x, drv, kn);  break;
                    case 2: y = clipTube(x, drv, kn);   break;
                    case 3: y = clipTape(x, drv, kn);   break;
                    default: y = clipGuitar(x, drv, kn); break;
                }
                ptr[i] = y;
            }
        }

        oversampler_->processSamplesDown(block);
    }
    else
    {
        for (int i = 0; i < numSamples; ++i)
        {
            const float drv = driveMin + (driveMax - driveMin) * smoothedDrive_.getNextValue();
            const float kn = kneeMin + (kneeMax - kneeMin) * smoothedKnee_.getNextValue();

            for (int ch = 0; ch < numCh; ++ch)
            {
                auto* ptr = buffer.getWritePointer(ch);
                float x = ptr[i];
                float y;
                switch (mode)
                {
                    case 0: y = clipSmooth(x, drv, kn); break;
                    case 1: y = clipCrisp(x, drv, kn);  break;
                    case 2: y = clipTube(x, drv, kn);   break;
                    case 3: y = clipTape(x, drv, kn);   break;
                    default: y = clipGuitar(x, drv, kn); break;
                }
                ptr[i] = y;
            }
        }
        if (mode == 3)
        {
            for (int ch = 0; ch < numCh; ++ch)
            {
                auto& filt = (ch == 0) ? tapeFilterL_ : tapeFilterR_;
                auto* ptr = buffer.getWritePointer(ch);
                for (int i = 0; i < numSamples; ++i)
                    ptr[i] = filt.processSample(0, ptr[i]);
            }
        }
    }

    // Dry/wet mix
    if (needDry && dryBuffer.getNumSamples() == numSamples)
    {
        for (int i = 0; i < numSamples; ++i)
        {
            const float mix = smoothedMix_.getNextValue();
            for (int ch = 0; ch < numCh; ++ch)
            {
                auto* wet = buffer.getWritePointer(ch);
                const float* dry = dryBuffer.getReadPointer(ch);
                wet[i] = dry[i] * (1.0f - mix) + wet[i] * mix;
            }
        }
    }
}

//==============================================================================
// SpaceDust Bit Crusher - Aggressive lo-fi (1-8 bits, rate crush, asymmetric)
//
// Bit depth: 1-8 bits. Floor quantization + DC bias (0.03) for harsh asymmetry.
// Rate: Zero-order hold downsampling, effective 1-8 kHz when maxed.
// Jitter: Tiny phase noise for less robotic character.
//==============================================================================

#include "SpaceDustBitCrusher.h"
#include <cmath>

namespace
{
    constexpr float kDenormThreshold = 1e-6f;   // Flush tiny values (fixes meter creep)
    constexpr float kDcBias = 0.03f;            // Asymmetric quantization crunch
    constexpr float kSilenceThreshold = 1e-5f;  // Below this: pass through to avoid DC from biasing silence

    // Harsh asymmetric quantization: floor + bias, quant_levels = 2^bits - 1
    inline float quantizeHarsh(float x, int bits, float bias)
    {
        if (std::abs(x) < kSilenceThreshold) return 0.0f;  // Silence stays silent (bias would create DC)
        if (bits <= 0) return (x >= 0.0f) ? 1.0f : -1.0f;
        const int quantLevels = (1 << bits) - 1;
        if (quantLevels <= 0) return (x >= 0.0f) ? 1.0f : -1.0f;
        // Map [-1,1] to [0,1], quantize with floor+bias, map back
        const float norm = x * 0.5f + 0.5f;
        const float q = std::floor(norm * static_cast<float>(quantLevels) + bias);
        const float crushed = (juce::jlimit(0.0f, static_cast<float>(quantLevels), q) / static_cast<float>(quantLevels)) * 2.0f - 1.0f;
        return juce::jlimit(-1.5f, 1.5f, crushed);
    }
}

//==============================================================================
void SpaceDustBitCrusher::prepare(const juce::dsp::ProcessSpec& spec)
{
    spec_ = spec;
    reset();

    const double rampSec = 0.02;
    smoothedMix_.reset(spec.sampleRate, rampSec);
    smoothedAmount_.reset(spec.sampleRate, rampSec);
    smoothedRate_.reset(spec.sampleRate, rampSec);

    smoothedMix_.setCurrentAndTargetValue(params_.mix);
    smoothedAmount_.setCurrentAndTargetValue(params_.amount);
    smoothedRate_.setCurrentAndTargetValue(params_.rate);
}

void SpaceDustBitCrusher::reset()
{
    holdSampleL_ = 0.0f;
    holdSampleR_ = 0.0f;
    phaseL_ = 0.0f;
    phaseR_ = 0.0f;
}

void SpaceDustBitCrusher::setParameters(const Parameters& p)
{
    params_ = p;
    smoothedMix_.setTargetValue(p.mix);
    smoothedAmount_.setTargetValue(p.amount);
    smoothedRate_.setTargetValue(p.rate);
}

//==============================================================================
void SpaceDustBitCrusher::process(juce::AudioBuffer<float>& buffer)
{
    if (!params_.enabled || buffer.getNumSamples() == 0)
        return;

    const int numChannels = buffer.getNumChannels();
    const int numSamples = buffer.getNumSamples();
    const float rateThreshold = 0.01f;  // Below this: full rate, quantize every sample

    for (int i = 0; i < numSamples; ++i)
    {
        const float mix = smoothedMix_.getNextValue();
        const float amount = juce::jlimit(0.0f, 1.0f, smoothedAmount_.getNextValue());
        const float rateParam = juce::jlimit(0.0f, 1.0f, smoothedRate_.getNextValue());

        // Bit depth: 1-8 bits. Power curve (0.35) so Amount responds immediately: 0.15≈5bits, 0.35≈4bits, 0.6≈3bits
        const float effectiveAmount = std::pow(juce::jmax(0.001f, amount), 0.35f);
        const float bitsFloat = 1.0f + (1.0f - effectiveAmount) * 7.0f;
        const int bits = juce::jlimit(1, 8, static_cast<int>(bitsFloat + 0.5f));

        // Rate: 0 = full rate (quantize every sample), 1 = ~1 kHz hold (heavy crush)
        // normfreq 0.023 = 1kHz at 44.1k, 0.18 = 8kHz
        const float normFreq = (rateParam < rateThreshold) ? 1.0f : (0.023f + rateParam * rateParam * 0.157f);
        const float phaseInc = normFreq;

        for (int ch = 0; ch < numChannels; ++ch)
        {
            auto* ptr = buffer.getWritePointer(ch);
            const float dry = ptr[i];

            float crushed;
            if (rateParam < rateThreshold)
            {
                // Full rate: quantize every sample (no hold)
                crushed = quantizeHarsh(dry, bits, kDcBias);
            }
            else
            {
                float held = (ch == 0) ? holdSampleL_ : holdSampleR_;
                float& phase = (ch == 0) ? phaseL_ : phaseR_;

                float phaseIncWithJitter = phaseInc;
                if (rateParam > 0.3f)
                    phaseIncWithJitter += jitterDist_(rng_);

                phase += phaseIncWithJitter;
                if (phase >= 1.0f)
                {
                    phase -= 1.0f;
                    if (phase < 0.0f) phase = 0.0f;
                    held = quantizeHarsh(dry, bits, kDcBias);
                }

                if (ch == 0) holdSampleL_ = held;
                else holdSampleR_ = held;
                crushed = held;
            }

            float out = (1.0f - mix) * dry + mix * crushed;
            if (std::abs(out) < kDenormThreshold) out = 0.0f;
            ptr[i] = out;
        }
    }
}

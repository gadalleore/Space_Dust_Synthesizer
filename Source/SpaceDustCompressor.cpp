//==============================================================================
// SpaceDust Compressor - Multi-Type Compressor
//
// Type 0: SSL G-Bus 4000G - Feed-forward VCA, hard knee, peak detection
// Type 1: 1176 FET - All-FET gain reduction, aggressive harmonics, ultra-fast
//
// Both share: auto-release, built-in soft clipper, parallel mix.
//==============================================================================

#include "SpaceDustCompressor.h"
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

    // dB <-> linear conversions
    inline float dbToLinear(float db) { return std::pow(10.0f, db / 20.0f); }
    inline float linearToDb(float lin)
    {
        return (lin > 1e-10f) ? 20.0f * std::log10(lin) : -200.0f;
    }
}

//==============================================================================
float SpaceDustCompressor::msToCoeff(float ms) const
{
    if (ms <= 0.0f || spec_.sampleRate <= 0.0)
        return 0.0f;
    return std::exp(-1.0f / (ms * 0.001f * static_cast<float>(spec_.sampleRate)));
}

float SpaceDustCompressor::computeGainDb(float inputDb, float threshDb, float ratio) const
{
    // Hard knee (SSL) or soft knee (1176 at high ratios)
    if (inputDb <= threshDb)
        return 0.0f;
    return -(inputDb - threshDb) * (1.0f - 1.0f / ratio);
}

float SpaceDustCompressor::softClipSample(float x)
{
    return fastTanh(x * 1.2f);
}

//==============================================================================
void SpaceDustCompressor::prepare(const juce::dsp::ProcessSpec& spec)
{
    spec_ = spec;
    reset();

    const double rampSec = 0.02;
    smoothedThreshold_.reset(spec.sampleRate, rampSec);
    smoothedRatio_.reset(spec.sampleRate, rampSec);
    smoothedMakeup_.reset(spec.sampleRate, rampSec);
    smoothedMix_.reset(spec.sampleRate, rampSec);

    smoothedThreshold_.setCurrentAndTargetValue(params_.thresholdDb);
    smoothedRatio_.setCurrentAndTargetValue(params_.ratio);
    smoothedMakeup_.setCurrentAndTargetValue(params_.makeupGainDb);
    smoothedMix_.setCurrentAndTargetValue(params_.mix);
}

void SpaceDustCompressor::reset()
{
    envelopeL_ = 0.0f;
    envelopeR_ = 0.0f;
    autoReleaseEnvL_ = 0.0f;
    autoReleaseEnvR_ = 0.0f;
    gainReductionDb_.store(0.0f, std::memory_order_relaxed);
}

void SpaceDustCompressor::setParameters(const Parameters& p)
{
    params_ = p;
    smoothedThreshold_.setTargetValue(p.thresholdDb);
    smoothedRatio_.setTargetValue(p.ratio);
    smoothedMakeup_.setTargetValue(p.makeupGainDb);
    smoothedMix_.setTargetValue(p.mix);
}

//==============================================================================
void SpaceDustCompressor::process(juce::AudioBuffer<float>& buffer)
{
    if (!params_.enabled || buffer.getNumSamples() == 0)
        return;

    const int numCh = buffer.getNumChannels();
    const int numSamples = buffer.getNumSamples();
    const int type = juce::jlimit(0, 2, params_.type);

    // Pre-compute attack/release coefficients
    // 1176: ultra-fast attack (20µs-800µs mapped from user's 0.1-80ms range)
    // SSL: standard attack
    float effectiveAttackMs = params_.attackMs;
    float effectiveReleaseMs = params_.releaseMs;
    if (type == 1) // 1176 FET
    {
        effectiveAttackMs = 0.02f + (params_.attackMs / 80.0f) * 0.78f;
        effectiveReleaseMs = juce::jmax(50.0f, params_.releaseMs);
    }
    else if (type == 2) // LA-2A Opto
    {
        // LA-2A: program-dependent. Attack 1.5-10ms, release 60-500ms.
        // User controls bias the range but the opto cell adapts.
        effectiveAttackMs = 1.5f + (params_.attackMs / 80.0f) * 8.5f;
        effectiveReleaseMs = 60.0f + (params_.releaseMs / 1200.0f) * 440.0f;
    }

    const float attackCoeff = msToCoeff(effectiveAttackMs);
    const float releaseCoeff = msToCoeff(effectiveReleaseMs);

    // Auto-release: wide fast/slow range for audible program-dependent behavior
    const float autoReleaseFast = msToCoeff(type == 1 ? 15.0f : (type == 2 ? 25.0f : 20.0f));
    const float autoReleaseSlow = msToCoeff(type == 1 ? 800.0f : (type == 2 ? 1000.0f : 1200.0f));
    const float autoReleaseAttack = msToCoeff(5.0f);
    const float autoEnvDecay = msToCoeff(type == 1 ? 80.0f : (type == 2 ? 120.0f : 100.0f));
    const bool useAutoRelease = params_.autoRelease || (type == 2);

    // Make a dry copy for parallel mix
    juce::AudioBuffer<float> dryBuffer;
    const bool needDry = (params_.mix < 0.999f);
    if (needDry)
        dryBuffer.makeCopyOf(buffer);

    float peakGrDb = 0.0f;

    for (int i = 0; i < numSamples; ++i)
    {
        const float thresh = smoothedThreshold_.getNextValue();
        const float ratio = juce::jmax(1.0f, smoothedRatio_.getNextValue());
        const float makeupLin = dbToLinear(smoothedMakeup_.getNextValue());

        // --- Stereo linked detection ---
        float inputPeakLin = 0.0f;
        for (int ch = 0; ch < numCh; ++ch)
        {
            float absVal = std::abs(buffer.getSample(ch, i));
            if (absVal > inputPeakLin)
                inputPeakLin = absVal;
        }

        // --- Envelope follower ---
        float& envelope = envelopeL_;

        if (inputPeakLin > envelope)
            envelope = attackCoeff * envelope + (1.0f - attackCoeff) * inputPeakLin;
        else
        {
            if (useAutoRelease)
            {
                float& autoEnv = autoReleaseEnvL_;
                float grAmount = envelope - inputPeakLin;
                if (grAmount > autoEnv)
                    autoEnv = autoReleaseAttack * autoEnv + (1.0f - autoReleaseAttack) * grAmount;
                else
                    autoEnv = autoEnvDecay * autoEnv;

                float blend = juce::jlimit(0.0f, 1.0f, autoEnv * 10.0f);
                float adaptiveRelease = autoReleaseFast * (1.0f - blend) + autoReleaseSlow * blend;
                envelope = adaptiveRelease * envelope + (1.0f - adaptiveRelease) * inputPeakLin;
            }
            else
            {
                envelope = releaseCoeff * envelope + (1.0f - releaseCoeff) * inputPeakLin;
            }
        }

        // --- Gain computer ---
        float envelopeDb = linearToDb(juce::jmax(1e-10f, envelope));
        float grDb;
        if (type == 2) // LA-2A: soft knee
        {
            float excess = envelopeDb - thresh;
            if (excess <= -6.0f)
                grDb = 0.0f;
            else if (excess < 6.0f)
            {
                // Soft knee: gradual onset over 12dB window
                float knee = (excess + 6.0f) / 12.0f;
                grDb = -(knee * knee) * (excess + 6.0f) * (1.0f - 1.0f / ratio) * 0.5f;
            }
            else
                grDb = computeGainDb(envelopeDb, thresh, ratio);
        }
        else
            grDb = computeGainDb(envelopeDb, thresh, ratio);
        float gainLin = dbToLinear(grDb);

        if (grDb < peakGrDb)
            peakGrDb = grDb;

        // --- Apply gain reduction + makeup + type-specific coloring ---
        for (int ch = 0; ch < numCh; ++ch)
        {
            float sample = buffer.getSample(ch, i);
            sample *= gainLin * makeupLin;

            if (type == 1) // 1176 FET
            {
                float fetDrive = 1.0f + juce::jmax(0.0f, -grDb) * 0.015f;
                float driven = sample * fetDrive;
                // Asymmetric: positive half is slightly harder clipped (FET characteristic)
                if (driven >= 0.0f)
                    sample = fastTanh(driven * 1.05f);
                else
                    sample = fastTanh(driven * 0.95f) * 1.02f;
            }
            else if (type == 2) // LA-2A Opto: gentle 2nd-harmonic tube warmth
            {
                float tubeDrive = 1.0f + juce::jmax(0.0f, -grDb) * 0.008f;
                sample = sample * tubeDrive;
                sample = fastTanh(sample) * 0.98f + sample * 0.02f;
            }

            // Built-in soft clipper (output stage)
            if (params_.softClip)
                sample = softClipSample(sample);

            buffer.setSample(ch, i, sample);
        }
    }

    // --- Parallel dry/wet mix ---
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

    gainReductionDb_.store(peakGrDb, std::memory_order_relaxed);
}

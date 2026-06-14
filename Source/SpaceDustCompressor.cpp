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
    detectorEnv_ = 0.0f;
    grEnvDb_ = 0.0f;
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
    // Each type keeps its CHARACTER via a different attack/release range, but every
    // range now spans an audibly different sweep across the full knob travel (the old
    // ranges were so narrow — e.g. 1176 attack 0.02-0.8ms — that both ends sounded
    // identical and the knob felt dead). attackMs is 0.1-80, releaseMs is 5-1200.
    float effectiveAttackMs = params_.attackMs;   // SSL: direct 0.1-80 ms
    float effectiveReleaseMs = params_.releaseMs;  // SSL: direct 5-1200 ms
    if (type == 1) // 1176 FET: fast-leaning, but the whole knob is audible
    {
        effectiveAttackMs = 0.05f + (params_.attackMs / 80.0f) * 20.0f;        // 0.05 - 20 ms
        effectiveReleaseMs = juce::jmax(10.0f, params_.releaseMs);             // 10 - 1200 ms
    }
    else if (type == 2) // LA-2A Opto: slow-leaning, but the whole knob is audible
    {
        effectiveAttackMs = 1.5f + (params_.attackMs / 80.0f) * 30.0f;         // 1.5 - 31.5 ms
        effectiveReleaseMs = 60.0f + (params_.releaseMs / 1200.0f) * 1140.0f;  // 60 - 1200 ms
    }

    // User attack/release act on the GAIN reduction (gain-domain ballistics).
    const float attackCoeff = msToCoeff(effectiveAttackMs);
    const float releaseCoeff = msToCoeff(effectiveReleaseMs);

    // Level-detector release "hold": a short FIXED time so the detected peak level
    // stays stable across each waveform cycle (doesn't collapse to zero at every
    // zero crossing). Attack is instantaneous (true peak). This is separate from
    // the user's compressor attack/release, which now live in the gain domain.
    const float detectorReleaseCoeff = msToCoeff(type == 1 ? 2.0f : 5.0f);

    // Auto-release: program-dependent gain recovery. Deeper gain reduction →
    // slower recovery (musical "hold"); shallow GR → faster recovery.
    const float autoReleaseFast = msToCoeff(type == 1 ? 60.0f : (type == 2 ? 120.0f : 80.0f));
    const float autoReleaseSlow = msToCoeff(type == 1 ? 800.0f : (type == 2 ? 1000.0f : 1200.0f));
    // Auto-release is now an opt-in toggle for ALL types. It used to be force-enabled
    // for type 2 (LA-2A), which silently bypassed that compressor's Release knob.
    const bool useAutoRelease = params_.autoRelease;

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

        // --- Stereo linked peak detection ---
        float inputPeakLin = 0.0f;
        for (int ch = 0; ch < numCh; ++ch)
        {
            float absVal = std::abs(buffer.getSample(ch, i));
            if (absVal > inputPeakLin)
                inputPeakLin = absVal;
        }

        // --- Level detector: instant-attack peak with a short release hold ---
        // Gives a stable per-cycle level so the gain computer below sees the note
        // level, not the instantaneous waveform shape.
        if (inputPeakLin > detectorEnv_)
            detectorEnv_ = inputPeakLin;                                   // instant attack
        else
            detectorEnv_ = detectorReleaseCoeff * detectorEnv_
                         + (1.0f - detectorReleaseCoeff) * inputPeakLin;    // short hold

        const float levelDb = linearToDb(juce::jmax(1e-10f, detectorEnv_));

        // --- Gain computer: TARGET gain reduction (dB, <= 0) for this level ---
        float targetGrDb;
        if (type == 2) // LA-2A: soft knee
        {
            float excess = levelDb - thresh;
            if (excess <= -6.0f)
                targetGrDb = 0.0f;
            else if (excess < 6.0f)
            {
                // Soft knee: gradual onset over 12dB window
                float knee = (excess + 6.0f) / 12.0f;
                targetGrDb = -(knee * knee) * (excess + 6.0f) * (1.0f - 1.0f / ratio) * 0.5f;
            }
            else
                targetGrDb = computeGainDb(levelDb, thresh, ratio);
        }
        else
            targetGrDb = computeGainDb(levelDb, thresh, ratio);

        // --- Ballistics on the GAIN REDUCTION (this is what makes the user's
        //     attack/release knobs audible) ---
        if (targetGrDb < grEnvDb_)
        {
            // Need MORE reduction → ATTACK (how fast GR clamps down).
            grEnvDb_ = attackCoeff * grEnvDb_ + (1.0f - attackCoeff) * targetGrDb;
        }
        else
        {
            // Recover toward less reduction → RELEASE (how fast GR lets go).
            if (useAutoRelease)
            {
                // Program-dependent: deeper current GR recovers slower.
                const float depth = juce::jlimit(0.0f, 1.0f, -grEnvDb_ / 12.0f);
                const float adaptiveRelease = autoReleaseFast * (1.0f - depth)
                                            + autoReleaseSlow * depth;
                grEnvDb_ = adaptiveRelease * grEnvDb_ + (1.0f - adaptiveRelease) * targetGrDb;
            }
            else
            {
                grEnvDb_ = releaseCoeff * grEnvDb_ + (1.0f - releaseCoeff) * targetGrDb;
            }
        }

        const float grDb = grEnvDb_;
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

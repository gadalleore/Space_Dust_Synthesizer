//==============================================================================
// SpaceDust Trance Gate - Tempo-synced rhythmic volume gating
//
// 8-step pattern sequencer with 4/8/16 step modes. Tempo sync via AudioPlayHead
// or free-running Hz. Attack/release smoothing for click-free gating.
//==============================================================================

#include "SpaceDustTranceGate.h"
#include <cmath>

#ifndef juce_PI
#define juce_PI 3.14159265358979323846
#endif

namespace
{
    constexpr float kDenormThreshold = 1e-15f;
}

//==============================================================================
void SpaceDustTranceGate::prepare(const juce::dsp::ProcessSpec& spec)
{
    spec_ = spec;
    reset();
}

void SpaceDustTranceGate::reset()
{
    phase_ = 0.0;
    smoothedEnv_ = 0.0f;
}

void SpaceDustTranceGate::setParameters(const Parameters& p)
{
    params_ = p;
}

//==============================================================================
float SpaceDustTranceGate::getStepValue(float phase) const
{
    // phase in [0, 1) - position in one pattern cycle
    const int steps = juce::jlimit(4, 16, params_.numSteps);
    const float stepSize = 1.0f / static_cast<float>(steps);
    const int stepIndex = static_cast<int>(std::floor(phase / stepSize)) % steps;
    return params_.stepOn[stepIndex] ? 1.0f : 0.0f;
}

float SpaceDustTranceGate::smoothEnvelope(float raw, float current, bool rising) const
{
    // One-pole RC smoother: different coefficients for attack vs release
    const float sr = static_cast<float>(spec_.sampleRate);
    const float attackSec = juce::jlimit(0.0001f, 0.1f, params_.attackMs * 0.001f);
    const float releaseSec = juce::jlimit(0.0001f, 0.1f, params_.releaseMs * 0.001f);
    // Resistance * capacitance style: alpha = 1 / (1 + rc*sr), rc ~ time constant
    const float attackAlpha = 1.0f / (1.0f + attackSec * sr * 0.25f);
    const float releaseAlpha = 1.0f / (1.0f + releaseSec * sr * 0.25f);
    const float alpha = rising ? attackAlpha : releaseAlpha;
    return current + alpha * (raw - current);
}

//==============================================================================
void SpaceDustTranceGate::process(juce::AudioBuffer<float>& buffer, double sampleRate,
                                  juce::AudioPlayHead* playHead)
{
    if (!params_.enabled || buffer.getNumSamples() == 0)
        return;

    const int numChannels = juce::jmin(buffer.getNumChannels(), 2);
    const int numSamples = buffer.getNumSamples();
    const float sr = static_cast<float>(sampleRate);

    // Rate / phase increment
    double phaseIncrement = 0.0;
    double phaseStart = phase_;

    if (params_.sync && playHead != nullptr)
    {
        auto posInfo = playHead->getPosition();
        if (posInfo.hasValue())
        {
            double tempo = 120.0;
            if (posInfo->getBpm().hasValue() && *posInfo->getBpm() > 0.0)
                tempo = *posInfo->getBpm();

            float rateClamped = juce::jlimit(0.0f, 12.0f, params_.rate);
            int musicalIndex = static_cast<int>(std::round(rateClamped * 8.0f / 12.0f));
            musicalIndex = juce::jlimit(0, 8, musicalIndex);
            static const double multipliers[9] = {
                8.0, 4.0, 2.0, 1.0, 0.5, 0.25, 0.125, 0.0625, 0.03125
            };
            double beatsPerCycle = multipliers[musicalIndex];
            double secondsPerBeat = 60.0 / tempo;
            double secondsPerCycle = beatsPerCycle * secondsPerBeat;
            double samplesPerCycle = secondsPerCycle * sr;
            phaseIncrement = 1.0 / samplesPerCycle;

            if (posInfo->getIsPlaying() && posInfo->getPpqPosition().hasValue())
            {
                // Host playing: align phase to beat position
                double ppq = *posInfo->getPpqPosition();
                phaseStart = std::fmod(ppq / beatsPerCycle, 1.0);
                if (phaseStart < 0.0) phaseStart += 1.0;
            }
            else
            {
                // Host stopped or no PPQ: use accumulated phase so gate still runs
                phaseStart = phase_;
            }
        }
    }

    if (phaseIncrement <= 0.0)
    {
        // Free mode: rate 0-12 maps to ~0.1-20 Hz log
        float rateClamped = juce::jlimit(0.0f, 12.0f, params_.rate);
        float norm = rateClamped / 12.0f;
        float logMin = std::log(0.1f);
        float logMax = std::log(20.0f);
        float hz = std::exp(logMin + norm * (logMax - logMin));
        hz = juce::jlimit(0.1f, 20.0f, hz);
        phaseIncrement = static_cast<double>(hz) / sr;
    }

    const float mix = juce::jlimit(0.0f, 1.0f, params_.mix);
    double phase = phaseStart;

    for (int i = 0; i < numSamples; ++i)
    {
        phase += phaseIncrement;
        if (phase >= 1.0) phase -= 1.0;
        if (phase < 0.0) phase += 1.0;

        const float rawEnv = getStepValue(static_cast<float>(phase));
        const bool rising = rawEnv > smoothedEnv_;
        smoothedEnv_ = smoothEnvelope(rawEnv, smoothedEnv_, rising);

        const float env = mix * smoothedEnv_ + (1.0f - mix);
        const float gain = juce::jlimit(0.0f, 1.0f, env);

        for (int ch = 0; ch < numChannels; ++ch)
        {
            float* data = buffer.getWritePointer(ch);
            data[i] *= gain;
            if (std::abs(data[i]) < kDenormThreshold)
                data[i] = 0.0f;
        }
    }

    phase_ = phase;
}

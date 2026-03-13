//==============================================================================
// SpaceDust Transient - TR-808/909 analog drum transient synthesizer
//
// Emulates the bridged-T resonator (kick), noise-shaped envelopes (snare),
// metallic square-wave mixing (hats/cowbell), and filtered noise bursts (clap)
// from the original Roland TR-808 and TR-909 drum machines.
//==============================================================================

#include "SpaceDustTransient.h"
#include <cmath>

namespace
{
    constexpr double kTwoPi = 6.283185307179586;
    constexpr float kDenormThreshold = 1e-7f;

    // 808 hat metallic frequencies (Hz) - 6 inharmonic square waves
    constexpr double kHatFreqs[6] = { 204.68, 295.64, 365.54, 430.08, 540.54, 800.00 };

    // 808 cowbell frequencies (Hz)
    constexpr double kCowbellFreqs[2] = { 540.0, 800.0 };

    inline float fastTanh(float x)
    {
        x = juce::jlimit(-9.0f, 9.0f, x);
        const float x2 = x * x;
        return x * (27.0f + x2) / (27.0f + 9.0f * x2);
    }

    inline float semitonesToRatio(float semitones)
    {
        return std::pow(2.0f, semitones / 12.0f);
    }
}

//==============================================================================
void SpaceDustTransient::prepare(const juce::dsp::ProcessSpec& spec)
{
    spec_ = spec;
    sampleRate_ = spec.sampleRate;
    reset();

    smoothedMix_.reset(spec.sampleRate, 0.02);
    smoothedMix_.setCurrentAndTargetValue(params_.mix);

    juce::dsp::ProcessSpec monoSpec;
    monoSpec.sampleRate = spec.sampleRate;
    monoSpec.maximumBlockSize = spec.maximumBlockSize;
    monoSpec.numChannels = 1;

    bpFilter_.prepare(monoSpec);
    bpFilter_.setType(juce::dsp::StateVariableTPTFilterType::bandpass);
    bpFilter_.setCutoffFrequency(4000.0f);
    bpFilter_.setResonance(1.5f);

    hpFilter_.prepare(monoSpec);
    hpFilter_.setType(juce::dsp::StateVariableTPTFilterType::highpass);
    hpFilter_.setCutoffFrequency(7000.0f);
    hpFilter_.setResonance(1.0f);
}

void SpaceDustTransient::reset()
{
    triggered_ = false;
    sampleCounter_ = 0;
    oscPhase1_ = 0.0;
    oscPhase2_ = 0.0;
    for (int i = 0; i < 6; ++i) sqPhase_[i] = 0.0;
    ampEnv_ = 0.0f;
    pitchEnv_ = 0.0f;
    noiseEnv_ = 0.0f;
    clapBurstCount_ = 0;
    clapBurstTimer_ = 0;
    clapBurstEnv_ = 0.0f;
    bpFilter_.reset();
    hpFilter_.reset();
}

void SpaceDustTransient::setParameters(const Parameters& p)
{
    params_ = p;
    smoothedMix_.setTargetValue(p.mix);
}

float SpaceDustTransient::getKaDonkDelaySamples() const
{
    return params_.kaDonk * static_cast<float>(sampleRate_);
}

//==============================================================================
void SpaceDustTransient::trigger(int midiNoteNumber)
{
    triggered_ = true;
    sampleCounter_ = 0;
    oscPhase1_ = 0.0;
    oscPhase2_ = 0.0;
    for (int i = 0; i < 6; ++i) sqPhase_[i] = 0.0;
    clapBurstCount_ = 0;
    clapBurstTimer_ = 0;
    clapBurstEnv_ = 0.0f;

    // Clamp note to middle C octave (C4=60 to B4=71)
    int pitchClass = midiNoteNumber % 12;
    int clampedNote = 60 + pitchClass;

    // Compute actual note frequency in octave 4, then apply coarse offset
    noteFreq_ = 440.0f * std::pow(2.0f, (static_cast<float>(clampedNote) - 69.0f) / 12.0f);
    noteFreq_ *= semitonesToRatio(params_.coarse);

    // pitchMultiplier_ used for filter/metallic scaling (ratio from C4)
    pitchMultiplier_ = noteFreq_ / 261.63f;

    bpFilter_.reset();
    hpFilter_.reset();

    initDrumEnvelopes(params_.type);
}

void SpaceDustTransient::initDrumEnvelopes(int type)
{
    const float sr = static_cast<float>(sampleRate_);

    // Length scales all decay times: 1.0 = full, near 0 = short click
    // Use squared curve so the knob feels musical (more range near the top)
    const float len = juce::jmax(0.02f, params_.length * params_.length);

    auto decay = [&](float baseTimeSec) {
        return std::exp(-1.0f / (sr * baseTimeSec * len));
    };

    switch (type)
    {
        case Kick808:
            ampEnv_ = 1.0f;
            ampDecayRate_ = decay(0.35f);
            pitchEnv_ = 1.0f;
            pitchDecayRate_ = decay(0.006f);
            noiseEnv_ = 0.8f;
            noiseDecayRate_ = decay(0.003f);
            break;

        case Snare808:
            ampEnv_ = 1.0f;
            ampDecayRate_ = decay(0.18f);
            pitchEnv_ = 0.0f;
            pitchDecayRate_ = 0.0f;
            noiseEnv_ = 1.0f;
            noiseDecayRate_ = decay(0.15f);
            bpFilter_.setCutoffFrequency(juce::jlimit(200.0f, 18000.0f, noteFreq_ * 8.0f));
            bpFilter_.setResonance(1.2f);
            break;

        case ClosedHat808:
            ampEnv_ = 1.0f;
            ampDecayRate_ = decay(0.04f);
            noiseEnv_ = 0.6f;
            noiseDecayRate_ = decay(0.03f);
            hpFilter_.setCutoffFrequency(juce::jlimit(200.0f, 18000.0f, noteFreq_ * 16.0f));
            hpFilter_.setResonance(1.0f);
            break;

        case OpenHat808:
            ampEnv_ = 1.0f;
            ampDecayRate_ = decay(0.30f);
            noiseEnv_ = 0.5f;
            noiseDecayRate_ = decay(0.25f);
            hpFilter_.setCutoffFrequency(juce::jlimit(200.0f, 18000.0f, noteFreq_ * 16.0f));
            hpFilter_.setResonance(0.8f);
            break;

        case Clap808:
            ampEnv_ = 1.0f;
            ampDecayRate_ = decay(0.20f);
            noiseEnv_ = 1.0f;
            noiseDecayRate_ = decay(0.18f);
            clapBurstCount_ = juce::jmax(1, static_cast<int>(4.0f * params_.length));
            clapBurstTimer_ = 0;
            clapBurstEnv_ = 1.0f;
            bpFilter_.setCutoffFrequency(juce::jlimit(200.0f, 18000.0f, noteFreq_ * 4.0f));
            bpFilter_.setResonance(2.0f);
            break;

        case Tom808:
            ampEnv_ = 1.0f;
            ampDecayRate_ = decay(0.22f);
            pitchEnv_ = 1.0f;
            pitchDecayRate_ = decay(0.008f);
            noiseEnv_ = 0.3f;
            noiseDecayRate_ = decay(0.005f);
            break;

        case Rim808:
            ampEnv_ = 1.0f;
            ampDecayRate_ = decay(0.015f);
            pitchEnv_ = 0.5f;
            pitchDecayRate_ = decay(0.002f);
            noiseEnv_ = 1.0f;
            noiseDecayRate_ = decay(0.008f);
            break;

        case Cowbell808:
            ampEnv_ = 1.0f;
            ampDecayRate_ = decay(0.08f);
            noiseEnv_ = 0.0f;
            noiseDecayRate_ = 0.0f;
            pitchEnv_ = 0.0f;
            pitchDecayRate_ = 0.0f;
            bpFilter_.setCutoffFrequency(juce::jlimit(200.0f, 18000.0f, noteFreq_ * 2.0f));
            bpFilter_.setResonance(3.0f);
            break;

        case Kick909:
            ampEnv_ = 1.0f;
            ampDecayRate_ = decay(0.30f);
            pitchEnv_ = 1.0f;
            pitchDecayRate_ = decay(0.004f);
            noiseEnv_ = 1.0f;
            noiseDecayRate_ = decay(0.001f);
            break;

        case Snare909:
            ampEnv_ = 1.0f;
            ampDecayRate_ = decay(0.16f);
            pitchEnv_ = 0.3f;
            pitchDecayRate_ = decay(0.003f);
            noiseEnv_ = 1.0f;
            noiseDecayRate_ = decay(0.12f);
            bpFilter_.setCutoffFrequency(juce::jlimit(200.0f, 18000.0f, noteFreq_ * 12.0f));
            bpFilter_.setResonance(1.0f);
            break;

        default:
            ampEnv_ = 0.0f;
            break;
    }
}

//==============================================================================
// 808 Kick: bridged-T resonator emulation
// Fundamental = noteFreq / 4 (octave 2 range, classic 808 territory)
float SpaceDustTransient::generateKick808()
{
    const float baseFreq = noteFreq_ * 0.25f;
    const float sweepAmount = 2.6f;

    float currentFreq = baseFreq * (1.0f + sweepAmount * pitchEnv_);
    pitchEnv_ *= pitchDecayRate_;

    double phaseInc = currentFreq / sampleRate_;
    oscPhase1_ += phaseInc;
    if (oscPhase1_ >= 1.0) oscPhase1_ -= 1.0;

    float sine = std::sin(static_cast<float>(oscPhase1_ * kTwoPi));

    float click = 0.0f;
    if (noiseEnv_ > 0.01f)
    {
        click = noiseDist_(noiseRng_) * noiseEnv_ * 0.4f;
        noiseEnv_ *= noiseDecayRate_;
    }

    float out = (sine + click) * ampEnv_;
    ampEnv_ *= ampDecayRate_;

    return fastTanh(out * 1.3f);
}

//==============================================================================
// 808 Snare: shell resonators at noteFreq/2 and noteFreq + filtered noise
float SpaceDustTransient::generateSnare808()
{
    const float shellFreq1 = noteFreq_ * 0.5f;
    const float shellFreq2 = noteFreq_;

    double phaseInc1 = shellFreq1 / sampleRate_;
    double phaseInc2 = shellFreq2 / sampleRate_;
    oscPhase1_ += phaseInc1;
    oscPhase2_ += phaseInc2;
    if (oscPhase1_ >= 1.0) oscPhase1_ -= 1.0;
    if (oscPhase2_ >= 1.0) oscPhase2_ -= 1.0;

    float shell1 = std::sin(static_cast<float>(oscPhase1_ * kTwoPi)) * 0.5f;
    float shell2 = std::sin(static_cast<float>(oscPhase2_ * kTwoPi)) * 0.3f;
    float shells = (shell1 + shell2) * ampEnv_;

    float noise = noiseDist_(noiseRng_) * noiseEnv_;
    noise = bpFilter_.processSample(0, noise);

    noiseEnv_ *= noiseDecayRate_;
    ampEnv_ *= ampDecayRate_;

    return fastTanh((shells + noise * 0.7f) * 1.2f);
}

//==============================================================================
// 808 Closed Hat: 6 metallic square waves scaled to note, HP filtered
float SpaceDustTransient::generateClosedHat808()
{
    float metallic = 0.0f;
    for (int i = 0; i < 6; ++i)
    {
        double freq = kHatFreqs[i] * pitchMultiplier_;
        double phaseInc = freq / sampleRate_;
        sqPhase_[i] += phaseInc;
        if (sqPhase_[i] >= 1.0) sqPhase_[i] -= 1.0;
        metallic += (sqPhase_[i] < 0.5) ? 1.0f : -1.0f;
    }
    metallic /= 6.0f;

    float noise = noiseDist_(noiseRng_) * noiseEnv_ * 0.3f;
    noiseEnv_ *= noiseDecayRate_;

    float out = (metallic + noise) * ampEnv_;
    ampEnv_ *= ampDecayRate_;

    out = hpFilter_.processSample(0, out);
    return fastTanh(out * 1.5f);
}

//==============================================================================
// 808 Open Hat: same metallic character, longer decay
float SpaceDustTransient::generateOpenHat808()
{
    float metallic = 0.0f;
    for (int i = 0; i < 6; ++i)
    {
        double freq = kHatFreqs[i] * pitchMultiplier_;
        double phaseInc = freq / sampleRate_;
        sqPhase_[i] += phaseInc;
        if (sqPhase_[i] >= 1.0) sqPhase_[i] -= 1.0;
        metallic += (sqPhase_[i] < 0.5) ? 1.0f : -1.0f;
    }
    metallic /= 6.0f;

    float noise = noiseDist_(noiseRng_) * noiseEnv_ * 0.25f;
    noiseEnv_ *= noiseDecayRate_;

    float out = (metallic + noise) * ampEnv_;
    ampEnv_ *= ampDecayRate_;

    out = hpFilter_.processSample(0, out);
    return fastTanh(out * 1.3f);
}

//==============================================================================
// 808 Clap: multiple filtered noise bursts + longer tail
float SpaceDustTransient::generateClap808()
{
    float noise = noiseDist_(noiseRng_);

    const int burstLenSamples = static_cast<int>(sampleRate_ * 0.005);
    const int burstGapSamples = static_cast<int>(sampleRate_ * 0.015);

    float burstGain = 0.0f;
    if (clapBurstCount_ > 0)
    {
        clapBurstTimer_++;
        if (clapBurstTimer_ <= burstLenSamples)
        {
            burstGain = clapBurstEnv_;
            clapBurstEnv_ *= 0.998f;
        }
        else if (clapBurstTimer_ >= burstLenSamples + burstGapSamples)
        {
            clapBurstTimer_ = 0;
            clapBurstCount_--;
            clapBurstEnv_ = 0.8f;
        }
    }

    float tailNoise = noise * noiseEnv_;
    noiseEnv_ *= noiseDecayRate_;

    float out = (noise * burstGain + tailNoise * 0.6f);
    out = bpFilter_.processSample(0, out);
    out *= ampEnv_;
    ampEnv_ *= ampDecayRate_;

    return fastTanh(out * 1.5f);
}

//==============================================================================
// 808 Tom: sine at noteFreq/2 with pitch sweep
float SpaceDustTransient::generateTom808()
{
    const float baseFreq = noteFreq_ * 0.5f;
    const float sweepAmount = 1.8f;

    float currentFreq = baseFreq * (1.0f + sweepAmount * pitchEnv_);
    pitchEnv_ *= pitchDecayRate_;

    double phaseInc = currentFreq / sampleRate_;
    oscPhase1_ += phaseInc;
    if (oscPhase1_ >= 1.0) oscPhase1_ -= 1.0;

    float sine = std::sin(static_cast<float>(oscPhase1_ * kTwoPi));

    float click = noiseDist_(noiseRng_) * noiseEnv_ * 0.2f;
    noiseEnv_ *= noiseDecayRate_;

    float out = (sine + click) * ampEnv_;
    ampEnv_ *= ampDecayRate_;

    return fastTanh(out * 1.2f);
}

//==============================================================================
// 808 Rimshot: click at noteFreq*4 + body at noteFreq
float SpaceDustTransient::generateRim808()
{
    const float freq = noteFreq_ * 4.0f;

    double phaseInc = freq / sampleRate_;
    oscPhase1_ += phaseInc;
    if (oscPhase1_ >= 1.0) oscPhase1_ -= 1.0;

    float sine = std::sin(static_cast<float>(oscPhase1_ * kTwoPi)) * 0.6f;

    const float freq2 = noteFreq_;
    double phaseInc2 = freq2 / sampleRate_;
    oscPhase2_ += phaseInc2;
    if (oscPhase2_ >= 1.0) oscPhase2_ -= 1.0;
    float body = std::sin(static_cast<float>(oscPhase2_ * kTwoPi)) * 0.4f;

    float noise = noiseDist_(noiseRng_) * noiseEnv_ * 0.5f;
    noiseEnv_ *= noiseDecayRate_;

    float out = (sine + body + noise) * ampEnv_;
    ampEnv_ *= ampDecayRate_;

    float pitchClick = pitchEnv_ * noiseDist_(noiseRng_) * 0.3f;
    pitchEnv_ *= pitchDecayRate_;

    return fastTanh((out + pitchClick) * 2.0f);
}

//==============================================================================
// 808 Cowbell: two square waves at noteFreq and noteFreq*1.498 (inharmonic)
float SpaceDustTransient::generateCowbell808()
{
    const double cowbellFreqs[2] = { noteFreq_, noteFreq_ * 1.498 };

    float sq = 0.0f;
    for (int i = 0; i < 2; ++i)
    {
        double freq = cowbellFreqs[i];
        double phaseInc = freq / sampleRate_;
        sqPhase_[i] += phaseInc;
        if (sqPhase_[i] >= 1.0) sqPhase_[i] -= 1.0;
        sq += (sqPhase_[i] < 0.5) ? 1.0f : -1.0f;
    }
    sq *= 0.5f;

    float out = bpFilter_.processSample(0, sq) * ampEnv_;
    ampEnv_ *= ampDecayRate_;

    return fastTanh(out * 1.5f);
}

//==============================================================================
// 909 Kick: punchier, fundamental = noteFreq / 4
float SpaceDustTransient::generateKick909()
{
    const float baseFreq = noteFreq_ * 0.25f;
    const float sweepAmount = 2.2f;

    float currentFreq = baseFreq * (1.0f + sweepAmount * pitchEnv_);
    pitchEnv_ *= pitchDecayRate_;

    double phaseInc = currentFreq / sampleRate_;
    oscPhase1_ += phaseInc;
    if (oscPhase1_ >= 1.0) oscPhase1_ -= 1.0;

    float sine = std::sin(static_cast<float>(oscPhase1_ * kTwoPi));

    float click = 0.0f;
    if (noiseEnv_ > 0.01f)
    {
        float clickOsc = std::sin(static_cast<float>(oscPhase1_ * kTwoPi * 4.0));
        click = (noiseDist_(noiseRng_) * 0.3f + clickOsc * 0.7f) * noiseEnv_;
        noiseEnv_ *= noiseDecayRate_;
    }

    float out = (sine * 0.9f + click * 0.5f) * ampEnv_;
    ampEnv_ *= ampDecayRate_;

    return fastTanh(out * 1.5f);
}

//==============================================================================
// 909 Snare: shells at noteFreq/2 and noteFreq, aggressive noise
float SpaceDustTransient::generateSnare909()
{
    const float shellFreq1 = noteFreq_ * 0.5f;
    const float shellFreq2 = noteFreq_;

    double phaseInc1 = shellFreq1 / sampleRate_;
    double phaseInc2 = shellFreq2 / sampleRate_;
    oscPhase1_ += phaseInc1;
    oscPhase2_ += phaseInc2;
    if (oscPhase1_ >= 1.0) oscPhase1_ -= 1.0;
    if (oscPhase2_ >= 1.0) oscPhase2_ -= 1.0;

    float shell1 = std::sin(static_cast<float>(oscPhase1_ * kTwoPi)) * 0.4f;
    float shell2 = std::sin(static_cast<float>(oscPhase2_ * kTwoPi)) * 0.3f;

    float pitchMod = 1.0f + pitchEnv_ * 0.5f;
    pitchEnv_ *= pitchDecayRate_;
    float shells = (shell1 + shell2) * pitchMod * ampEnv_;

    float noise = noiseDist_(noiseRng_) * noiseEnv_;
    noise = bpFilter_.processSample(0, noise);

    noiseEnv_ *= noiseDecayRate_;
    ampEnv_ *= ampDecayRate_;

    return fastTanh((shells + noise * 0.9f) * 1.4f);
}

//==============================================================================
void SpaceDustTransient::process(juce::AudioBuffer<float>& buffer)
{
    if (!params_.enabled || !triggered_)
        return;

    const int numChannels = buffer.getNumChannels();
    const int numSamples = buffer.getNumSamples();

    // Stop generating once envelope is essentially silent
    if (ampEnv_ < kDenormThreshold && noiseEnv_ < kDenormThreshold
        && clapBurstCount_ <= 0)
    {
        triggered_ = false;
        return;
    }

    for (int i = 0; i < numSamples; ++i)
    {
        const float mix = smoothedMix_.getNextValue();

        float sample = 0.0f;

        switch (params_.type)
        {
            case Kick808:     sample = generateKick808();     break;
            case Snare808:    sample = generateSnare808();    break;
            case ClosedHat808:sample = generateClosedHat808();break;
            case OpenHat808:  sample = generateOpenHat808();  break;
            case Clap808:     sample = generateClap808();     break;
            case Tom808:      sample = generateTom808();      break;
            case Rim808:      sample = generateRim808();      break;
            case Cowbell808:  sample = generateCowbell808();  break;
            case Kick909:     sample = generateKick909();     break;
            case Snare909:    sample = generateSnare909();    break;
            default: break;
        }

        sample *= mix;

        if (std::abs(sample) < kDenormThreshold)
            sample = 0.0f;

        for (int ch = 0; ch < numChannels; ++ch)
        {
            auto* ptr = buffer.getWritePointer(ch);
            ptr[i] += sample;
        }

        sampleCounter_++;
    }
}

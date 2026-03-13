//==============================================================================
// SpaceDust Lo-Fi - RC-20 Retro Color style lo-fi processor
//
// Single "amount" knob progressively engages:
// LP rolloff, bit crush, sample rate reduction, noise, flutter, saturation.
//==============================================================================

#include "SpaceDustLofi.h"
#include <cmath>

namespace
{
    inline float fastTanh(float x)
    {
        x = juce::jlimit(-9.0f, 9.0f, x);
        const float x2 = x * x;
        return x * (27.0f + x2) / (27.0f + 9.0f * x2);
    }
}

//==============================================================================
void SpaceDustLofi::prepare(const juce::dsp::ProcessSpec& spec)
{
    spec_ = spec;
    reset();

    smoothedAmount_.reset(spec.sampleRate, 0.05);
    smoothedAmount_.setCurrentAndTargetValue(params_.amount);

    // Envelope follower: fast attack (~1ms), slow release (~150ms)
    const float sr = static_cast<float>(spec.sampleRate);
    envAttackCoeff_  = 1.0f - std::exp(-1.0f / (sr * 0.001f));
    envReleaseCoeff_ = 1.0f - std::exp(-1.0f / (sr * 0.150f));

    juce::dsp::ProcessSpec monoSpec;
    monoSpec.sampleRate = spec.sampleRate;
    monoSpec.maximumBlockSize = spec.maximumBlockSize;
    monoSpec.numChannels = 1;

    lpFilterL_.prepare(monoSpec);
    lpFilterR_.prepare(monoSpec);
    lpFilterL_.setType(juce::dsp::StateVariableTPTFilterType::lowpass);
    lpFilterR_.setType(juce::dsp::StateVariableTPTFilterType::lowpass);
    lpFilterL_.setCutoffFrequency(20000.0f);
    lpFilterR_.setCutoffFrequency(20000.0f);
    lpFilterL_.setResonance(0.707f);
    lpFilterR_.setResonance(0.707f);
}

void SpaceDustLofi::reset()
{
    lpFilterL_.reset();
    lpFilterR_.reset();
    flutterPhase_ = 0.0f;
    wowPhase_ = 0.0f;
    holdL_ = 0.0f;
    holdR_ = 0.0f;
    holdCounter_ = 0.0f;
    envelopeLevel_ = 0.0f;
}

void SpaceDustLofi::setParameters(const Parameters& p)
{
    params_ = p;
    smoothedAmount_.setTargetValue(p.amount);
}

//==============================================================================
void SpaceDustLofi::process(juce::AudioBuffer<float>& buffer)
{
    if (!params_.enabled || buffer.getNumSamples() == 0)
        return;

    const int numCh = buffer.getNumChannels();
    const int numSamples = buffer.getNumSamples();
    const float sampleRate = static_cast<float>(spec_.sampleRate);

    for (int i = 0; i < numSamples; ++i)
    {
        const float amount = smoothedAmount_.getNextValue();
        if (amount < 0.001f)
            continue; // Skip processing when amount is essentially zero

        // ====================================================================
        // 1. LP Filter Rolloff (20kHz → 2kHz as amount increases)
        //    Starts engaging immediately, very musical
        // ====================================================================
        float filterCutoff = 20000.0f * std::pow(0.1f, amount); // 20k → 2k
        filterCutoff = juce::jlimit(1500.0f, 20000.0f, filterCutoff);
        // Slight resonance bump at higher amounts (like RC-20's filter character)
        float filterQ = 0.707f + amount * 0.3f;
        lpFilterL_.setCutoffFrequency(filterCutoff);
        lpFilterR_.setCutoffFrequency(filterCutoff);
        lpFilterL_.setResonance(filterQ);
        lpFilterR_.setResonance(filterQ);

        // ====================================================================
        // 2. Tape Wow & Flutter
        // ====================================================================
        float flutterAmount = juce::jmax(0.0f, (amount - 0.2f) / 0.8f);
        flutterPhase_ += 6.0f / sampleRate;
        if (flutterPhase_ > 1.0f) flutterPhase_ -= 1.0f;
        float flutter = std::sin(flutterPhase_ * 6.2831853f) * flutterAmount * 0.003f;

        // Slow pitch wobble: 0.62 Hz, scales 0..0.5 semitones with amount
        // 0.5 semitones = 2^(0.5/12) - 1 ≈ 0.02930
        wowPhase_ += 0.62f / sampleRate;
        if (wowPhase_ > 1.0f) wowPhase_ -= 1.0f;
        float wow = std::sin(wowPhase_ * 6.2831853f) * amount * 0.02930f;

        float pitchMod = 1.0f + flutter + wow;

        // ====================================================================
        // 3. Sample Rate Reduction (kicks in around 30% amount)
        //    Emulates digital degradation / cheap converters
        // ====================================================================
        float srAmount = juce::jmax(0.0f, (amount - 0.3f) / 0.7f);
        // Decimation factor: 1 (none) to 8 (heavy)
        float decimFactor = 1.0f + srAmount * 7.0f;
        holdCounter_ += 1.0f;
        bool updateHold = (holdCounter_ >= decimFactor);
        if (updateHold)
        {
            holdCounter_ -= decimFactor;
            if (numCh >= 1) holdL_ = buffer.getSample(0, i);
            if (numCh >= 2) holdR_ = buffer.getSample(1, i);
        }

        // ====================================================================
        // 4. Bit Depth Reduction (kicks in around 40% amount)
        //    16-bit → 6-bit progressive crush
        // ====================================================================
        float bitAmount = juce::jmax(0.0f, (amount - 0.4f) / 0.6f);
        // Quantization levels: 65536 (16-bit) → 64 (6-bit)
        float levels = 65536.0f * std::pow(64.0f / 65536.0f, bitAmount);

        // ====================================================================
        // 5. Envelope follower (tracks input level for ADSR-gated hiss)
        // ====================================================================
        float peakIn = 0.0f;
        for (int ch = 0; ch < numCh; ++ch)
            peakIn = juce::jmax(peakIn, std::abs(buffer.getSample(ch, i)));

        if (peakIn > envelopeLevel_)
            envelopeLevel_ += envAttackCoeff_ * (peakIn - envelopeLevel_);
        else
            envelopeLevel_ += envReleaseCoeff_ * (peakIn - envelopeLevel_);

        float envGate = juce::jlimit(0.0f, 1.0f, envelopeLevel_ * 8.0f);

        // ====================================================================
        // 6. Tape Hiss (kicks in around 10%, gated by envelope)
        // ====================================================================
        float noiseAmount = juce::jmax(0.0f, (amount - 0.10f) / 0.90f);
        float noiseGain = noiseAmount * 0.06f * envGate;
        float crackle = 0.0f;
        if (noiseAmount > 0.2f && noiseRng_.nextFloat() < 0.0008f * noiseAmount)
            crackle = (noiseRng_.nextFloat() - 0.5f) * noiseAmount * 0.15f * envGate;

        // ====================================================================
        // 7. Saturation (cranked: more aggressive harmonic distortion)
        // ====================================================================
        float satDrive = 1.0f + amount * 2.5f;

        // ====================================================================
        // Apply all effects per-channel
        // ====================================================================
        for (int ch = 0; ch < numCh; ++ch)
        {
            float sample = buffer.getSample(ch, i);

            sample *= pitchMod;

            if (srAmount > 0.001f)
            {
                float held = (ch == 0) ? holdL_ : holdR_;
                sample = sample * (1.0f - srAmount * 0.7f) + held * (srAmount * 0.7f);
            }

            if (ch == 0)
                sample = lpFilterL_.processSample(0, sample);
            else
                sample = lpFilterR_.processSample(0, sample);

            if (bitAmount > 0.001f)
            {
                float quantized = std::round(sample * levels) / levels;
                sample = sample * (1.0f - bitAmount * 0.8f) + quantized * (bitAmount * 0.8f);
            }

            // Cranked saturation: push harder then normalize
            sample = fastTanh(sample * satDrive);
            float normFactor = 1.0f / fastTanh(satDrive);
            sample *= normFactor;

            if (noiseAmount > 0.001f)
            {
                float noise = (noiseRng_.nextFloat() * 2.0f - 1.0f) * noiseGain;
                sample += noise + crackle;
            }

            buffer.setSample(ch, i, sample);
        }
    }
}

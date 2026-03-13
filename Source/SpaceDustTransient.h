#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include <random>

//==============================================================================
/**
    SpaceDust Transient - TR-808/909 style percussive transient synthesizer.

    Generates analog-modeled drum transients (kick, snare, hat, clap, etc.)
    that play at note onset. Emulates the bridged-T oscillator, noise shaping,
    and metallic square-wave mixing techniques from the original Roland circuits.

    Features:
    - 10 drum types: 808 Kick/Snare/Hat/Open Hat/Clap/Tom/Rim/Cowbell, 909 Kick/Snare
    - Mix control for transient volume
    - Ka-Donk: delays synth output 0-1s so transient leads
    - Coarse pitch control (atonal, shifts base frequencies)
    - Pre/Post effects chain positioning
*/
class SpaceDustTransient
{
public:
    enum DrumType
    {
        Kick808 = 0,
        Snare808,
        ClosedHat808,
        OpenHat808,
        Clap808,
        Tom808,
        Rim808,
        Cowbell808,
        Kick909,
        Snare909,
        NumTypes
    };

    struct Parameters
    {
        bool enabled = false;
        int type = 0;
        float mix = 0.5f;
        bool postEffect = false;
        float kaDonk = 0.0f;     // 0-1 → 0-0.5 seconds synth delay
        float coarse = 0.0f;     // -24 to +24 semitones
        float length = 1.0f;     // 0-1: 1 = full sound, 0 = short click
    };

    SpaceDustTransient() = default;

    void prepare(const juce::dsp::ProcessSpec& spec);
    void reset();
    void setParameters(const Parameters& p);
    void trigger(int midiNoteNumber = 60);
    void process(juce::AudioBuffer<float>& buffer);

    float getKaDonkDelaySamples() const;

private:
    juce::dsp::ProcessSpec spec_{};
    Parameters params_;
    double sampleRate_ = 44100.0;

    // Smoothed mix for zipper-free changes
    juce::SmoothedValue<float> smoothedMix_{0.5f};

    // Trigger state
    bool triggered_ = false;
    int sampleCounter_ = 0;

    // Note frequency: actual Hz of the played note clamped to octave 4, with coarse offset
    float noteFreq_ = 261.63f;    // Default to C4
    float pitchMultiplier_ = 1.0f; // Ratio for filter/metallic freq scaling

    // === Oscillator state for various drum types ===

    // Sine oscillator phase (used by kick, snare shells, tom)
    double oscPhase1_ = 0.0;
    double oscPhase2_ = 0.0;

    // Square wave phases (used by hat, cowbell)
    double sqPhase_[6] = {};

    // Noise generator
    std::mt19937 noiseRng_{42u};
    std::uniform_real_distribution<float> noiseDist_{-1.0f, 1.0f};

    // Amplitude envelope
    float ampEnv_ = 0.0f;
    float ampDecayRate_ = 0.0f;

    // Pitch envelope (for kick/tom pitch sweep)
    float pitchEnv_ = 0.0f;
    float pitchDecayRate_ = 0.0f;

    // Noise envelope (for snare, clap, hat)
    float noiseEnv_ = 0.0f;
    float noiseDecayRate_ = 0.0f;

    // Clap burst state
    int clapBurstCount_ = 0;
    int clapBurstTimer_ = 0;
    float clapBurstEnv_ = 0.0f;

    // Bandpass filter for noise shaping (snare, clap, hat)
    juce::dsp::StateVariableTPTFilter<float> bpFilter_;

    // High-pass filter for hats
    juce::dsp::StateVariableTPTFilter<float> hpFilter_;

    // Internal generation methods
    float generateKick808();
    float generateSnare808();
    float generateClosedHat808();
    float generateOpenHat808();
    float generateClap808();
    float generateTom808();
    float generateRim808();
    float generateCowbell808();
    float generateKick909();
    float generateSnare909();

    void initDrumEnvelopes(int type);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SpaceDustTransient)
};

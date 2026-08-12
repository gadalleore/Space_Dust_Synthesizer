#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include <random>

#include "UserWavetable.h"

//==============================================================================
/**
    SpaceDust Transient - TR-808/909 style percussive transient synthesizer.

    Generates analog-modeled drum transients (kick, snare, hat, clap, etc.)
    that play at note onset. Emulates the bridged-T oscillator, noise shaping,
    and metallic square-wave mixing techniques from the original Roland circuits.

    Features:
    - 10 drum types: 808 Kick/Snare/Hat/Open Hat/Clap/Tom/Rim/Cowbell, 909 Kick/Snare
    - 8 User slots: any imported sample, played as the hit
    - Mix control for transient volume
    - Ka-Donk: delays synth output 0-1s so transient leads
    - Coarse pitch control (atonal, shifts base frequencies)
    - Pre/Post effects chain positioning

    IMPORTED SAMPLES AS THE HIT

    A type at or past UserWave::transientUserBase selects one of the eight import
    slots the oscillators share, and the transient plays that sample once per note
    instead of synthesising a drum. The four knobs keep their meanings:

      Mix      the hit's volume, as for every drum
      Coarse   playback speed, because speed IS pitch for a sample
      Length   1 plays the file through untouched; below that a decay closes it
               down, reaching a click at 0 -- the same knob shape the drums use
      Ka-Donk  unchanged; it delays the synth, not the hit

    The sample is played from its FIRST sample, not from the loop point the
    oscillators start at. A drum's attack is the first few milliseconds of the
    file, and starting past it would throw away the only part that matters here.
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

    /** Point at the imported waveforms, for the User types.

        Call once per block with whatever the processor's own exchange handed it.
        The bank is borrowed for that block only: a newly published one retires
        the last, which is then freed on the message thread. So the slot is looked
        up again on every block rather than held across them -- see resolveUserSlot.
        Null is fine and simply means no User type can sound. */
    void setUserWaveBank(const UserWaveBank* bank) noexcept { userWaveBank_ = bank; }

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

    // === Imported sample playback (User types) ===

    // Borrowed for one block at a time. See setUserWaveBank.
    const UserWaveBank* userWaveBank_ = nullptr;

    // Where the hit has reached. File samples in Full Sample mode, turns of the
    // waveform in Single Cycle mode -- userRate_ is in the matching unit.
    double userPos_ = 0.0;
    double userRate_ = 0.0;

    // Where the closing fade begins, in the same unit, and how long it lasts.
    // Full Sample only: a file rarely ends on a zero crossing, and cutting one
    // off mid-swing is a click on every note.
    double userFadeStart_ = 0.0;
    double userFadeLength_ = 0.0;

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

    /** One sample of an imported hit, or 0 once it has finished. Ends itself by
        dropping ampEnv_ to zero, which is the same signal every drum gives. */
    float generateUserSlot(const UserWaveSlot& slot);

    /** The slot the current type selects, or nullptr when the type is a built-in
        drum, the bank is missing, or the slot holds nothing playable. */
    const UserWaveSlot* resolveUserSlot() const noexcept;

    void initDrumEnvelopes(int type);

    /** Set the playback speed and the tail for an imported hit. */
    void initUserSlot(const UserWaveSlot& slot);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SpaceDustTransient)
};

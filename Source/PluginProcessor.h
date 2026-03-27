#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include "SynthVoice.h"
#include "SynthSound.h"
#include "SpaceDustSynthesiser.h"
#include "SpaceDustReverb.h"
#include "SpaceDustGrainDelay.h"
#include "SpaceDustPhaser.h"
#include "SpaceDustTranceGate.h"
#include "SpaceDustFlanger.h"
#include "SpaceDustBitCrusher.h"
#include "SpaceDustParametricEQ.h"
#include "SpaceDustSoftClipper.h"
#include "SpaceDustCompressor.h"
#include "SpaceDustLofi.h"
#include "SpaceDustTransient.h"
#include "SpaceDustFinalEQ.h"

//==============================================================================
/**
    SpaceDust Audio Processor
    
    Main audio processor for the Space Dust cosmic subtractive synthesizer.
    
    Responsibilities:
    - Manages polyphonic synthesizer with 8 voices
    - Handles AudioProcessorValueTreeState for all parameters
    - Coordinates real-time parameter updates to voices
    - Processes audio blocks and MIDI messages
    - Manages plugin state (save/load)
    
    Architecture:
    - Uses juce::Synthesiser for voice management and MIDI handling
    - All parameters exposed via AudioProcessorValueTreeState for thread-safe access
    - Real-time safe: parameter updates happen in audio thread without allocations
    
    Signal Flow:
    MIDI Input → Synthesiser → Voices (Osc1+Osc2 → Filter → ADSR) → Audio Output
*/
class SpaceDustAudioProcessor : public juce::AudioProcessor,
                                 public juce::AudioProcessorValueTreeState::Listener,
                                 private juce::AsyncUpdater
{
public:
    //==============================================================================
    SpaceDustAudioProcessor();
    ~SpaceDustAudioProcessor() override;

    //==============================================================================
    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

#ifndef JucePlugin_PreferredChannelConfigurations
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
#endif

    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    //==============================================================================
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    //==============================================================================
    const juce::String getName() const override;

    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    //==============================================================================
    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram(int index) override;
    const juce::String getProgramName(int index) override;
    void changeProgramName(int index, const juce::String& newName) override;

    //==============================================================================
    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    //==============================================================================
    juce::AudioProcessorValueTreeState& getValueTreeState() { return apvts; }
    
    // LFO buffers for per-sample access from voices
    juce::AudioBuffer<float> lfo1Buffer;
    juce::AudioBuffer<float> lfo2Buffer;
    
    // LFO retrigger flags (public for voice access)
    std::atomic<bool> lfo1Retrigger{true};
    std::atomic<bool> lfo2Retrigger{true};
    
    // LFO current phases (public for voice access)
    double lfo1CurrentPhase{0.0};         // Current LFO1 phase (0.0 to 1.0)
    double lfo2CurrentPhase{0.0};         // Current LFO2 phase (0.0 to 1.0)

    // LFO output smoothing (prevents clicks on retrigger/phase jumps)
    float lfo1SmoothedValue{0.0f};
    float lfo2SmoothedValue{0.0f};

    // Sample & Hold: held random value and RNG state (audio thread only)
    float lfo1SampleHoldValue{0.0f};
    float lfo2SampleHoldValue{0.0f};
    uint32_t lfo1ShState{12345u};
    uint32_t lfo2ShState{67890u};
    double lfo1PrevPhase{-1.0};  // For beat-phase wrap detection
    double lfo2PrevPhase{-1.0};

    // Pitch bend snap-back: smooth linear ramp over 0.05s (editor triggers, processor ramps)
    std::atomic<bool> pitchBendSnapActive{false};
    std::atomic<float> pitchBendSnapStartValue{0.0f};
    std::atomic<float> pitchBendRampCurrentValue{0.0f};  // For UI display sync
    std::atomic<bool> pitchBendRampComplete{false};
    std::atomic<bool> pitchBendRampReset{false};  // Editor sets to reset elapsed when starting
    
    // Noise type getter/setter (UI-only control)
    void setNoiseType(int type) { noiseType = type; updateVoicesWithParameters(); }
    int getNoiseType() const { return noiseType; }
    
    // Stereo level meter getters (thread-safe atomic reads)
    float getLeftPeakLevel() const;
    float getRightPeakLevel() const;

    // Goniometer (Lissajous) buffer getter - thread-safe double-buffered copy of output
    const juce::AudioBuffer<float>& getGoniometerBuffer() const;
    int getGoniometerValidSamples() const { return goniometerValidSamples.load(std::memory_order_acquire); }

    // Update all voices with current parameter values (called after preset load)
    void updateVoicesWithParameters(float lfo1Modulation = 0.0f, float lfo2Modulation = 0.0f);

private:
    //==============================================================================
    // -- Parameter Management --
    
    juce::AudioProcessorValueTreeState apvts;  // Thread-safe parameter storage
    
    //==============================================================================
    // -- Core Synthesis Components --
    
    SpaceDustSynthesiser synth;          // Manages polyphonic voices and MIDI handling with mono mode support
    
    // -- Atomic ADSR Parameter Storage --
    // CRITICAL: These atomic values store converted ADSR parameters (seconds/level)
    // for real-time safe access from the audio thread. The APVTS stores normalized
    // values (0.0-1.0), but ADSR needs actual time values in seconds.
    // 
    // These are updated via ValueTree listener when parameters change, ensuring
    // lock-free, real-time safe access during audio processing.
    std::atomic<float> currentAttackTime{0.01f};   // Attack time in seconds (0.01-20.0), default 0.01s
    std::atomic<float> currentDecayTime{0.8f};    // Decay time in seconds (0.01-20.0)
    std::atomic<float> currentSustainLevel{0.7f}; // Sustain level (0.0-1.0)
    std::atomic<float> currentReleaseTime{0.2f};  // Release time in seconds (0.01-20.0)
    
    // Filter envelope (same pattern as main ADSR - ensures correct conversion and label match)
    std::atomic<float> currentFilterEnvAttack{0.01f};
    std::atomic<float> currentFilterEnvDecay{0.8f};
    std::atomic<float> currentFilterEnvRelease{3.0f};
    
    // UI-only noise type (0=White, 1=Pink)
    std::atomic<int> noiseType{0};  // Default to White
    
    // Reentrancy guard for filter Link sync (prevents crash from modFilter<->master feedback loop)
    std::atomic<bool> filterSyncInProgress{false};

    float pitchBendRampSamplesElapsed{0.0f};  // Audio thread only (pitch bend ramp)
    
    //==============================================================================
    // -- LFO State (Per-Sample Computation) --
    // Per-sample LFO buffers to prevent aliasing at high rates
    // Buffers and phases are public for voice access (moved to public section)
    double currentSampleRate{44100.0};
    
    //==============================================================================
    // -- Voice / Mono state (optional future use) --
    int lastPlayedNote = -1;                      // Last played MIDI note - accessed only in audio thread
    
    //==============================================================================
    // -- Reverb Effect State --
    SpaceDustReverb reverb_;

    //==============================================================================
    // -- Grain Delay Effect State --
    SpaceDustGrainDelay grainDelay_;

    //==============================================================================
    // -- Trance Gate Effect State --
    SpaceDustTranceGate tranceGate_;

    //==============================================================================
    // -- Phaser, Flanger, Parametric EQ Effect State --
    SpaceDustPhaser phaser_;
    SpaceDustFlanger flanger_;
    SpaceDustBitCrusher bitCrusher_;
    SpaceDustParametricEQ parametricEQ_;
    SpaceDustSoftClipper softClipper_;
    SpaceDustCompressor compressor_;
    SpaceDustLofi lofi_;
    SpaceDustTransient transient_;
    SpaceDustFinalEQ finalEQ_;

    // Ka-Donk delay line: delays synth output up to 1 second so transient leads
    static constexpr int kaDonkMaxSamples = 48000;  // ~1s at 48kHz
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear> kaDonkDelayL_{kaDonkMaxSamples};
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear> kaDonkDelayR_{kaDonkMaxSamples};
    juce::SmoothedValue<float> smoothedKaDonkDelay_{0.0f};

    //==============================================================================
    // -- Delay Effect State --
    static constexpr int maxDelaySamples = 88200;  // ~2s at 44.1 kHz
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear> delayLineL{maxDelaySamples};
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear> delayLineR{maxDelaySamples};
    juce::dsp::StateVariableTPTFilter<float> delayFilterHP;
    juce::dsp::StateVariableTPTFilter<float> delayFilterLP;
    juce::dsp::StateVariableTPTFilter<float> delayFilterHPFb;  // Low-Q for feedback (prevents resonance runaway)
    juce::dsp::StateVariableTPTFilter<float> delayFilterLPFb;
    
    // Smoothed parameters (prevents zippers, resonance spikes, pitch artifacts on param changes)
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> smoothedDelayTime{1.0f};
    juce::SmoothedValue<float> smoothedDelayDecay{0.0f};
    juce::SmoothedValue<float> smoothedDelayDryWet{0.0f};
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> smoothedDelayHPCutoff{1000.0f};
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> smoothedDelayLPCutoff{4000.0f};
    juce::SmoothedValue<float> smoothedDelayHPQ{0.707f};
    juce::SmoothedValue<float> smoothedDelayLPQ{0.707f};
    
    //==============================================================================
    // -- Stereo Level Meter State --
    // Real-time peak levels for L/R channels (for level meter display)
    // Atomic for thread-safe access from audio thread (processBlock) and UI thread (timer)
    std::atomic<float> leftPeakLevel{0.0f};   // Peak level for left channel (0.0 = silence, 1.0 = 0 dB)
    std::atomic<float> rightPeakLevel{0.0f};  // Peak level for right channel (0.0 = silence, 1.0 = 0 dB)

    //==============================================================================
    // -- Goniometer (Lissajous) State --
    // Double-buffered copy of output for Spectral tab goniometer display.
    // Audio thread writes to one buffer, UI reads from the other (atomic swap).
    static constexpr int goniometerMaxSamples = 4096;
    juce::AudioBuffer<float> goniometerBuffer[2];
    std::atomic<int> goniometerReadIndex{0};
    std::atomic<int> goniometerValidSamples{0};
    
    // -- Helper Methods --
    
    /**
        Create parameter layout for AudioProcessorValueTreeState.
        Defines all synthesizer parameters with ranges and defaults.
    */
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    
    /**
        ValueTree listener callback for ADSR parameter updates.
        Converts normalized parameter values to actual seconds/levels and stores
        in atomic variables for real-time safe access from audio thread.
    */
    void parameterChanged(const juce::String& parameterID, float newValue) override;

    void handleAsyncUpdate() override;

    // Deferred filter sync flags (set in parameterChanged, handled in handleAsyncUpdate).
    // AsyncUpdater coalesces rapid automation into a single callback and auto-cancels
    // on destruction, eliminating the use-after-free from Timer::callAfterDelay.
    std::atomic<bool> pendingSyncMasterToMod1{false};
    std::atomic<bool> pendingSyncMasterToMod2{false};
    std::atomic<bool> pendingSyncMod1ToMaster{false};
    std::atomic<bool> pendingSyncMod2ToMaster{false};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SpaceDustAudioProcessor)
};


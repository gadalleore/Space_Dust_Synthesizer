#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>
#include "SynthSound.h"
#include <array>
#include <numeric>

class SpaceDustSynthesiser;
class SpaceDustAudioProcessor;

//==============================================================================
/**
    SpaceDust Synthesiser Voice
    
    A polyphonic voice with dual oscillators, multimode filter, and ADSR envelope.
    This voice generates cosmic subtractive synthesis tones with real-time safe processing.
    
    Signal Path: Osc1 (with detune) + Osc2 (with detune) → Mix → Filter → ADSR Envelope → Output
    
    Real-time Safety: All processing is allocation-free and uses smooth parameter updates.
    
    ADSR Envelope:
    - Proper 4-stage envelope: Attack → Decay → Sustain → Release
    - Linear amplitude ramping for real-time safety
    - Long cosmic tails supported (release up to 20 seconds)
    
    Detuning:
    - Independent detune for each oscillator (coarse + fine)
    - Applied directly to oscillator pitch before phase calculation
    - Creates shimmering, unison-like effects
*/
class SynthVoice : public juce::SynthesiserVoice
{
public:
    //==============================================================================
    // Waveform enumeration for oscillator selection
    enum Waveform
    {
        Sine = 0,
        Triangle = 1,
        Saw = 2,
        Square = 3
    };
    
    // Noise type enumeration
    enum NoiseType
    {
        White = 0,
        Pink = 1
    };

    //==============================================================================
    // SynthesiserVoice overrides
    bool canPlaySound(juce::SynthesiserSound* sound) override
    {
        return dynamic_cast<SynthSound*>(sound) != nullptr;
    }

    void startNote(int midiNoteNumber, float velocity,
                   juce::SynthesiserSound* sound,
                   int currentPitchWheelPosition) override;

    void stopNote(float velocity, bool allowTailOff) override;

    void controllerMoved(int controllerNumber, int newControllerValue) override;

    void pitchWheelMoved(int newPitchWheelValue) override;

    void renderNextBlock(juce::AudioBuffer<float>& outputBuffer,
                         int startSample, int numSamples) override;

    void setCurrentPlaybackSampleRate(double newRate) override;
    
    /**
        Prepare voice DSP with valid sample rate and block size.
        
        CRITICAL: This method MUST be called explicitly in PluginProcessor::prepareToPlay()
        after voices are added but BEFORE synth.setCurrentPlaybackSampleRate() is called.
        
        Why DSP initialization MUST happen here, not in constructor:
        - Sample rate is only known when host calls prepareToPlay()
        - DSP objects (filter, ADSR) require valid sample rate for prepare()
        - Constructor runs before sample rate is known (sampleRate = 0)
        - Initializing DSP in constructor with sampleRate=0 causes:
          * StateVariableTPTFilter assertions (invalid sample rate)
          * ADSR assertions (invalid timing calculations)
          * Corrupted voice state in strict hosts like Ableton Live
        
        This is the standard, bulletproof way to initialize voices in modern JUCE.
    */
    void prepareToPlay(double sampleRate, int samplesPerBlock);

    //==============================================================================
    // -- Parameter Update Methods --
    // These are called from the processor to update voice parameters in real-time.
    // All methods are thread-safe and lock-free for audio thread compatibility.

    void setOsc1Waveform(int waveform);
    void setOsc2Waveform(int waveform);
    
    // Oscillator pitch tuning (simple, intuitive system)
    void setOsc1CoarseTune(float semitones);
    void setOsc1Detune(float cents);
    void setOsc2CoarseTune(float semitones);
    void setOsc2Detune(float cents);
    
    // Independent oscillator and noise level controls
    void setOsc1Level(float level);
    void setOsc2Level(float level);
    void setOsc1Pan(float pan);   // -1 = full left, 0 = center, 1 = full right
    void setOsc2Pan(float pan);
    void setNoiseLevel(float level);
    
    // Sub oscillator (one octave down)
    void setSubOscOn(bool on);
    void setSubOscWaveform(int waveform);
    void setSubOscLevel(float level);
    void setSubOscCoarse(float semitones);
    void setNoiseType(int type);  // 0=White, 1=Pink
    
    // Noise EQ parameters (affects noise source only)
    // Range: -1.0 to +1.0 (negative = cut, positive = boost)
    void setLowShelfAmount(float amount);   // Affects frequencies below 200 Hz
    void setHighShelfAmount(float amount);  // Affects frequencies above 1.5 kHz
    
    void setFilterMode(int mode);
    void setFilterCutoff(float cutoffHz);
    void setFilterResonance(float resonance);
    void setWarmSaturationMaster(bool enabled);  // Moog-style saturation when ON
    
    // Mod tab filters (show=enabled in UI, linkToMaster=use main filter params)
    void setModFilter1(bool show, bool linkToMaster, int mode, float cutoffHz, float resonance);
    void setWarmSaturationMod1(bool enabled);
    void setModFilter2(bool show, bool linkToMaster, int mode, float cutoffHz, float resonance);
    void setWarmSaturationMod2(bool enabled);
    
    // Filter envelope parameters
    void setFilterEnvAttack(float seconds);
    void setFilterEnvDecay(float seconds);
    void setFilterEnvSustain(float level);  // 0.0 to 1.0
    void setFilterEnvRelease(float seconds);
    // Amount is provided in UI percent (-100 to +100). Internally this is mapped
    // to a normalized bipolar range (-1.0 to +1.0) for modulation depth.
    void setFilterEnvAmount(float amount);
    
    // ADSR envelope parameters
    void setEnvAttack(float seconds);
    void setEnvDecay(float seconds);
    void setEnvSustain(float level);  // 0.0 to 1.0
    void setEnvRelease(float seconds);
    
    // Voice mode and glide (portamento) parameters
    void setGlideTime(float seconds);      // 0.0 to 5.0 seconds
    void setLegatoGlide(bool enabled);    // Enable/disable fingered (legato-only) glide behaviour
    
    // Pitch envelope (ramp from 0 to pitch over time, scaled by amount)
    void setPitchEnvAmount(float amount);   // -100 to 100 %
    void setPitchEnvTime(float seconds);    // 0-10 s
    void setPitchEnvPitch(float semitones); // 0-24 st
    
    // Pitch bend (scaled by pitchBendAmount: 0-24 semitones) - separate from pitch envelope
    void setPitchBendAmount(float semitones);  // Range for pitch bend (0-24)
    void setPitchBend(float value);           // Manual pitch bend (-1 to 1)
    void setLfoTargets(int lfo1Target, int lfo2Target);  // 0=Pitch, 1=Filter, 2=MasterVol, 3=Osc1, 4=Osc2, 5=Noise
    void setSynthesiser(SpaceDustSynthesiser* s) { synthesiser = s; }
    void setProcessor(SpaceDustAudioProcessor* p) { processor = p; }
    
    /** For glide: when a new note uses a different voice, we need the previous pitch from another voice. */
    double getCurrentPitch() const { return juce::jlimit(20.0, 20000.0, currentPitch); }

private:
    SpaceDustSynthesiser* synthesiser = nullptr;
    SpaceDustAudioProcessor* processor = nullptr;  // Pointer to processor for LFO buffer access
    //==============================================================================
    // -- Oscillator State --
    double osc1Angle = 0.0;
    double osc1AngleDelta = 0.0;
    double osc2Angle = 0.0;
    double osc2AngleDelta = 0.0;
    
    // Waveform selection (0=Sine, 1=Triangle, 2=Saw, 3=Square)
    int osc1Waveform = Saw;
    int osc2Waveform = Saw;
    
    // -- Oscillator Pitch Tuning --
    // Each oscillator has independent coarse tuning (±24 semitones) and fine detuning (±50 cents)
    // Simple, intuitive system: Coarse for intervals, Detune for shimmer
    // Both default to 0 (perfectly in tune) - double-click any knob to reset
    float osc1CoarseTune = 0.0f;      // Semitones (-24 to +24), default 0
    float osc1Detune = 0.0f;         // Cents (-50 to +50), default 0
    float osc2CoarseTune = 0.0f;      // Semitones (-24 to +24), default 0
    float osc2Detune = 0.0f;         // Cents (-50 to +50), default 0
    
    // -- Independent Oscillator and Noise Level Controls --
    // Each source has independent volume (0.0 to 1.0) for flexible additive mixing
    // This allows layering detuned saws, adding noise wash, or creating subtle textures
    float osc1Level = 0.8f;          // Oscillator 1 level (0.0-1.0), default 0.8
    float osc2Level = 0.8f;          // Oscillator 2 level (0.0-1.0), default 0.8
    float osc1Pan = 0.0f;            // Osc 1 pan (-1=left, 0=center, 1=right)
    float osc2Pan = 0.0f;            // Osc 2 pan (-1=left, 0=center, 1=right)
    float noiseLevel = 0.0f;         // Noise level (0.0-1.0), default 0.0 (off)
    
    // Sub oscillator (one octave down)
    bool subOscOn = false;
    int subOscWaveform = 1;          // 0=Sine, 1=Triangle, 2=Saw, 3=Square
    float subOscLevel = 0.5f;
    float subOscCoarse = 0.0f;      // Semitones
    double subOscAngle = 0.0;
    double subOscAngleDelta = 0.0;
    int noiseType = White;           // Noise type (0=White, 1=Pink)
    
    // Noise EQ parameters (affects noise source only)
    float lowShelfAmount = 0.0f;     // Low shelf/cut amount (-1.0 to +1.0), affects frequencies below 200 Hz
    float highShelfAmount = 0.0f;    // High shelf/cut amount (-1.0 to +1.0), affects frequencies above 1.5 kHz
    
    // Noise EQ filter state (for simple 1-pole filters)
    float lowShelfState = 0.0f;      // Low shelf filter state (one sample delay)
    float highShelfState = 0.0f;     // High shelf filter state (one sample delay)
    
    // Pre-allocated buffers for renderNextBlock (no allocations in audio thread)
    juce::AudioBuffer<float> voiceTempBuffer;
    juce::AudioBuffer<float> voiceSingleSampleBuffer;
    
    // -- Pink Noise State (Voss-McCartney algorithm) --
    std::array<float, 16> pinkState{};
    float pinkSum = 0.0f;
    int pinkIndex = 0;
    juce::Random random{static_cast<juce::int64>(reinterpret_cast<uintptr_t>(this))};  // Random number generator for noise (seeded with voice address)
    
    // -- Filter --
    juce::dsp::StateVariableTPTFilter<float> filter;
    juce::dsp::StateVariableTPTFilter<float> modFilter1;  // Second filter when modFilter1 unlinked
    juce::dsp::StateVariableTPTFilter<float> modFilter2;  // Third filter when modFilter2 unlinked
    
    // Noise EQ filters: simple 1-pole shelf filters for low and high frequency shaping
    juce::dsp::IIR::Filter<float> lowShelfFilter;
    juce::dsp::IIR::Filter<float> highShelfFilter;
    int filterMode = 0;               // 0=LowPass, 1=BandPass, 2=HighPass
    float filterCutoff = 8000.0f;     // Hz (20-20000) - current modulated cutoff
    float baseFilterCutoff = 8000.0f; // Base cutoff value (unmodulated, from parameter)
    float filterResonance = 0.3f;     // Normalized (0.0-1.0, maps to Q 0.1-20.0)
    bool warmSaturationMaster = false; // Moog-style tanh saturation when ON
    
    // Mod tab filters (show=Filter toggle on, linkToMaster=use main filter params)
    bool modFilter1Show = false;
    bool modFilter1Linked = true;
    int modFilter1Mode = 0;
    float modFilter1Cutoff = 8000.0f;
    float modFilter1Resonance = 0.3f;
    bool warmSaturationMod1 = false;
    bool modFilter2Show = false;
    bool modFilter2Linked = true;
    int modFilter2Mode = 0;
    float modFilter2Cutoff = 8000.0f;
    float modFilter2Resonance = 0.3f;
    bool warmSaturationMod2 = false;
    
    // -- Filter Envelope (ADSR) --
    juce::ADSR filterAdsr;            // Filter envelope processor
    float filterEnvAttackTime = 0.01f;   // Attack time (0.01-20.0s, skewed)
    float filterEnvDecayTime = 0.8f;     // Decay time (0.01-20.0s, skewed)
    float filterEnvSustainLevel = 0.7f;  // Sustain level (0.0-1.0, linear)
    float filterEnvReleaseTime = 3.0f;   // Release time (0.01-20.0s, skewed)
    float filterEnvAmount = 0.0f;        // Envelope amount (-1.0 to 1.0, bipolar)
    
    // -- ADSR Envelope --
    // Using JUCE's built-in ADSR for reliable, professional envelope behavior.
    // This provides proper 4-stage envelope: Attack → Decay → Sustain → Release
    // with smooth transitions and proper parameter handling.
    juce::ADSR adsr;                   // JUCE's ADSR envelope processor
    
    // ADSR timing parameters (in seconds) - stored for parameter updates
    float envAttackTime = 0.1f;        // Attack time (0.01-20.0s, skewed)
    float envDecayTime = 0.8f;         // Decay time (0.01-20.0s, skewed)
    float envSustainLevel = 0.7f;      // Sustain level (0.0-1.0, linear)
    float envReleaseTime = 0.2f;        // Release time (0.01-20.0s, skewed) - long cosmic tails!
    
    double sampleRate = 44100.0;       // Current sample rate for envelope calculations
    bool isDspInitialized = false;     // Track if DSP has been properly initialized
    
    // Voice state
    bool isActive = false;
    bool inReleasePhase = false;   // True after noteOff() until ADSR completes

    // Anti-click: one-pole lowpass smoother on envelope output (~3ms time constant).
    float smoothedEnvelope = 0.0f;
    float envSmoothCoeff = 0.0f;   // Computed from sample rate in prepareToPlay

    // Voice fade: linear gain ramp applied to the FINAL output sample (after
    // filter + ADSR) to prevent clicks on any hard stop.  When stopNote is called
    // with allowTailOff=false (voice stealing, allNotesOff, etc.), the voice keeps
    // producing audio while voiceFade ramps linearly from 1→0 over kVoiceFadeLength
    // samples.  ONLY when the fade reaches zero does renderNextBlock do the full
    // cleanup (adsr.reset, clearCurrentNote, zero deltas).  startNote cancels any
    // pending fade immediately (voiceFade=1, remaining=0) for seamless voice reuse.
    float voiceFade = 1.0f;                        // Current fade multiplier (1.0=full, 0.0=silent)
    int voiceFadeSamplesRemaining = 0;             // Samples left in fade-out (0 = inactive)
    static constexpr int kVoiceFadeLength = 64;    // ~1.5ms at 44.1kHz

    // Safety output smoother: one-pole lowpass on the final sample value.
    // Catches any residual single-sample discontinuity (pitch/filter jumps on
    // mono/legato handoff).  coeff=0.99 means 99% of error corrected each sample
    // (τ ≈ 100 samples / ~2.3ms at 44.1kHz) — transparent to normal audio but
    // smooths single-sample spikes.  Negligible CPU.
    float outputSmootherL = 0.0f;
    float outputSmootherR = 0.0f;
    static constexpr float kOutputSmoothCoeff = 0.99f;
    
    //==============================================================================
    // -- Glide (Portamento) State --
    // 
    // Glide provides smooth pitch transitions between notes for expressive playing.
    // Works in BOTH polyphonic and monophonic modes:
    // - Poly mode: Each voice glides independently when new notes start
    // - Mono mode: Glide only occurs during legato (overlapping notes)
    // 
    // Implementation: Linear slew (ramp) from currentPitch to targetPitch over glideTime.
    // Real-time safe: No allocations, pure computation in audio thread.
    
    double currentPitch = 0.0;         // Current pitch in Hz (slewed toward target)
    double targetPitch = 0.0;          // Target pitch in Hz (from MIDI note + tuning)
    double glideDelta = 0.0;           // Pitch change per sample (calculated from glideTime)
    // Per-note legato state: true when this startNote was triggered as a legato overlap
    // (set from SpaceDustSynthesiser via nextNoteIsLegato flag). This is *not* the same
    // as the global "Legato Glide" parameter – that lives in legatoGlideEnabled.
    bool isLegatoNote = false;

    // Global per-voice flag set from the processor: when true, glide only happens
    // on overlapping (legato) notes; when false, glide happens on every note change.
    bool legatoGlideEnabled = true;
    float glideTimeSeconds = 0.0f;     // Glide time in seconds (0.0 = instant, no glide)
    
    // Pitch envelope (ramp over time from note-on)
    float pitchEnvAmount = 0.0f;       // -100 to 100 %
    float pitchEnvTime = 0.0f;         // 0-5 s
    float pitchEnvPitch = 0.0f;        // 0-24 semitones
    float pitchEnvSamplesElapsed = 0.0f;  // Samples since note-on (for ramp)
    
    // LFO targets (cached per-block from processor - avoids per-sample APVTS reads)
    int lfo1TargetCached = 0;  // 0=Pitch, 1=Filter, 2=MasterVol, 3=Osc1, 4=Osc2, 5=Noise
    int lfo2TargetCached = 1;
    
    // Pitch bend (from processor, updated every block) - separate from pitch envelope
    float pitchBendAmountFloat = 0.0f; // 0-24 semitones (range for bend, 0 = no bend)
    float pitchBend = 0.0f;            // Manual pitch bend (-1 to 1)
    float lastPitchWheelNormalized = 0.0f;  // MIDI pitch wheel -1 to 1 (from pitchWheelMoved)
    
    //==============================================================================
    // -- Helper Methods --
    
    /**
        Generate a waveform sample from an angle and waveform type.
        Real-time safe: no allocations, pure computation.
    */
    float generateWaveform(double angle, int waveform);
    
    /**
        Update filter parameters based on current mode, cutoff, and resonance.
        Called when filter parameters change.
    */
    void updateFilter();
    void updateModFilter1();
    void updateModFilter2();
    
    /**
        Update noise EQ shelf filter coefficients based on current shelf amounts.
        Called when shelf amounts change or sample rate changes.
    */
    void updateNoiseEqFilters();
    
    /**
        Update oscillator frequencies based on base frequency, coarse tune, and detune.
        Final pitch calculation:
        - Osc1: midiNote + osc1CoarseTune + (osc1Detune / 100) [all in semitones]
        - Osc2: midiNote + osc2CoarseTune + (osc2Detune / 100) [all in semitones]
        Convert cents to semitones by dividing by 100.
    */
    void updateOsc1Frequency(double baseFrequency);
    void updateOsc2Frequency(double baseFrequency);
    
    /**
        Update ADSR parameters from stored timing values.
        
        This method must be called:
        - In the voice constructor (initial setup)
        - When any envelope parameter changes (via setEnvAttack, setEnvDecay, etc.)
        - When sample rate changes (to recalculate sample-based timing)
        
        CRITICAL: JUCE's ADSR requires all parameters to be set together via setParameters().
        Individual parameter changes won't take effect until this is called.
        
        Real-time Safety: This method is safe to call from the audio thread as it only
        updates the ADSR's internal parameters (no allocations).
    */
    void updateAdsrParameters();
    
    /**
        Update Filter Envelope ADSR parameters from stored timing values.
        Same requirements and behavior as updateAdsrParameters() but for filter envelope.
    */
    void updateFilterAdsrParameters();
};

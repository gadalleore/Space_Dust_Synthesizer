#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include <array>
#include <atomic>
#include <cstdint>

#include "SynthVoice.h"
#include "SynthSound.h"
#include "NonlinearSVF.h" // transient mirror filters (same five modes as the voice filter)
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
#include "PresetHotReload.h"
#include "ResampleCapture.h"
#include "ModMatrixState.h"

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

    /** Every effect, in order, over one buffer.

        startSampleInBlock is how far into the host's block this buffer begins,
        which the trance gate needs so its grid keeps moving when the chain is
        called on chunks.

        Split out of processBlock so it can be called once per block, or sixteen
        times on 32-sample views of the same memory when an effect parameter is
        modulated. */
    void runEffectsChain (juce::AudioBuffer<float>& buffer, int startSampleInBlock);

    /** Test hook: make the chain run chunked even with nothing modulated, so the
        chunk audit can compare the two paths on the same patch. */
    void setForceEffectChunkingForTests (bool shouldChunk) noexcept
    {
        forceChunking = shouldChunk;
    }

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

    /** The samples the player has imported as oscillator waveforms.

        The editor edits this directly; the audio thread never touches it, and
        gets an immutable snapshot instead (see the note in UserWavetable.h). */
    UserWaveLibrary& getUserWaveLibrary() { return userWaveLibrary; }

    /** The routing list, and the knobs it is allowed to reach.

        Public because the editor edits the matrix directly in assign mode.
        Edited on the message thread only; the audio thread reads the compiled
        form that Task 4 publishes, never this. */
    spacedust::ModMatrix        modMatrix;
    spacedust::DestinationTable modDestinations;

    // LFO buffers for per-sample access from voices
    juce::AudioBuffer<float> lfo1Buffer;
    juce::AudioBuffer<float> lfo2Buffer;
    
    // LFO retrigger flags (public for voice access)
    std::atomic<bool> lfo1Retrigger{true};
    std::atomic<bool> lfo2Retrigger{true};

    // Realised free-run rate of each LFO, published to the voices so their oversample
    // latch can tell that a cutoff is being swept at audio rate. Written during the
    // LFO render, read on the next block -- the latch only consults it at note start,
    // so a block of delay is immaterial.
    double lastLfo1Hz{0.0};
    double lastLfo2Hz{0.0};

public:
    //==========================================================================
    // Free-running LFO rate range, in one place because the DSP, the knob's snap
    // grid and the readout must all agree.
    //
    // The top is 200 Hz. It was briefly raised to 2 kHz chasing audio-rate LFO
    // modulation, then put back: at 2 kHz an LFO has about 24 samples per cycle, too
    // few for its waveform to have a shape, so it stops being a modulator and becomes
    // a poor oscillator. Nothing musical lives up there, which is why other synths cap
    // their LFOs well below it too.
    static constexpr double lfoFreeRateMinHz = 0.01;
    static constexpr double lfoFreeRateMaxHz = 200.0;

    // The 2 kHz top that state version 2 was written against.
    static constexpr double lfoFreeRateV2MaxHz = 2000.0;

    /** Knob position (0-12) -> Hz, using the current range. */
    static double lfoKnobToHz(double knob0to12);

    /** Rescales lfo1Rate / lfo2Rate when a state tree was saved against a different
        rate range, so the LFO keeps the frequency it was saved with.

        A stored rate is a KNOB POSITION, so its meaning moves with the range. Only
        version 2 is affected: it is the one written while the top was 2 kHz. Version 1
        (or a missing attribute) predates that and already means what it says. */
    static void migrateLfoRatesIfOld(juce::ValueTree& state, int stateVersion);

    /** Carry a waveform choice across the day the built-in shapes grew from four
        to twenty-one.

        Up to stateVersion 3 the list was Sine, Triangle, Saw, Square, then User
        1..8 -- so a stored 4 meant User 1. From version 4 there are seventeen
        more shapes in front of the User slots, and that same 4 means Pulse 25%.
        Every stored value at or above the old base is moved up by the number of
        shapes that were inserted.

        Applies to the three menus that share the shape list: osc1Waveform,
        osc2Waveform and subOscWaveform. noiseType and transientType have lists of
        their own and are untouched.

        WHAT THIS CANNOT FIX: a host's automation lane. The DAW stores a
        NORMALISED float, not an index, and it never passes through here -- it is
        written straight into the parameter. Any automation already drawn against
        a waveform menu will select a different shape after this change. Presets
        are safe; automation is not, and cannot be made so. */
    static void migrateWaveformChoicesIfOld(juce::ValueTree& state, int stateVersion);

    /** Bumped whenever a stored value changes meaning.
        1 (or absent) = 200 Hz LFO top. 2 = 2 kHz top. 3 = 200 Hz again. */
    /** 4: the built-in oscillator shapes grew from four to twenty-one, moving
        every User slot up the list. See migrateWaveformChoicesIfOld. */
    static constexpr int currentStateVersion = 4;

    /** Built-in shapes there were up to stateVersion 3: Sine, Triangle, Saw,
        Square. Frozen as a number on purpose -- it is a fact about old files, so
        it must not follow OscShape::numShapes when more shapes are added. */
    static constexpr int legacyOscUserBase = 4;

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

    /** Incremented on the audio thread when a voice replaces non-finite osc freq or sample output. */
    std::atomic<std::uint32_t> dspSanitizeEventCount { 0 };

    // Pitch bend snap-back: smooth linear ramp over 0.05s (editor triggers, processor ramps)
    std::atomic<bool> pitchBendSnapActive{false};
    std::atomic<float> pitchBendSnapStartValue{0.0f};
    std::atomic<float> pitchBendRampCurrentValue{0.0f};  // For UI display sync
    std::atomic<bool> pitchBendRampComplete{false};
    std::atomic<bool> pitchBendRampReset{false};  // Editor sets to reset elapsed when starting

    /** Debug/diagnostics: times voices had to fix NaN/Inf (e.g. after investigating glitches). */
    std::uint32_t getDspSanitizeEventCount() const noexcept { return dspSanitizeEventCount.load(std::memory_order_relaxed); }
    void resetDspSanitizeEventCount() noexcept { dspSanitizeEventCount.store(0u, std::memory_order_relaxed); }
    
    // Stereo level meter getters (thread-safe atomic reads)
    float getLeftPeakLevel() const;
    float getRightPeakLevel() const;

    // Goniometer (Lissajous) buffer getter - thread-safe double-buffered copy of output
    const juce::AudioBuffer<float>& getGoniometerBuffer() const;
    int getGoniometerValidSamples() const { return goniometerValidSamples.load(std::memory_order_acquire); }

    // Spectrum analyser: copy the most-recent `numSamples` of continuous mono output
    // history into dest (gap-free, so the FFT window is faithful and the display is stable).
    void readSpectrumSamples(float* dest, int numSamples) const;

    // Oscilloscope: the same, kept in STEREO. Copies the most-recent `numSamples` of
    // continuous L and R output history so the scope can draw both channels over a
    // long window without stitching non-adjacent blocks together.
    void readScopeSamples(float* destL, float* destR, int numSamples) const;

    //==============================================================================
    // -- Resample --
    // The Waveforms window's Resample button: the synth plays itself a middle C
    // and keeps what comes out of the whole chain. Nothing has to be held down.
    // See ResampleCapture for how a recording starts, ends and changes hands.

    /** Ask for a recording. It starts at the top of the next audio block and runs
        until the sound has died away, so this returns at once and the caller has
        to wait for isResampleRecording() to go false. False here means one is
        already running, or that audio has never been prepared. */
    bool startResampleRecording() { return resampleCapture.arm(); }

    bool isResampleRecording() const noexcept { return resampleCapture.isBusy(); }

    /** How far the recording has got, 0 to 1, for the bar the player watches. */
    float getResampleProgress() const noexcept { return resampleCapture.progress(); }

    /** Take the finished recording, mono, ready to be built into a waveform slot.
        False while one is still running, and false when it holds no sound.

        stoppedByLength says the tail was still sounding when the recording ran
        out of room -- see ResampleCapture::take. */
    bool takeResampleRecording(std::vector<float>& left, std::vector<float>& right,
                               double& sampleRate, bool& stoppedByLength)
    {
        return resampleCapture.take(left, right, sampleRate, stoppedByLength);
    }

    /** Give up on a recording that can never finish, because the host has stopped
        calling processBlock. */
    void cancelResampleRecording() { resampleCapture.cancel(); }

    //==============================================================================
    // -- Where an imported sample has got to --
    // Drives the playhead in the Waveforms window. Cleared at the top of every
    // block and written by whichever voices are reading a slot, so it empties by
    // itself the moment nothing is playing one -- no note-off to catch, no timer
    // to expire. With a chord down it shows one of the voices; there is one line
    // and there is nothing to choose between them.

    /** Whether the Waveforms window is open and watching. Nothing is published
        while it is not, so a shut window costs the audio thread nothing. */
    void setUserWavePhaseWanted(bool wanted) noexcept
    {
        userWavePhaseWanted.store(wanted, std::memory_order_relaxed);
    }

    bool isUserWavePhaseWanted() const noexcept
    {
        return userWavePhaseWanted.load(std::memory_order_relaxed);
    }

    /** Audio thread: say how far through its sample this source is, 0 to 1. */
    void publishUserWavePhase(UserWave::Group group, float phase01) noexcept
    {
        userWavePhase[(int) group].store(phase01, std::memory_order_relaxed);
    }

    /** Message thread: how far through, or a negative number when this list's
        sample is not being read by anything. */
    float getUserWavePhase(UserWave::Group group) const noexcept
    {
        return userWavePhase[(int) group].load(std::memory_order_relaxed);
    }

    // Update all voices with current parameter values (called after preset load)
    void updateVoicesWithParameters(float lfo1Modulation = 0.0f, float lfo2Modulation = 0.0f);

    // Persistent UI state (survives editor close/reopen, saved in DAW session)
    juce::String currentPresetName { "Init" };
    bool cheezeGuyActivated = false;
    // Last tab the user had open, so reopening the editor returns to it instead of Main.
    int lastActiveTabIndex = 0;

    // On-screen / computer-keyboard MIDI input for the STANDALONE build (dev auditioning
    // without a DAW). The editor adds a juce::MidiKeyboardComponent bound to this state
    // ONLY when wrapperType == wrapperType_Standalone; processBlock merges its notes into
    // the MIDI stream. In a plugin (VST3) no keys are ever pressed, so the merge is a
    // harmless no-op. Lives in the processor so it survives editor open/close.
    juce::MidiKeyboardState keyboardState;

private:
    //==============================================================================
    // -- Parameter Management --
    
    juce::AudioProcessorValueTreeState apvts;  // Thread-safe parameter storage

    // Publishes the live sound and re-applies presets edited on disk. Declared after
    // apvts so it is destroyed first — its timer reads apvts on every tick.
    PresetHotReload presetHotReload { *this, apvts, currentPresetName };

    //==============================================================================
    // -- Imported waveforms --

    UserWaveLibrary userWaveLibrary;

    /** The snapshot the audio thread is playing from.

        Deliberately a raw pointer, not a smart one. It is handed over by an atomic
        exchange inside processBlock, and a smart pointer would free the previous
        bank right there on the audio thread -- which is the one thing the whole
        handover exists to avoid. It is freed on the message thread instead, either
        by the library's timer or by the destructor here. */
    UserWaveBank* audioUserWaveBank = nullptr;

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
    
    // Reentrancy guard for filter Link sync (prevents crash from modFilter<->master feedback loop)
    std::atomic<bool> filterSyncInProgress{false};

    // MPE zone-layout reconfig is REQUESTED from the message thread (parameterChanged on
    // mpeMode / mpePitchBendRange) but APPLIED on the audio thread (top of processBlock),
    // so it can't race the synth's note rendering. setZoneLayout()/enableLegacyMode()
    // release notes and aren't thread-safe against renderNextBlock (JUCE's noteStateLock
    // is private, so we serialise by doing the reconfig on the audio thread instead).
    std::atomic<bool> mpeReconfigPending{false};
    void applyPendingMpeReconfig();

    float pitchBendRampSamplesElapsed{0.0f};  // Audio thread only (pitch bend ramp)
    
    //==============================================================================
    // -- LFO State (Per-Sample Computation) --
    // Per-sample LFO buffers to prevent aliasing at high rates
    // Buffers and phases are public for voice access (moved to public section)
    double currentSampleRate{44100.0};
    
    //==============================================================================
    // -- Voice / Mono state (optional future use) --
    int lastPlayedNote = -1;                      // Last played MIDI note - accessed only in audio thread

    // -- Transport-edge tracking (audio thread only) --
    // Used to detect transport stop and playhead jumps (loop wrap / seek) so the
    // mono/legato note stack can be flushed (synth.resetNoteState()).  Without this
    // the note stack desyncs from the host across a loop → wrong / stuck notes.
    bool   wasPlayingState = false;
    double lastPpqPosition = 0.0;

    //==============================================================================
    // -- Effects chain chunking --
    bool forceChunking = false;

    /** 32 samples is a control rate near 1400 Hz at 44.1 kHz -- smooth for any
        LFO you can hear -- and short enough that no effect's own smoothing can
        step audibly between pieces. */
    static constexpr int effectChunkSamples = 32;

    /** Whether any live routing lands on a parameter the effects chain reads.
        Task 3 stubs this to false; Task 4 gives it the real answer. */
    bool anyEffectParameterIsModulated() const noexcept;

    //==============================================================================
    // -- Reverb Effect State --
    SpaceDustReverb reverb_;
    /** Last reverb decay (seconds) used for edge-detect: reset tail once when hitting minimum, not every block. */
    float lastReverbDecayForBypass_{ -1.0f };

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

    // Pre-mode transient routing: when Post-Effect is OFF the transient must be
    // coloured "before the filter". The per-voice synth filter lives inside the
    // synth (cannot be re-entered from the master buffer), so we mirror it here:
    // the transient is rendered into transientScratch_, run through these filters
    // (matched to the synth's mode/cutoff/resonance), then summed into the mix.
    // To stay consistent with the per-voice chain (master → mod1 → mod2 in series,
    // each mod stage active only when it is Shown AND unlinked) the transient is run
    // through the same series of mirrors, so the unlinked Mod-tab filters cut the
    // transient exactly like the Main-tab filter does. Post mode is unchanged
    // (end-of-chain, unfiltered). This keeps the fix entirely in the master chain —
    // no change to SynthVoice.
    // NonlinearSVF (rather than juce::dsp::StateVariableTPTFilter) because the mirrors
    // must offer the same five modes as the voice filter, including Notch and Peak,
    // which JUCE's SVF does not expose. Driven through setResonanceQ() so the legacy
    // mirror Q map — and therefore the existing LP/BP/HP sound — is unchanged: the
    // topology and maths are the same TPT SVF.
    NonlinearSVF transientPreFilter_;     // mirrors Main-tab (master) filter
    NonlinearSVF transientPreFilterMod1_; // mirrors unlinked Mod filter 1
    NonlinearSVF transientPreFilterMod2_; // mirrors unlinked Mod filter 2
    juce::AudioBuffer<float> transientScratch_;

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

    // -- Spectrum Analyser Capture --
    // Continuous (gap-free) mono ring buffer of recent output, written by the audio
    // thread and read by the UI's FFT. Power-of-two size for cheap wrap masking.
    static constexpr int spectrumFifoSize = 8192;
    std::array<float, spectrumFifoSize> spectrumFifo{};
    std::atomic<int> spectrumFifoWritePos{0};

    static constexpr int scopeFifoSize = 8192;
    std::array<float, scopeFifoSize> scopeFifoL{};
    std::array<float, scopeFifoSize> scopeFifoR{};
    std::atomic<int> scopeFifoWritePos{0};

    // -- Resample --
    // Far longer than the three FIFOs above, because this one is not read to draw
    // the last few milliseconds but to take a whole note back in as a waveform.
    // Sized in prepareToPlay to hold as many seconds as a waveform slot can.
    ResampleCapture resampleCapture;

    /** How far through its sample each of the five lists is, or -1 for one that
        nothing is reading. See publishUserWavePhase. */
    std::atomic<float> userWavePhase[UserWave::numGroups] {};

    /** Whether anything is watching the above. Read once per block and once per
        voice, so the whole mechanism switches off with the window that uses it. */
    std::atomic<bool> userWavePhaseWanted { false };
    
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


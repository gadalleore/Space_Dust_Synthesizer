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

    /** Rebuild the compiled form from modMatrix.

        MESSAGE THREAD ONLY -- it allocates. Call after every change to the
        routing list, and after the patch is restored. */
    void rebuildCompiledRoutings();

    /** Where one destination sits right now, modulation included.

        Only meaningful for destinations the EFFECTS read: it is refreshed per
        chunk inside runEffectsChain, which does not run until about 400 lines
        after the voices have already rendered. A voice must read
        voiceModRow() instead. */
    float effectModulatedValue (int slot) const noexcept
    {
        return effectModulated[(size_t) slot];
    }

    //==========================================================================
    // -- Per-sample modulated values for VOICE destinations --
    //
    // Filled by fillVoiceModScratch(), which processBlock calls immediately
    // BEFORE synth.renderNextBlock(). It cannot share effectModulated: the
    // voices render at the top of processBlock and the effects chain -- where
    // effectModulated is filled -- does not run until roughly 400 lines later,
    // so a voice reading effectModulated would get the PREVIOUS block's numbers.
    //
    // Only destinations that actually carry a routing get a row -- normally
    // none, sometimes a handful. A row per destination would be about 150 by
    // 512 floats, 300 KB every block, for a patch that usually modulates
    // nothing. The rows are allocated once, in prepareToPlay.

    /** How many rows fillVoiceModScratch filled for THIS block. */
    int numVoiceModRows() const noexcept { return voiceModRowsFilled; }

    /** The destination slot row `row` carries. */
    int voiceModSlotForRow (int row) const noexcept
    {
        return (row >= 0 && row < voiceModRowsFilled) ? voiceModRowSlots[row] : -1;
    }

    /** How many samples of THIS block the rows actually cover.

        Normally the whole block. It is smaller only when a host hands over a
        block bigger than the 8192-sample headroom the rows were built with,
        because nothing grows them on the audio thread -- a heap allocation
        inside processBlock would be a worse fault than covering less of a rare
        oversized block.

        A reader MUST clamp to this. Reading a row at an index at or past it
        walks off the end of that row and into the next one. Holding the last
        covered value is the right behaviour for the samples beyond it. */
    int numVoiceModSamples() const noexcept { return voiceModValidSamples; }

    /** One row of per-sample values, or nullptr when there is no such row.
        Row, not slot: the scratch holds only the destinations that carry a
        routing, so the row index is the voice's own small handful.
        Valid for numVoiceModSamples() samples, which is not always the whole
        block -- see there. */
    const float* voiceModRow (int row) const noexcept
    {
        if (row < 0 || row >= voiceModRowsFilled || voiceModValidSamples <= 0)
            return nullptr;

        return voiceModScratch.data() + (size_t) row * (size_t) voiceModRowSamples;
    }

    /** The modulated value of a voice destination at the START of this block.

        Voice knobs are pushed into the voices once per block, so column 0 of the
        scratch row is the value they get. The row holds all the columns, so a
        knob that later needs full audio rate can read them without any of this
        being restructured.

        Takes one of the vp_xxx constants from SPACEDUST_VOICE_PARAMS (defined
        in PluginProcessor.cpp), not a parameter id string: voiceParamSlots
        resolves the id to a destination slot ONCE, on the message thread, in
        rebuildCompiledRoutings, so this call is an array read on the audio
        thread rather than a std::string construction and a hash lookup.

        Returns the unmodulated value when the destination carries no routing. */
    float voiceModulatedValue (int voiceParamIndex, float fallback) const noexcept;

    // LFO buffers for per-sample access from voices. One per LFO, indexed 0..3.
    //
    // There are four buffers but only two LFOs have parameters today. The other
    // two stay SILENT: safeGetParam returns 0 for a parameter that does not
    // exist, which gives a depth of 0 and a buffer of zeros, and a zero buffer
    // contributes nothing. The parameters arrive later; the buffers are here now
    // because the modulation matrix indexes LFOs 0..3.
    juce::AudioBuffer<float> lfoBuffers[spacedust::numLfos];

    /** The buffer for one LFO, with the index held inside the array. */
    juce::AudioBuffer<float>& lfoBufferFor (int lfo) noexcept
    {
        return lfoBuffers[juce::jlimit (0, spacedust::numLfos - 1, lfo)];
    }

    const juce::AudioBuffer<float>& lfoBufferFor (int lfo) const noexcept
    {
        return lfoBuffers[juce::jlimit (0, spacedust::numLfos - 1, lfo)];
    }

    // LFO retrigger flags (public for voice access)
    std::atomic<bool> lfoRetrigger[spacedust::numLfos] { {true}, {true}, {true}, {true} };

    // Realised free-run rate of each LFO, published to the voices so their oversample
    // latch can tell that a cutoff is being swept at audio rate. Written during the
    // LFO render, read on the next block -- the latch only consults it at note start,
    // so a block of delay is immaterial.
    double lastLfoHz[spacedust::numLfos] { 0.0, 0.0, 0.0, 0.0 };

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

    // LFO current phases (public for voice access), 0.0 to 1.0
    double lfoCurrentPhase[spacedust::numLfos] { 0.0, 0.0, 0.0, 0.0 };

    /** Where each LFO is in its cycle, 0..1, for the editor's indicator bars.

        Written once per block by the audio thread and read by the editor's timer
        on the message thread. Relaxed atomics: a bar that is one frame old is
        invisible to the eye, and a lock here would be worse than the staleness.
        What is NOT acceptable is the plain double array this replaces -- that was
        a genuine data race, however harmless it looked. */
    std::atomic<float> lfoPhase01[spacedust::numLfos] { 0.0f, 0.0f, 0.0f, 0.0f };

    // LFO output smoothing (prevents clicks on retrigger/phase jumps)
    float lfoSmoothedValue[spacedust::numLfos] { 0.0f, 0.0f, 0.0f, 0.0f };

    // Sample & Hold: held random value and RNG state (audio thread only).
    // The seeds differ per LFO so four LFOs on Sample & Hold do not step in
    // lockstep, which is what the original two seeds were for.
    float lfoSampleHoldValue[spacedust::numLfos] { 0.0f, 0.0f, 0.0f, 0.0f };
    uint32_t lfoShState[spacedust::numLfos] { 12345u, 67890u, 24681u, 13579u };
    double lfoPrevPhase[spacedust::numLfos] { -1.0, -1.0, -1.0, -1.0 };  // beat-phase wrap detection

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
        Decides whether the chain is chunked at all: with nothing modulated the
        chain runs as ONE whole block, exactly as it did before the matrix. */
    bool anyEffectParameterIsModulated() const noexcept;

    /** Whether this parameter id names a knob the effects chain reads.

        "master" is deliberately absent: masterVolume is one of the six
        destinations the VOICE already applies per sample, so listing it would
        chunk the whole effects chain for a knob that does not need it, and
        risks the same knob being applied twice. */
    static bool isEffectParameter (const std::string& id) noexcept;

    /** One effect parameter read by the chain, as the modulated value when a
        routing reaches it and as the raw parameter value when none does.

        `which` is an index into the effect-parameter list in the .cpp, not a
        destination slot: the slot is resolved once on the message thread so the
        chunk loop never looks a string up. */
    float modParam (int which, float fallback = 0.0f) noexcept;

    /** Recompute effectModulated for one point in the block. Audio thread.

        Called at the top of runEffectsChain, once per chunk, and once per block
        before the Pre placements of the bit crusher and trance gate. Reads no
        strings and writes no memory the message thread owns. */
    void refreshEffectModulatedValues (int startSampleInBlock) noexcept;

    /** Take this block's routing set and tell the message thread which buffer
        is in use, so it never builds into the one being walked. Audio thread,
        at the very top of processBlock, before ANY consumer reads. */
    void latchCompiledRoutings() noexcept;

    /** Fill voiceModScratch for this block. Audio thread, BEFORE the voices
        render -- see the note on voiceModScratch. */
    void fillVoiceModScratch (int numSamples) noexcept;

    /** The single implementation of each effect, called from BOTH placements:
        the Pre site in processBlock (always startSampleInBlock == 0, never
        chunked -- the delay ahead of it isn't chunk-aware) and the Post site
        in runEffectsChain (chunked when an effect parameter is modulated).
        One body each, so a later change to how these read their parameters
        -- e.g. reading a modulated value instead of the raw APVTS value --
        takes effect at both placements instead of silently only one. */
    void processBitCrusher (juce::AudioBuffer<float>& buffer, int startSampleInBlock);
    void processTranceGate (juce::AudioBuffer<float>& buffer, int startSampleInBlock);

    //==============================================================================
    // -- The modulation matrix, as the audio thread sees it --

    /** The routings with the strings and the lookups taken out, plus the voice
        destinations picked out of them.

        The two live together so the audio thread gets ONE atomic load and a
        coherent pair: a routing list and the voice rows derived from that very
        list. */
    struct CompiledSet
    {
        std::vector<spacedust::CompiledRouting> routings;
        std::vector<int>                        voiceSlots;   // distinct, non-effect

        /** How far each LFO moves each destination that is applied INSIDE a
            per-sample loop rather than through the scratch -- see
            spacedust::PerSampleModDest. Destination-major, so
            perSampleAmounts[dest][lfo], and zero where no routing exists.

            The RAW amount, not amount * halfRange: these six-and-one keep the
            proportional formulas the deleted Destination drop-down used, where
            +1.0 is the old full swing. Everything else in this struct is
            pre-multiplied because everything else is additive around its knob.

            Fixed-size and part of the same triple-buffered set as the routings,
            so the audio thread gets it from the one latched index and cannot see
            amounts from a different edit than the routings it is walking. */
        float perSampleAmounts[spacedust::numPerSampleMod][spacedust::numLfos] { };
    };

    /** THREE buffers, an atomic index, and a reader epoch -- not two buffers.

        Two was a use-after-free waiting for Task 5. With two, a second rebuild
        inside one audio block puts the message thread back on the very buffer
        the audio thread is walking, where routings.clear() plus push_back can
        reallocate and free the array under it. That needed two edits inside one
        ~10 ms block, which was rare while setStateInformation was the only
        caller -- and becomes the common case the moment the editor rebuilds on
        every drag of an assign gesture.

        WHAT THIS GUARANTEES. The writer skips both the buffer it published last
        (liveCompiled) and the buffer the audio thread last announced
        (readerInUse), so with three buffers it always has one to build into,
        and it cannot touch the announced buffer. For a reader that has
        completed latchCompiledRoutings(), that closes the race: two, three, any
        number of consecutive rebuilds during the block leave the announced
        buffer alone.

        WHAT IT DOES NOT. The latch is a load then a separate store, not one
        atomic step. If the audio thread is pre-empted between them and TWO
        complete rebuilds finish inside that window, the second can choose the
        buffer the reader has already latched but not yet announced. The window
        is a few instructions wide and needs two full rebuilds inside it.

        This is left open ON PURPOSE. Closing it means publish-then-verify with
        a retry loop, which costs the audio thread its wait-freedom -- a
        guaranteed unbounded wait in exchange for a window this narrow is the
        wrong trade in a real-time thread. Do not "fix" it with a spin.

        The audio thread latches its index ONCE per block, in
        latchCompiledRoutings(), so every consumer in that block sees one
        coherent routing set -- the effects and the voices cannot disagree, and
        the set cannot change under a chunk loop halfway through.

        Single writer (message thread) and single reader (audio thread) is what
        makes this safe without a lock. */
    static constexpr int numCompiledBuffers = 3;

    CompiledSet      compiledBuffers[numCompiledBuffers];
    std::atomic<int> liveCompiled { 0 };

    /** The buffer the audio thread is walking this block. Written by the audio
        thread, read by the message thread when it picks where to build. */
    std::atomic<int> readerInUse { 0 };

    /** The latched index for THIS block. Audio thread only. */
    int blockCompiledIndex = 0;

    /** Base values and ranges, one per destination slot.

        Sized on the FIRST rebuild and never again: the destination table is
        built once in the constructor and never changes, so re-sizing these on
        every rebuild would write zeros into arrays the audio thread is reading
        from at that moment. destRawValues holds the same atomic the old
        safeGetParam() call looked up by name, so an unmodulated destination
        reads bit-identically to the way it read before the matrix existed. */
    std::vector<float>                destBases;
    std::vector<spacedust::DestRange> destRanges;
    std::vector<std::atomic<float>*>  destRawValues;
    std::vector<float>                effectModulated;

    /** Destination slot for each parameter the effects chain reads, resolved
        once on the message thread. -1 for a parameter that cannot be modulated
        -- a bool or a choice -- which then reads raw exactly as it did before.
        Indexed by the effect-parameter list in the .cpp. */
    std::vector<int> effectParamSlots;

    /** Destination slot for each voice-knob parameter updateVoicesWithParameters
        reads through voiceModulatedValue(), resolved once on the message
        thread. -1 for a parameter that carries no routing this block.
        Indexed by the voice-parameter list (SPACEDUST_VOICE_PARAMS) in the
        .cpp -- see voiceModulatedValue() for why this exists instead of a
        per-call slotFor() lookup. */
    std::vector<int> voiceParamSlots;

    /** Destination slot for each of the per-sample destinations, in
        spacedust::PerSampleModDest order, resolved once on the message thread.
        -1 for one this build has no parameter for.

        This is what lets rebuildCompiledRoutings fill perSampleAmounts with an
        int compare instead of comparing seven parameter ids against every
        routing's std::string. */
    std::vector<int> perSampleDestSlots;

    /** The chain's CHOICE parameters and the trance gate's sixteen step
        switches, resolved to pointers once on the message thread.

        The chunked chain reads these up to sixteen times a block. Looking them
        up by id would build a juce::String -- a heap allocation -- every time.
        Indexed by the choice list in the .cpp; a null entry means the parameter
        is missing and the caller's fallback stands. */
    std::vector<juce::AudioParameterChoice*> effectChoiceParams;
    std::atomic<float>* tranceGateStepValues[16] { };

    /** One choice parameter's index, or `fallback` when it is not there. */
    int effectChoiceIndex (int which, int fallback) const noexcept
    {
        if (which >= 0 && which < (int) effectChoiceParams.size())
            if (auto* p = effectChoiceParams[(size_t) which])
                return p->getIndex();

        return fallback;
    }

    /** Whether any live routing lands on a parameter the effects chain reads. */
    std::atomic<bool> effectsAreModulated { false };

    /** Each LFO's current value at the start of the piece being processed,
        already scaled by that LFO's Depth. */
    float lfoValues[spacedust::numLfos] { 0.0f, 0.0f, 0.0f, 0.0f };

    /** Voice scratch: maxVoiceModRows rows of voiceModRowSamples floats, one
        flat block allocated in prepareToPlay and NEVER resized afterwards --
        nothing on the audio thread grows it, so filling it cannot allocate.

        A patch that routes more than maxVoiceModRows DISTINCT voice knobs has
        the ones past the cap REFUSED when the routing is compiled, on the
        message thread, and logged -- see rebuildCompiledRoutings. They are not
        accepted and then quietly dropped here. Sixteen is far past "a
        handful". */
    static constexpr int maxVoiceModRows = 16;

    std::vector<float> voiceModScratch;
    int                voiceModRowSamples = 0;   // columns per row
    int                voiceModRowsFilled = 0;   // audio thread only
    int                voiceModValidSamples = 0; // audio thread only
    int                voiceModRowSlots[maxVoiceModRows] { };   // audio thread only

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
    // (matched to the synth's mode/cutoff/resonance), then summed into the mix, so
    // the filter cuts the transient exactly as it cuts the voices. Post mode is
    // unchanged (end-of-chain, unfiltered). This keeps the fix entirely in the
    // master chain — no change to SynthVoice.
    // NonlinearSVF (rather than juce::dsp::StateVariableTPTFilter) because the mirror
    // must offer the same five modes as the voice filter, including Notch and Peak,
    // which JUCE's SVF does not expose. Driven through setResonanceQ() so the legacy
    // mirror Q map — and therefore the existing LP/BP/HP sound — is unchanged: the
    // topology and maths are the same TPT SVF.
    NonlinearSVF transientPreFilter_;     // mirrors Main-tab (master) filter
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


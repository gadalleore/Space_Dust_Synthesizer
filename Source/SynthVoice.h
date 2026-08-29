#pragma once

#include <cstdint>
// MPE support: juce_audio_basics provides MPESynthesiserVoice, MPENote, MPEValue
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>
#include "ModMatrix.h"    // numLfos, and which destinations are applied per sample
#include "PitchCurve.h"   // The drawn pitch shape, in place of the old three-knob ramp
#include "NonlinearSVF.h" // Self-oscillating state-variable filter
#include "OversampledStage.h" // Per-sample oversampling wrapper for the nonlinear master filter
#include "RetargetableADSR.h" // juce::ADSR-faithful envelope + live release retargeting
#include "SynthSound.h"   // Kept for build compatibility (some headers still include it indirectly)
#include "OscillatorShapes.h" // The built-in shapes, shared with the Waveforms picture
#include "PhaseShaper.h"      // Bend, Spectrum and Sync -- what is done to the phase
#include "UnisonSpread.h"     // Several detuned copies of one oscillator
#include "UserWavetable.h" // Imported samples played as oscillator waveforms
#include <array>
#include <numeric>

class SpaceDustSynthesiser;
class SpaceDustAudioProcessor;

//==============================================================================
/**
    SpaceDust Synthesiser Voice (MPE-aware)

    A polyphonic voice with dual oscillators, multimode filter, and ADSR envelope.
    This voice generates cosmic subtractive synthesis tones with real-time safe processing.

    MPE migration notes:
    - Inherits from juce::MPESynthesiserVoice (was juce::SynthesiserVoice).
    - Replaces startNote / stopNote with MPE callbacks:
        noteStarted()             — equivalent of startNote; read currentlyPlayingNote
                                    (an MPENote) for note number, velocity, initial bend/timbre.
        noteStopped(bool tailOff) — equivalent of stopNote.
        notePressureChanged()     — MPE Y-axis (channel pressure / aftertouch).
        notePitchbendChanged()    — MPE pitch bend (master OR per-note); we read
                                    currentlyPlayingNote.totalPitchbendInSemitones.
        noteTimbreChanged()       — MPE Z-axis / CC74 (Seaboard slide).
        noteKeyStateChanged()     — sustain pedal etc; left empty by default.
    - juce::MPESynthesiserVoice exposes currentSampleRate (double) and the
      `currentlyPlayingNote` MPENote member.  We override setCurrentSampleRate()
      so that DSP can update if the host changes sample rate at runtime.
    - canPlaySound / controllerMoved / pitchWheelMoved are gone — MPE handles
      all of that through the MPEInstrument internally.

    Signal Path: Osc1 (with detune) + Osc2 (with detune) → Mix → Filter → ADSR Envelope → Output

    Real-time Safety: All processing is allocation-free and uses smooth parameter updates.

    MPE expression mapping (applied during renderNextBlock):
    - Pressure (Y)       → multiplicative amplitude modulation (1 + mpePressure01)
    - Pitch bend         → totalPitchbendInSemitones is added to the existing
                           pitch bend semitones (manual UI slider + this value).
    - Timbre (CC74 / Z)  → multiplicative filter cutoff modulation in log-frequency space.

    Detuning:
    - Independent detune for each oscillator (coarse + fine)
    - Applied directly to oscillator pitch before phase calculation
    - Creates shimmering, unison-like effects
*/
class SynthVoice : public juce::MPESynthesiserVoice
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
    // -- MPESynthesiserVoice overrides --
    // These are the MPE-equivalents of startNote/stopNote/controllerMoved/pitchWheelMoved.
    // Each must be fast and real-time safe (called from the audio thread).

    /** Called by MPESynthesiser when a new note has started on this voice.
        Equivalent to juce::SynthesiserVoice::startNote.  Read note info via the
        inherited `currentlyPlayingNote` MPENote member. */
    void noteStarted() override;

    /** Called by MPESynthesiser when this voice's note has stopped.
        Equivalent to juce::SynthesiserVoice::stopNote.  If allowTailOff is false,
        we still apply a short linear voice fade to avoid clicks. */
    void noteStopped (bool allowTailOff) override;

    /** Click-safe hard stop that ignores the legato/preserve handoff flags.
        Used to enforce single-voice behaviour in Mono/Legato: when a new note
        starts, any OTHER voice still ringing out a long release must be cut so
        the synth stays monophonic.  Unlike noteStopped(false), this does NOT
        defer to isPreservingVoice()/isNextNoteLegato() — the voice is always
        faded out over kVoiceFadeLength and then cleaned up in renderNextBlock. */
    void forceFadeOut();

    /** Debug-only: compact one-line snapshot of this voice's note/envelope/fade
        state, used by the loop-debug logging in PluginProcessor to see exactly
        which voice gets cut and when.  Cheap and read-only. */
    juce::String getDebugState() const;

    /** Called when the MPE pressure ("Y" axis / channel pressure) dimension changes.
        We map this to a multiplicative amplitude boost (0..+100%). */
    void notePressureChanged() override;

    /** Called when the MPE pitch-bend dimension changes (master OR per-note).
        currentlyPlayingNote.totalPitchbendInSemitones already accounts for both
        the per-note bend value and the appropriate bend range from the active
        zone (or legacy-mode range). */
    void notePitchbendChanged() override;

    /** Called when the MPE timbre ("Z" axis / CC74) dimension changes.
        Mapped to a filter cutoff modulation in log-frequency space. */
    void noteTimbreChanged() override;

    /** Called when the key state changes (e.g. sustain pedal toggled while the key
        is up).  No special handling here. */
    void noteKeyStateChanged() override;

    /** Renders this voice into the supplied buffer.  Same signature as the
        regular SynthesiserVoice override; called by MPESynthesiser. */
    void renderNextBlock(juce::AudioBuffer<float>& outputBuffer,
                         int startSample, int numSamples) override;

    /** Updates the voice's internal sample rate.  Called from the MPESynthesiser
        whenever setCurrentPlaybackSampleRate() is called on the parent synth. */
    void setCurrentSampleRate(double newRate) override;
    
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

    /** Bend, Spectrum and Sync for one oscillator -- see PhaseShaper.h.

        Set together because they are read together: the voice asks once per
        block whether any of the three would change anything, and takes the old
        plain path when none of them would. */
    /** Set where the shaping is HEADING. What the oscillator reads slides towards
        it a sample at a time -- see advanceShapingSmoothing. */
    void setOsc1WaveShaping(const PhaseShaper::Amounts& a) noexcept { osc1ShapingTarget = a; }
    void setOsc2WaveShaping(const PhaseShaper::Amounts& a) noexcept { osc2ShapingTarget = a; }

    /** The same five, for the sub oscillator.

        It had none of these, on the argument that a sub is meant to be felt
        rather than heard and that bending it puts harmonics where they are least
        wanted. That is a good DEFAULT, not a good rule -- and it already is the
        default, because all five sit at zero until they are turned up. The sub
        reads a real cycle like the other two, so the same knobs act on it. */
    void setSubOscWaveShaping(const PhaseShaper::Amounts& a) noexcept { subOscShapingTarget = a; }

    /** Unison for one oscillator: how many copies, how far apart, how wide. */
    void setOsc1Unison(int voices, float detune, float width, float phase) noexcept
    {
        updateUnison(osc1Unison, voices, detune, width, phase);
    }

    void setOsc2Unison(int voices, float detune, float width, float phase) noexcept
    {
        updateUnison(osc2Unison, voices, detune, width, phase);
    }

    void setSubOscUnison(int voices, float detune, float width, float phase) noexcept
    {
        updateUnison(subUnison, voices, detune, width, phase);
    }

    /** Unison for the noise source.

        Two different things behind one control, because the slot holds two
        different things. An IMPORTED waveform here is a third oscillator with a
        pitch, so all three knobs do what they do everywhere else. Built-in White
        and Pink have no pitch and no cycle, so Detune has nothing to act on --
        what Voices and Width buy there is a STEREO noise field: each copy is its
        own independent stream, and spreading them decorrelates the two sides.

        The level compensation differs for the same reason, which is why it is
        worked out here and not left to Unison::layout: independent noise streams
        never line up, whatever the detune says, so they sum to sqrt(N) and the
        compensation is 1/sqrt(N) flat. */
    void setNoiseUnison(int voices, float detune, float width, float phase) noexcept
    {
        updateUnison(noiseUnison, voices, detune, width, phase);
        noiseIncoherentComp = 1.0f / std::sqrt ((float) juce::jmax (1, noiseUnison.voices));
    }

    /** Hand over the imported waveforms for this block.

        The bank is owned by the processor, which holds it for the whole of
        processBlock, so the voice only borrows the pointer and never frees it. */
    void setUserWaveBank(const UserWaveBank* bank) noexcept { userWaveBank = bank; }


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
    void setFilterKeyTrack(bool enabled);        // Cutoff follows the played key when ON
    void setFilterOversample(bool enabled);      // [A/B prototype] 4x-oversample the master filter stage to kill audio-rate-modulation aliasing

    
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
    
    // Pitch curve: a shape drawn by hand, in place of the old three-knob ramp.
    // The curve itself is owned by the processor -- every voice points at the
    // SAME one, the way setLfoModAmounts points every voice at the same
    // compiled routings, so drawing a new shape reaches a held note immediately.
    void setPitchCurve(const spacedust::PitchCurve* curveToUse) noexcept { pitchCurve = curveToUse; }
    void setPitchCurveTime(float seconds);  // 0-10 s

    // Pitch bend (scaled by pitchBendAmount: 0-24 semitones) - separate from pitch envelope
    void setPitchBendAmount(float semitones);  // Range for pitch bend (0-24)
    void setPitchBend(float value);           // Manual pitch bend (-1 to 1)
    /** How far each LFO moves each of the six destinations this voice applies
        inside its own per-sample loop.

        `amounts` points at spacedust::numVoicePerSampleMod * spacedust::numLfos
        floats, laid out destination-major: amounts[dest * numLfos + lfo]. Each is
        the routing amount from the modulation matrix, -1..+1, and zero when that
        LFO does not reach that destination. The processor compiles the figure on
        the MESSAGE thread, so this call is a copy -- no lookup, no string.

        This replaces the old Destination drop-down: one LFO used to reach one
        of six things, chosen from a drop-down. Now any number of LFOs reach any
        number of them, and the per-sample formulas are unchanged. */
    void setLfoModAmounts (const float* amounts) noexcept;

    /** Current free-run frequency of each LFO, in Hz. `hzPerLfo` points at
        spacedust::numLfos doubles.

        Needed by the oversample latch. Sweeping a filter's cutoff at audio rate makes
        sidebands that fold back regardless of how linear the filter is, so resonance
        and warm saturation alone are not enough to decide whether oversampling is
        required -- see updateOversampleLatch(). */
    void setLfoRates (const double* hzPerLfo) noexcept;
    // Analog Drift: emulates hardware component tolerance and slow oscillator/filter drift
    void setAnalogDrift(float amount) { analogDriftAmount = juce::jlimit(0.0f, 1.0f, amount); }

    // MPE expression depth controls (0.0 = off, 1.0 = full).
    // These scale the raw per-note MPE values before they modulate the voice.
    void setMpePressureDepth(float depth01) { mpePressureDepth = juce::jlimit(0.0f, 1.0f, depth01); }
    void setMpeTimbreDepth(float depth01)   { mpeTimbreDepth   = juce::jlimit(0.0f, 1.0f, depth01); }
    /** How far a note's MIDI velocity sets its level and opens its filter, 0..1.

        Full velocity is the anchor: at 127 a note is unchanged whatever this is,
        and raising it only takes softer notes down and darker. So turning this up
        never makes a patch quieter than it was -- it gives the quiet end of the
        keyboard somewhere to go. */
    void setVelocityAmount(float amount01) { velocityAmount = juce::jlimit(0.0f, 1.0f, amount01); }

    void setSynthesiser(SpaceDustSynthesiser* s) { synthesiser = s; }
    void setProcessor(SpaceDustAudioProcessor* p) { processor = p; }
    
    /** For glide: when a new note uses a different voice, we need the previous pitch from another voice. */
    double getCurrentPitch() const { return juce::jlimit(20.0, 20000.0, currentPitch); }

    // Mono/legato pitch debug (see SynthVoice.cpp — SPACE_DUST_LOG_MONO_LEGATO_PITCH)
    void debugLogPitchAfterStartNote(int midiNoteNumber);
    void debugLogPitchRenderSample0(double osc1Hz, double baseHzBeforeOscTuning);

private:
    SpaceDustSynthesiser* synthesiser = nullptr;
    SpaceDustAudioProcessor* processor = nullptr;  // Pointer to processor for LFO buffer access

    //==============================================================================
    /** Where a NON-LOOPING sample has got to.

        A Full Sample slot with its loop turned off plays once and then stops, but
        the thing that reads it is a phase that turns over for ever -- the same
        phase that gives a sine its pitch. So the turn has to be COUNTED, and the
        only place that can see one is where the phase is read: an angle that
        comes back smaller than the last one has just come round.

        Kept per oscillator rather than per voice because the two oscillators, the
        sub and the noise source can each be playing a different slot at a
        different pitch, so they come round at different moments. Reset when a note
        starts, which is the same moment the phase itself is reset. */
    struct OneShotState
    {
        double previousPhase = 0.0;
        bool finished = false;

        void reset() noexcept
        {
            previousPhase = 0.0;
            finished = false;
        }
    };

    OneShotState osc1OneShot, osc2OneShot, subOscOneShot, noiseOneShot;

    //==============================================================================
    // -- Oscillator State --
    double osc1Angle = 0.0;
    double osc1AngleDelta = 0.0;
    double osc2Angle = 0.0;
    double osc2AngleDelta = 0.0;
    
    // Waveform selection. 0..OscShape::numShapes-1 are the built-in shapes;
    // anything at or above UserWave::oscUserBase is an imported slot.
    int osc1Waveform = Saw;
    int osc2Waveform = Saw;

    //==========================================================================
    // -- Bend, Spectrum and Sync --
    //
    // What is done to the PHASE before the waveform is read. All zero by default,
    // so a patch that never touches them behaves exactly as it always did.
    //
    // TWO copies, and the reason is Sync. Sync works by multiplying the phase and
    // wrapping it, so the READ POSITION depends on the sync amount. A parameter
    // read once per block and applied whole would move that position by a step
    // every block boundary -- and a step in read position is a click, which is
    // what turning the Sync knob under a held note used to sound like
    // (Giuseppe, 2026-08-26).
    //
    // So the knobs set a target and the oscillator reads a value that slides
    // towards it a sample at a time. The bends need it for the same reason; only
    // Spectrum, which crossfades a value rather than moving a position, would
    // have been safe without it, and it costs nothing to treat all five alike.
    /** Whether either oscillator is playing a slot with two channels.

        Resolved once per block in updateUserWaveSlots. The per-sample path takes
        a plainer route when it is false, because running the stereo path over a
        mono source computes a second decimation filter across a copy of the
        first -- per voice, on the common case, which is what pushed the audio
        thread over at high polyphony. */
    bool osc1Stereo = false;
    bool osc2Stereo = false;

    //==========================================================================
    // -- Unison --
    //
    // Several detuned copies of one oscillator, spread across the field. What
    // each copy's speed and position ARE is Unison::layout, which is checked
    // without a synth; what is here is the running of them.
    //
    // Each copy keeps its own phase. They are given DIFFERENT starting phases at
    // note-on, because copies that start together sum coherently for the first
    // instant and the note begins with a click of one loud copy before the detune
    // pulls them apart.
    struct UnisonState
    {
        double angle[Unison::maxVoices] {};
        Unison::Copy copies[Unison::maxVoices] {};
        int voices = 1;
        float compensation = 1.0f;

        /** How far apart the copies START in the cycle, 0..1. Kept with the
            copies rather than passed in at note-on, because updateUnison has to
            seed a copy that has only just appeared and note-on is not running
            then. */
        float phaseScatter = 0.0f;

        /** Whether this is doing anything at all. One copy at the note's own
            pitch, dead centre, is the plain oscillator -- and the render path
            takes its old route when this is false, so an untouched patch pays
            nothing for unison existing. */
        bool active() const noexcept { return voices > 1; }
    };

    UnisonState osc1Unison;
    UnisonState osc2Unison;

    /** The sub's copies, laid out by the same Unison::layout as the others.

        Width is the one control here that changes what the sub USED to be: it
        was summed to both sides at the same gain and could not be anywhere but
        the middle. At one voice this is never touched and the sub is still that
        single centred signal, so no existing patch moves. */
    UnisonState subUnison;

    /** The noise source's copies. See setNoiseUnison for why this one is not
        quite like the other three. */
    UnisonState noiseUnison;

    /** 1/sqrt(voices). Used INSTEAD of UnisonState::compensation for built-in
        White and Pink, whose copies are independent streams and so are fully
        decorrelated at every detune setting, zero included. An imported waveform
        in the noise slot is a real oscillator and takes the ordinary
        detune-dependent compensation. */
    float noiseIncoherentComp = 1.0f;

    /** One independent pink-noise generator.

        Pink noise IS its state -- the Voss-McCartney rows are what makes it pink
        -- so copies cannot share one. Reading a single generator N times per
        sample would run it N times too fast and the noise would get brighter as
        Voices went up, which is a spectrum change dressed up as a width control.

        Copy 0 is NOT here: it stays on the voice's own pinkState / pinkSum /
        pinkNoiseCounter, untouched, so one voice runs exactly the code it always
        did. These are copies 1 upwards. */
    struct PinkGenerator
    {
        std::array<float, 16> state {};
        float sum = 0.0f;
        std::uint32_t counter = 0;
    };

    PinkGenerator pinkCopies[Unison::maxVoices - 1];

    /** One Voss-McCartney step on the state handed in. */
    float nextPinkSample (std::array<float, 16>& state, float& sum,
                          std::uint32_t& counter) noexcept;

    /** Set the copies up for this block, from the three parameters. */
    void updateUnison (UnisonState& state, int voices, float detune, float width,
                       float phase) noexcept;

    /** `base` moved a random distance round the cycle, scaled by `scatter`.

        Random rather than evenly spaced, and that is not a shortcut. Copies
        spread EVENLY around one cycle sum to exactly zero -- the roots of unity
        -- which is what made Voices up with Detune down silent once before
        (measured at -157 dB, tools/unisonaudit). A random set has no such
        arrangement to fall into. */
    double scatteredPhase (double base, float scatter) noexcept;

    /** How many sets of phases to draw before picking one.

        A set of random phases has a RANDOM resultant. Seven of them land near
        sqrt(7) on average, which is the level the compensation is built for, but
        a single draw can land well either side: measured over eight notes, the
        attack ranged from 6 dB below the steady state to 6 dB above it. The low
        draws are harmless and the high ones are the very thing this knob exists
        to remove, so roughly one note in eight still arrived with the burst on it
        (tools/unisonaudit, 2026-08-27).

        Drawing a few and keeping the closest costs a few dozen sines at note-on
        and takes most of that spread out. It stays genuinely random -- no fixed
        arrangement, and in particular never the evenly spaced one that sums to
        zero -- it just declines to use the unluckiest draws.

        Sixteen and not eight, which was the first guess. Eight left the hardest
        case still moving: at Detune EXACTLY zero the copies share a frequency, so
        whatever phases they are given they keep for the whole note and there is
        no drift to wash a poor draw out. Five notes came back spread over 5 dB,
        one of them 4.24 dB down. At sixteen the same five sit inside 0.8 dB. The
        cost of the other eight draws is about a hundred sines, once, at note-on
        (tools/unisonaudit, 2026-08-27). */
    static constexpr int phaseDrawAttempts = 16;

    /** Read and advance every copy, and return the summed pair.

        Advances the copies' phases itself, because each runs at its own speed --
        the caller's single angle is not what any of them uses. The caller's angle
        is still advanced separately, so switching unison off mid-note leaves it
        where it should be.

        Returns the left channel and writes the right through rightOut. */
    float renderUnison (UnisonState& state, int waveform, const UserWaveSlot* slot,
                        double freqHz, double baseDelta,
                        const PhaseShaper::Amounts& shaping,
                        bool slotIsStereo, float& rightOut) noexcept;

    /** Give every copy its starting phase.

        At Phase 0 they all start together, on the note's own phase. That is the
        safe default and it is what every preset written before the knob existed
        gets -- but it is also what makes the first instant of a note N times
        louder than it settles, which is heard as a downward sweep on the attack.

        Turning Phase up moves the copies away from that shared start, and the
        cost is the one the old comment here warned about: a random phase per note
        makes the attack of the same note slightly different every time. On a
        short percussive patch that reads as an inconsistent transient, which is
        why this is a knob and not a fixed behaviour. */
    void seedUnisonPhases (UnisonState& state, double startAngle) noexcept;

    PhaseShaper::Amounts osc1ShapingTarget;
    PhaseShaper::Amounts osc2ShapingTarget;
    PhaseShaper::Amounts subOscShapingTarget;
    PhaseShaper::Amounts osc1Shaping;
    PhaseShaper::Amounts osc2Shaping;
    PhaseShaper::Amounts subOscShaping;

    /** How much of the remaining distance the smoothed values close each sample.
        Set from the sample rate in prepareToPlay for a fixed time in
        milliseconds, so the glide sounds the same at any rate. */
    double shapingSmoothingCoeff = 1.0;

    /** How long the shaping takes to arrive, in milliseconds.

        Short enough that the knob feels attached to the sound, long enough that
        the biggest possible jump -- Sync from nothing to eight cycles a note --
        is spread over hundreds of samples instead of landing in one. */
    static constexpr double shapingSmoothingMs = 12.0;

    /** Slide the read values one sample closer to the knobs. */
    void advanceShapingSmoothing() noexcept;

    /** Put the read values ON the knobs at once, with no glide.

        For a note that is only now starting: it has no sound yet to click, and
        gliding into the shaping from wherever the last note left it would make
        the first few milliseconds of every note depend on the one before. */
    void snapShapingToTarget() noexcept
    {
        osc1Shaping = osc1ShapingTarget;
        osc2Shaping = osc2ShapingTarget;
        subOscShaping = subOscShapingTarget;
    }
    
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
    int subOscWaveform = 1;          // 0=Sine, 1=Triangle, 2=Saw, 3=Square, 4+=imported waveform
    float subOscLevel = 0.5f;
    float subOscCoarse = 0.0f;      // Semitones
    double subOscAngle = 0.0;
    double subOscAngleDelta = 0.0;
    int noiseType = White;           // Noise type (0=White, 1=Pink, 2+=imported waveform)

    //==============================================================================
    // -- Imported waveforms --
    // The bank is owned by the processor and swapped in whole; this is only ever a
    // borrowed pointer, valid for the block it was handed over in.
    const UserWaveBank* userWaveBank = nullptr;

    // Resolved once per block from the bank and the four waveform choices, so the
    // per-sample path is a null check rather than a lookup.
    const UserWaveSlot* osc1UserSlot = nullptr;
    const UserWaveSlot* osc2UserSlot = nullptr;
    const UserWaveSlot* subOscUserSlot = nullptr;
    const UserWaveSlot* noiseUserSlot = nullptr;

    // 1.0 for the built-in shapes and for Single Cycle slots. Full Sample slots
    // stretch one turn of the phase to cover the whole file instead of one period.
    double osc1PhaseScale = 1.0;
    double osc2PhaseScale = 1.0;
    double subOscPhaseScale = 1.0;
    double noisePhaseScale = 1.0;

    // The noise source has no pitch of its own, so an imported waveform in that
    // slot needs its own phase, run at the played note's frequency.
    double noiseWaveAngle = 0.0;
    double noiseWaveAngleDelta = 0.0;
    
    // Noise EQ parameters (affects noise source only)
    float lowShelfAmount = 0.0f;     // Low shelf/cut amount (-1.0 to +1.0), affects frequencies below 200 Hz
    float highShelfAmount = 0.0f;    // High shelf/cut amount (-1.0 to +1.0), affects frequencies above 1.5 kHz
    
    // Noise EQ filter state (for simple 1-pole filters)
    float lowShelfState = 0.0f;      // Low shelf filter state (one sample delay)
    float highShelfState = 0.0f;     // High shelf filter state (one sample delay)
    
    // Pre-allocated buffers for renderNextBlock (no allocations in audio thread)
    juce::AudioBuffer<float> voiceTempBuffer;
    juce::AudioBuffer<float> voiceSingleSampleBuffer;
    
    // -- Pink Noise State (Voss-McCartney algorithm, 16 rows) --
    std::array<float, 16> pinkState{};
    float pinkSum = 0.0f;
    /** 1..65535 wrapped — keeps lowest-set-bit index in 0..15 (see SynthVoice.cpp). */
    std::uint32_t pinkNoiseCounter = 0;
    juce::Random random{static_cast<juce::int64>(reinterpret_cast<uintptr_t>(this))};  // Random number generator for noise (seeded with voice address)
    
    // Analog Drift: emulates hardware component tolerance and slow oscillator/filter drift
    float analogDriftAmount = 0.0f;
    // Per-note random factors in [-1, 1]; scaled in render by analogDriftAmount
    // (±12 cents static + ±6 cents wander per osc, ±5% + ±3.5% cutoff wander at amount=1)
    float osc1DriftOffset = 0.0f;
    float osc2DriftOffset = 0.0f;
    float filterDriftOffset = 0.0f;
    float analogOscWalk = 0.0f;    // Smoothed osc1 drift (slow independent random walk)
    float analogOscWalk2 = 0.0f;   // Smoothed osc2 drift (independent walk → evolving beating)
    float analogFilterWalk = 0.0f; // Smoothed filter cutoff wander (smoothed)
    float analogDriftWalkCoeff = 0.0f; // One-pole coeff toward white noise (~3 s time constant)
    
    // -- Filter --
    // Self-oscillating SVF: identical to a clean TPT filter for the lower ~80% of
    // the resonance knob; the top of the knob drives it into self-oscillation.
    NonlinearSVF filter;

    // Oversampling for the nonlinear filter stages (SVF + warm-sat + per-stage
    // clip). OFF => filters run at host rate, bit-identical to before. ON => 4x, so
    // the nonlinear/time-varying filter's products no longer fold back into the
    // audible band when driven by audio-rate LFO modulation. The master stage is
    // always oversampled when enabled.
    OversampledStage masterFilterOS;
    bool oversampleFilter = false;
    static constexpr int kFilterOSFactor = 4;

    // -- Per-note, per-filter oversampling latch (CPU optimisation) --
    // The 4x FIR only matters when a filter's NONLINEAR stage is actually engaged —
    // warm saturation on, or resonance high enough to clip / ring — because that is
    // the only thing that folds content above Nyquist back into the band. A near
    // linear filter (low resonance, no saturation) is bit-identical at host rate, so
    // we skip its oversampler entirely and save the FIR cost (per sample, per channel,
    // per voice). The decision is LATCHED at note-start, where the filters are already
    // reset, and never changes mid-note: switching a filter's sample-rate scale while
    // it holds resonant energy would re-introduce a note-onset click. Consequence:
    // automating resonance UP on a sustained note engages oversampling on the NEXT
    // note, not the current one (an uncommon gesture; reverts to pre-OS behaviour).
    // INVARIANT: <stage>OSActive == true  <=>  that filter's sampleRateScale == kFilterOSFactor.
    bool masterOSActive = false;
    // Resonance (0..1) at/above which a filter is treated as "needs oversampling" (~Q6).
    // Tunable: lower = safer (oversample more often), higher = bigger CPU savings.
    static constexpr float kOversampleResThreshold = 0.35f;

    // LFO rate (Hz) at/above which modulating a filter cutoff or a pitch needs
    // oversampling on its own account, whatever the resonance. Sweeping either that
    // fast is a time-varying system: it produces sidebands that fold back even through
    // a perfectly linear filter, which resonance and warm saturation say nothing about.
    //
    // 100 Hz, which is half the LFO's 200 Hz top. It was 200 while the LFO range
    // briefly reached 2 kHz; leaving it there once the range came back down would have
    // meant the latch only ever fired at the exact maximum, i.e. never in practice.
    // Still far above any vibrato or tremolo, so ordinary patches pay nothing.
    static constexpr double kOversampleLfoHzThreshold = 100.0;
    double lfoRateHz[spacedust::numLfos] { };

    //==========================================================================
    // -- Oscillator oversampling (audio-rate pitch modulation only) --
    //
    // The oscillators are deliberately naive: a saw is 2*phase and a square is a
    // hard sign flip, and that grit is the instrument's voice. Band-limiting the
    // shapes themselves was tried and measured -- 15 dB cleaner, but it sanded the
    // saws down too far, so it was reverted.
    //
    // Oversampling gets the cleanliness without touching the tone: the same naive
    // shapes are generated at 4x, so their harmonics have four times the room
    // before Nyquist, and the decimation FIR removes what is left. What comes back
    // at base rate is the same waveform with the fold-back taken out.
    //
    // It only earns its CPU when something is actually sweeping pitch fast enough
    // to fold, so it engages only for an LFO on Pitch at or above the same
    // threshold the filter path uses -- and is latched at note start for the same
    // reason (switching mid-note would jump the decimator's FIR history).
    OversampledStage oscOsc12OS;   // ch0 = osc1, ch1 = osc2
    OversampledStage oscSubOS;     // ch0 = sub
    bool oscOSActive = false;
    static constexpr int kOscOSFactor = 4;
    // Re-derive the latches from the current params and apply the matching
    // sample-rate scale + OS factor to every stage (keeps the INVARIANT above).
    void updateOversampleLatch() noexcept;

    /** Whether an LFO at or above kOversampleLfoHzThreshold carries a routing to
        one of the per-sample destinations -- the whole of what the latch needs
        to know about the modulation matrix. `destination` is a
        spacedust::PerSampleModDest below numVoicePerSampleMod. */
    bool anyFastLfoReaches (int destination) const noexcept;
    
    // Noise EQ filters: simple 1-pole shelf filters for low and high frequency shaping
    juce::dsp::IIR::Filter<float> lowShelfFilter;
    juce::dsp::IIR::Filter<float> highShelfFilter;

    /** The right channel's pair.

        The noise source was mono, so one filter each was enough. With Width up it
        is not: filtering only the left would leave the right unshelved, and the
        Low and High Shelf knobs would tilt the noise off to one side. They share
        the left pair's coefficient objects -- same numbers, separate state --
        which is what keeps the two sides identical when Width is at zero. */
    juce::dsp::IIR::Filter<float> lowShelfFilterR;
    juce::dsp::IIR::Filter<float> highShelfFilterR;
    int filterMode = 0;               // 0=LowPass, 1=BandPass, 2=HighPass, 3=Notch, 4=Peak
    float filterCutoff = 8000.0f;     // Hz (20-20000) - current modulated cutoff
    float baseFilterCutoff = 8000.0f; // Base cutoff value (unmodulated, from parameter)
    float filterResonance = 0.3f;     // Normalized (0.0-1.0, maps to Q 0.1-20.0)
    bool warmSaturationMaster = false; // Moog-style tanh saturation when ON
    bool filterKeyTrack = false;       // Master filter cutoff follows the played key when ON

    
    // -- Filter Envelope (ADSR) --
    RetargetableADSR filterAdsr;     // Filter envelope (juce::ADSR-faithful + live release retarget)
    float filterEnvAttackTime = 0.01f;   // Attack time (0.01-20.0s, skewed)
    float filterEnvDecayTime = 0.8f;     // Decay time (0.01-20.0s, skewed)
    float filterEnvSustainLevel = 0.7f;  // Sustain level (0.0-1.0, linear)
    float filterEnvReleaseTime = 3.0f;   // Release time (0.01-20.0s, skewed)
    float filterEnvAmount = 0.0f;        // -100..+100 % (blend to full-range env sweep; sign inverts E)
    
    // -- ADSR Envelope --
    // Using JUCE's built-in ADSR for reliable, professional envelope behavior.
    // This provides proper 4-stage envelope: Attack → Decay → Sustain → Release
    // with smooth transitions and proper parameter handling.
    RetargetableADSR adsr;             // Amp envelope (juce::ADSR-faithful + live release retarget)
    
    // ADSR timing parameters (in seconds) - stored for parameter updates
    float envAttackTime = 0.1f;        // Attack time (0.01-20.0s, skewed)
    float envDecayTime = 0.8f;         // Decay time (0.01-20.0s, skewed)
    float envSustainLevel = 0.7f;      // Sustain level (0.0-1.0, linear)
    float envReleaseTime = 0.2f;        // Release time (0.01-20.0s, skewed) - long cosmic tails!
    
    // Last values actually pushed into the ADSRs. We update voice parameters every
    // processBlock, but a full setParameters() recomputes releaseRate from the sustain
    // level (sustain/(release*sr)); with a low/zero sustain that rate is <= 0 and
    // recalculateRates() forces the envelope straight to idle — cutting off the release
    // tail mid-flight. So we only re-push when a value truly changed, and while a voice
    // is IN release we never call setParameters(): instead we retarget the live tail via
    // RetargetableADSR::setReleaseRetainingLevel() (re-derives the slope from the current
    // level), so changing Release re-shapes the ringing note click-free. lastAdsrRelease
    // tracks that retarget; the other lastAdsr* stay put so deferred Attack/Decay/Sustain
    // apply on the next note. Sentinel -1.0f guarantees the first real update is applied.
    float lastAdsrAttack = -1.0f, lastAdsrDecay = -1.0f, lastAdsrSustain = -1.0f, lastAdsrRelease = -1.0f;
    float lastFilterAdsrAttack = -1.0f, lastFilterAdsrDecay = -1.0f, lastFilterAdsrSustain = -1.0f, lastFilterAdsrRelease = -1.0f;

    double sampleRate = 44100.0;       // Current sample rate for envelope calculations
    bool isDspInitialized = false;     // Track if DSP has been properly initialized
    
    // Voice state
    bool isActive = false;
    bool inReleasePhase = false;   // True after noteOff() until ADSR completes

    // Anti-click: one-pole lowpass smoother on envelope output (~3ms time constant).
    float smoothedEnvelope = 0.0f;
    float envSmoothCoeff = 0.0f;   // Computed from sample rate in prepareToPlay

    // Matching smoother for the *filter* envelope. The amplitude envelope has
    // smoothedEnvelope to prevent clicks on retrigger. The filter envelope was
    // fed raw into the log-space cutoff calculation, which could perturb the
    // StateVariableTPTFilter state badly (especially with high resonance).
    float smoothedFilterEnvelope = 0.0f;
    float filterEnvSmoothCoeff = 0.0f;

    // Anti-click: slew master filter cutoff (now ~7ms base, with extra damping after steals).
    float smoothedFilterCutoffHz = 8000.0f;
    float filterCutoffSmoothCoeff = 0.0f;

    // Short-term extra damping on cutoff right after poly voice steal / new chord note.
    // The filter envelope restart on stolen voices was driving fast cutoff movement
    // that excited the observed click (confirmed by "click disappears when filter open").
    int postStealCutoffSlowdownSamples = 0;
    static constexpr int kPostStealCutoffSlowdownLength = 192; // ~4.3 ms @ 44.1 kHz
    float postStealCutoffSlowCoeff = 0.0f;  // Precomputed very slow (~35ms) slew coeff, used only during postStealCutoffSlowdownSamples window after poly steals

    // Anti-click: SNAP the cutoff smoother to the new note's target on the first
    // rendered sample of a FRESH note (poly fresh voice / mono retrigger) instead
    // of slewing from the previous note's cutoff. With key-tracking on, the high-Q
    // / self-oscillating resonant peak sits AT the note pitch, so the ~7ms slew
    // would sweep that sharp peak across the note-to-note gap → a "zip"/click on
    // note-on. Snapping is click-safe because the amplitude envelope is ~0 at note
    // start. NOT set for poly steals (they want the slow slew) or legato (glide).
    bool snapFilterCutoffOnNote = false;

    // Mono retrigger only clears a ringing filter when the previous note has decayed
    // below this amplitude — above it, zeroing the (un-enveloped) resonant output
    // pops, so the filter is left running instead. ~ -30 dB. See noteStarted().
    static constexpr float kMonoFilterResetMaxLevel = 0.03f;

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

    // Real-time discontinuity (click/pop) detector for QA.
    // Tracks the final smoothed output so we can catch single-sample jumps
    // that survive all the other anti-click measures (envelope smoother,
    // output smoother, voice fade, 3 ms auto-glide, etc.).
    float prevSmoothedL = 0.0f;
    float prevSmoothedR = 0.0f;
    // Running EMA of the per-sample |step| on each channel (the "local slope
    // envelope").  A normal waveform has a steady slope, so its step stays close
    // to this average; a true click/pop is a single step many times larger than
    // the local slope.  Comparing against this (instead of a fixed absolute
    // threshold) stops the detector firing on the natural steepness of loud/high
    // notes.  Time constant ~5 ms (see kClickSlopeEmaCoeff).
    float meanAbsDeltaL = 0.0f;
    float meanAbsDeltaR = 0.0f;
    int   discontinuityCount = 0;
    // Throttle: samples since we last logged a click on this voice.  The detector
    // runs per-sample, so an unthrottled click storm produced multi-GB logs and
    // saturated the safety ring (1.5M+ dropped entries).  Log at most ~once/100ms/voice.
    int   samplesSinceClickLog = 1 << 30;
    
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
    // Snapshot of the last meaningful currentPitch BEFORE the voice released to idle.
    // Used by computeGlideFromPitch() in mono/legato so "Legato Glide OFF / always
    // glide" works on sequential (non-overlapping) notes after the voice has fully
    // released. We don't repurpose currentPitch itself because poly mode's
    // getMaxCurrentPitch() iterates idle voices and would pick up stale values.
    double lastPlayedPitch = 0.0;
    // Per-note legato state: true when this startNote was triggered as a legato overlap
    // (set from SpaceDustSynthesiser via nextNoteIsLegato flag). This is *not* the same
    // as the global "Legato Glide" parameter – that lives in legatoGlideEnabled.
    bool isLegatoNote = false;

    uint32_t pitchTraceSeq = 0;              // incremented each startNote (mono/legato debug)
    uint32_t pitchTraceLastRenderLogSeq = 0; // so we log render line once per note-on

    // Click-debug: which allocation branch the most recent noteStarted took
    // ("fresh" / "steal" / "mono" / "none"). Logged at note-start and at each
    // detected click so we can correlate the snap with the path. (SPACEDUST_CLICK_DEBUG)
    const char* lastStartPath_ = "none";
    int dbgSamplesSinceStart_ = 0;  // samples since last noteStarted (click-debug onset/end disambiguation)

    // Global per-voice flag set from the processor: when true, glide only happens
    // on overlapping (legato) notes; when false, glide happens on every note change.
    bool legatoGlideEnabled = true;
    float glideTimeSeconds = 0.0f;     // Glide time in seconds (0.0 = instant, no glide)
    
    // Pitch curve (drawn shape, played over time from note-on)
    const spacedust::PitchCurve* pitchCurve = nullptr;  // Owned by the processor; not owned here
    float pitchCurveTime = 0.0f;          // 0-10 s
    float pitchEnvSamplesElapsed = 0.0f;  // Samples since note-on (for the curve's timebase)
    
    /** How far each LFO moves each per-sample destination, copied from the
        processor once a block by setLfoModAmounts(). Destination-major, so
        lfoModAmount[dest][lfo]. All zero means no LFO reaches any of the six and
        the per-sample loop skips the whole thing.

        Cached rather than read through the processor for the same reason the
        Destination drop-down was cached before it: the alternative is a lookup
        per sample, per voice. */
    float lfoModAmount[spacedust::numVoicePerSampleMod][spacedust::numLfos] { };

    /** Whether any entry of lfoModAmount is non-zero. Recomputed by
        setLfoModAmounts, so the sample loop tests one bool instead of walking
        twenty-four floats. */
    bool anyLfoModAmount = false;
    
    // Pitch bend (from processor, updated every block) - separate from pitch envelope.
    // pitchBendAmountFloat is the bend range used by the *manual* UI bend slider
    // (`pitchBend`).  For MPE / hardware MIDI bend we use `mpeBendSemitones` instead,
    // which is taken directly from currentlyPlayingNote.totalPitchbendInSemitones
    // (already in semitones, already weighted by the active zone's bend range or by
    // legacy-mode bend range).  The two are summed in renderNextBlock.
    float pitchBendAmountFloat = 0.0f; // 0-24 semitones (range for manual bend slider)
    float pitchBend = 0.0f;            // Manual pitch bend (-1 to 1) from UI slider

    //==============================================================================
    // -- MPE per-note expression state --
    // All three dimensions are populated in the corresponding notePressureChanged /
    // notePitchbendChanged / noteTimbreChanged callbacks AND inside noteStarted (so
    // the very first sample of a new note already reflects the controller's current
    // expression values).  They are read in renderNextBlock to modulate the voice.
    //
    // mpeBendSemitones: master + per-note pitch bend in semitones (signed double).
    //                   For legacy mode this is simply (wheel * legacyBendRange).
    //                   For MPE this is master_pb_semitones + per_note_pb_semitones.
    // mpePressure01:    pressure (channel pressure / Y axis), 0.0 .. 1.0 (centre is 0.0
    //                   under the default trackingMode = "pressureLatest").  We map
    //                   this to a multiplicative amplitude boost.
    // mpeTimbre01:      timbre (CC74 / Z axis / slide), 0.0 .. 1.0.  We map this to
    //                   filter cutoff modulation in log-frequency space.
    double mpeBendSemitones = 0.0;
    float  mpePressure01    = 0.0f;
    float  mpeTimbre01      = 0.5f;

    // MPE expression depth: 0.0 = expression dimension disabled, 1.0 = full range.
    // Set from the UI via setMpePressureDepth / setMpeTimbreDepth.
    float  mpePressureDepth = 1.0f;
    float  mpeTimbreDepth   = 1.0f;

    // -- Velocity --
    // How hard THIS note was struck, held for as long as it sounds, and how much
    // of that the patch asked to hear. Both are needed every sample: the level
    // scales the mix and the same number opens the filter.
    float  noteVelocity01  = 1.0f;
    float  velocityAmount  = 0.0f;
    float  velocityGain    = 1.0f;   // worked out once per note, not per sample
    float  velocityLogOffset = 0.0f; // ditto, in log-frequency like the other offsets
    float  velocityResonanceScale = 1.0f;  // multiplies the knob, never replaces it

    /** How much of the resonance knob a soft note gives up, at full amount.

        A quarter, which is deliberately small next to the four octaves of cutoff.
        Resonance is the one filter control with a cliff in it -- the top of the
        knob self-oscillates -- so a large velocity swing here would have notes
        crossing into and out of self-oscillation by how hard they were played.
        A quarter is enough to hear a soft note as calmer without moving which
        side of that edge the patch sits on. */
    static constexpr float velocityResonanceDepth = 0.25f;

    /** How far down velocity may take the filter, in octaves, at full amount.

        Four, not two. Two moved the level but the tone barely followed, so a soft
        note read as the same sound turned down rather than as a softer sound
        (Giuseppe, 2026-08-26: "the effect on the velocity should be more
        dramatic"). Four octaves is the distance between a bright note and a
        genuinely dull one. */
    static constexpr float velocityFilterOctaves = 4.0f;
    
    //==============================================================================
    // -- Helper Methods --
    
    /**
        Generate a waveform sample from an angle and waveform type.
        Real-time safe: no allocations, pure computation.

        userSlot short-circuits the built-in shapes: when it is non-null the
        sample the player imported is read instead. It is resolved once per block
        by refreshUserWaveSelection() rather than looked up per sample, because
        this runs up to four times per sample per oscillator per voice.

        freqHz is the pitch this oscillator is sounding at, and decides how much
        bandwidth an imported waveform may use before it would fold back. It is
        ignored by the built-in shapes.

        oneShot is where a NON-LOOPING sample keeps its place. Every oscillator
        that can play one passes its own; a built-in shape has no use for it and
        passes nothing.
    */
    /** The shaping arguments default to doing nothing, so the noise source --
        which has no Bend, Spectrum or Sync of its own -- calls this exactly as it
        always did. */
    /** Returns the LEFT channel, and writes the right through rightOut when one is
        asked for.

        Null rightOut means the caller wants one signal, which is what the noise
        source wants, and what the sub wants when its unison is not running -- and
        what an oscillator on a built-in shape or a mono slot gets anyway, since
        both sides are then the same number. Only a stereo import makes the two
        differ. */
    float generateWaveform(double angle, int waveform, const UserWaveSlot* userSlot,
                           double freqHz, OneShotState* oneShot = nullptr,
                           const PhaseShaper::Amounts& shaping = {},
                           float* rightOut = nullptr);

    /**
        Work out which imported waveform each source is set to, and what that does
        to its phase increment. Called once per block.

        Full Sample slots need the phase to sweep once per pass through the file
        rather than once per period of the note, which is a fixed multiplier per
        slot -- see UserWaveSlot::phaseIncrementScale.
    */
    void refreshUserWaveSelection() noexcept;
    
    /**
        Update filter parameters based on current mode, cutoff, and resonance.
        Called when filter parameters change.
    */
    void updateFilter();
    
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

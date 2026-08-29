#include "SynthVoice.h"
#include "SpaceDustSynthesiser.h"
#include "PluginProcessor.h"
#include "MemorySafetyLogger.h"
#include <juce_core/juce_core.h>
#include <cmath>
#include <cstdint>

namespace
{
    inline void reportDspSanitize(SpaceDustAudioProcessor* proc)
    {
        if (proc != nullptr)
            proc->dspSanitizeEventCount.fetch_add(1u, std::memory_order_relaxed);
    }

    // Keyboard tracking reference note: the played key at which key-track adds no
    // offset (cutoff == knob). MIDI 60 = middle C. Above it the cutoff rises, below
    // it falls, at 100% tracking (cutoff doubles per octave of keyboard).
    constexpr int   kFilterKeyTrackRefNote = 60;
    const     float kFilterKeyTrackLogPerSemi = static_cast<float>(std::log(2.0) / 12.0);
}

// Set to 1 to trace MIDI vs Hz in mono/legato: appends CSV rows to
// Documents/SpaceDustPitchTrace.csv (and DBG in Debug builds). Set to 0 for release.
#ifndef SPACE_DUST_LOG_MONO_LEGATO_PITCH
#define SPACE_DUST_LOG_MONO_LEGATO_PITCH 0
#endif

//==============================================================================
// CLICK DEBUG â€” set to 1 to capture, per note-start and per detected click, the
// voice allocation path + filter/envelope state to Documents\SpaceDust_ClickDebug.txt.
// Used to pin the "Sunset Beach" persistent snap. Default 0 (compiled out for release).
#ifndef SPACEDUST_CLICK_DEBUG
#define SPACEDUST_CLICK_DEBUG 0   // flip to 1 to log per-note allocation path + filter state to Documents\SpaceDust_ClickDebug.txt
#endif

#if SPACEDUST_CLICK_DEBUG
namespace
{
    // Append one line to Documents\SpaceDust_ClickDebug.txt. Bounded so a long
    // session can't grow without limit. Truncates once on first use. Not strictly
    // RT-safe (file I/O on the audio thread) â€” fine for a focused debug build; the
    // per-note + throttled-click rate keeps the write volume low.
    void clickDbgLog(const juce::String& line)
    {
        static juce::CriticalSection lock;
        static int  lineCount = 0;
        static bool inited     = false;
        const juce::ScopedLock sl(lock);
        if (lineCount > 12000) return;
        juce::File f = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                           .getChildFile("SpaceDust_ClickDebug.txt");
        if (! inited) { f.replaceWithText("=== Space Dust click debug ===\n"); inited = true; lineCount = 0; }
        f.appendText(line + "\n");
        ++lineCount;
    }
}
#endif

//==============================================================================
// -- UTF-8 String Validation Helper --
namespace
{
    // Safe string creation from number with UTF-8 validation
    juce::String safeStringFromNumber(int value)
    {
        // String from number is always valid UTF-8 (numbers are ASCII)
        return juce::String(value);
    }
    
    juce::String safeStringFromNumber(float value)
    {
        // String from number is always valid UTF-8 (numbers are ASCII)
        return juce::String(value);
    }
    
    juce::String safeStringFromNumber(double value)
    {
        // String from number is always valid UTF-8 (numbers are ASCII)
        return juce::String(value);
    }
    
    juce::String safeStringFromBool(bool value)
    {
        return value ? juce::String("true") : juce::String("false");
    }

#if SPACE_DUST_LOG_MONO_LEGATO_PITCH
    juce::File getSpaceDustPitchTraceFile()
    {
        return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
            .getChildFile("SpaceDustPitchTrace.csv");
    }

    void appendSpaceDustPitchCsvRow(const juce::String& row)
    {
        auto f = getSpaceDustPitchTraceFile();
        static bool headerWritten = false;
        if (!headerWritten)
        {
            headerWritten = true;
            // START: col5=targetHz col6=currentHz col7=glideDelta col8=legato col9=mode col10=juceNote
            // RENDER: col5=baseHz(slew+env) col6=osc1Hz col7=targetHz col8=currentHz col9=glideDelta col10=bendRatio
            f.appendText("type,seq,midi,midiHz,col5,col6,col7,col8,col9,col10,extra\n");
        }
        f.appendText(row + "\n");
    }
#endif
}

//==============================================================================
// -- MPE Note Callbacks --
//
// noteStarted() / noteStopped() / notePressureChanged() / notePitchbendChanged() /
// noteTimbreChanged() / noteKeyStateChanged() replace the old SynthesiserVoice
// methods startNote / stopNote / pitchWheelMoved / controllerMoved.  All info
// about the current note is available via the inherited `currentlyPlayingNote`
// MPENote member.

void SynthVoice::noteStarted()
{
    // The shaping starts where the knobs are, with no glide into it. A note that
    // has not sounded yet has nothing to click, and easing in from wherever the
    // last note left this voice would make the first few milliseconds of every
    // note depend on the note before it.
    snapShapingToTarget();

    // -- Read MPE note state --
    // currentlyPlayingNote is set by MPESynthesiser::startVoice() before this call.
    //   initialNote                       : MIDI note number 0..127
    //   noteOnVelocity.asUnsignedFloat()  : 0..1 velocity
    //   totalPitchbendInSemitones         : signed double â€” combined master +
    //                                       per-note pitch bend in semitones,
    //                                       already weighted by the active zone
    //                                       (or legacy-mode) bend range.
    //   pressure / timbre                 : MPEValues, asUnsignedFloat() in 0..1
    const auto& note = currentlyPlayingNote;
    const int   midiNoteNumber = static_cast<int>(note.initialNote);
    const float velocity       = note.noteOnVelocity.asUnsignedFloat();

    // Pull current MPE expression state up-front so the very first sample of
    // this note already reflects the controller's actual pressure / timbre /
    // bend values (not the zeroed defaults).  These members are also written
    // to from the MPE callbacks below as the controller moves.
    // Velocity, worked out once here rather than per sample. Full velocity is the
    // anchor: at 1.0 both of these come out neutral whatever the amount knob says,
    // so raising the knob never makes a patch quieter than it was -- it only gives
    // the soft end of the keyboard somewhere to go.
    noteVelocity01 = juce::jlimit (0.0f, 1.0f, velocity);
    {
        // Squared, not straight. Level in a straight line off velocity is a weak
        // effect: half velocity gives half amplitude, which is only 6 dB, and a
        // keyboard played gently still comes out close to full. Squaring makes
        // half velocity a quarter of the amplitude -- 12 dB -- which is the range
        // a player actually feels under their fingers.
        const float curved = noteVelocity01 * noteVelocity01;
        velocityGain = (1.0f - velocityAmount) + velocityAmount * curved;

        // The filter follows the same curve, so tone and level move together
        // rather than the sound going quiet while staying bright.
        const float shortfall = 1.0f - curved;                  // 0 at full velocity
        velocityLogOffset = -velocityAmount * shortfall
                          * velocityFilterOctaves * 0.6931471805599453f;  // ln 2 per octave

        // Resonance comes off the same curve, but only a quarter as far, and as a
        // SCALE on the knob rather than a subtraction from it. A patch with the
        // resonance knob down stays down at every velocity instead of the offset
        // being clamped away at zero, and a patch with it up keeps its character.
        velocityResonanceScale = 1.0f - velocityAmount * shortfall * velocityResonanceDepth;
    }

    mpeBendSemitones = note.totalPitchbendInSemitones;
    mpePressure01    = note.pressure.asUnsignedFloat();
    mpeTimbre01      = note.timbre.asUnsignedFloat();

    // Memory-safety logger: voice start. RT-safe (no allocations).
    SAFETY_LOG_VOICE_NOTE(note.noteID, this, midiNoteNumber,
                          (float) note.getFrequencyInHertz(),
                          "noteStarted");

    // Snapshot before this note (used for glide-from and pitch-wheel handling).
    const bool wasVoiceActive = isActive;

    // Per-note legato / stack transition (SpaceDustSynthesiser); separate from "Legato Glide" portamento.
    isLegatoNote = (synthesiser != nullptr) ? synthesiser->getAndClearNextNoteLegato() : false;

    const int voiceMode = (synthesiser != nullptr) ? synthesiser->getVoiceModeIndex() : 0;

    // Cancel pending voice fade, but be careful in poly mode during voice stealing.
    // In mono/legato we want instant takeover. In poly chord changes (voice steal),
    // we prefer to let a short fade complete or blend to avoid a hard jump when
    // the same voice is reassigned to a new note while its previous tail was decaying.
    const bool isPolySteal = (voiceMode == 0) && wasVoiceActive;
    if (isPolySteal)
    {
        // This is the exact moment a poly voice is stolen for a new chord note.
        // The previous waveform contribution from this voice is about to be replaced.
        SAFETY_LOG_VOICE_NOTE(note.noteID, this, midiNoteNumber,
                              (float)note.getFrequencyInHertz(),
                              "POLY_VOICE_STEAL_START");

        // Activate extra cutoff damping for the next ~4 ms to tame any rapid
        // movement coming from the (re)starting filter envelope on this stolen voice.
        postStealCutoffSlowdownSamples = kPostStealCutoffSlowdownLength;
    }

    // Always cancel any pending voice fade on note start.
    // Leaving an old fade running on a stolen poly voice is dangerous because
    // the fade completion code will do full cleanup (clearCurrentNote + isActive=false),
    // which would kill the new note after only ~64 samples.
    // The anti-click protection for poly steals now comes from:
    // - Preserving oscillator phases
    // - Not zeroing smoothedEnvelope / outputSmoothers
    // - Not resetting the filter
    // - The 3ms smoothedFilterEnvelope + post-steal cutoff slowdown
    voiceFade = 1.0f;
    voiceFadeSamplesRemaining = 0;

    // MPE: pitch wheel is no longer fed via a separate startNote() argument.
    // mpeBendSemitones (set above from currentlyPlayingNote.totalPitchbendInSemitones)
    // is the single source of truth for both the master pitch wheel AND the per-note
    // MPE bend.  For mono/legato voice handoff we deliberately KEEP the existing
    // mpeBendSemitones value (don't reset to 0) so the wheel doesn't snap on legato
    // overlaps â€” MPESynthesiser will fire notePitchbendChanged() if it actually
    // changed.

    // Target pitch: MIDI note frequency (base frequency, tuning applied in renderNextBlock)
    auto baseFrequency = juce::MidiMessage::getMidiNoteInHertz(midiNoteNumber);
    targetPitch = baseFrequency;
    targetPitch = juce::jlimit(20.0, 20000.0, targetPitch);

    // Analog Drift: emulates hardware component tolerance and slow oscillator/filter drift
    // New random draw each non-legato note; legato overlaps keep prior offsets (same "analog voice").
    {
        const bool resampleAnalogDrift = ! (isLegatoNote && voiceMode == 2);
        if (resampleAnalogDrift)
        {
            if (analogDriftAmount > 0.0f)
            {
                osc1DriftOffset = random.nextFloat() * 2.0f - 1.0f;
                osc2DriftOffset = random.nextFloat() * 2.0f - 1.0f;
                filterDriftOffset = random.nextFloat() * 2.0f - 1.0f;
            }
            else
            {
                osc1DriftOffset = 0.0f;
                osc2DriftOffset = 0.0f;
                filterDriftOffset = 0.0f;
            }
            analogOscWalk = 0.0f;
            analogOscWalk2 = 0.0f;
            analogFilterWalk = 0.0f;
        }
    }
    
    // LFO Retrigger: Reset LFO phases if retrigger is enabled
    if (processor != nullptr)
    {
        try
        {
            if (processor->lfoRetrigger[0].load())
            {
                processor->lfoCurrentPhase[0] = 0.0;
            }
        }
        catch (...) { /* ignore */ }

        try
        {
            if (processor->lfoRetrigger[1].load())
            {
                processor->lfoCurrentPhase[1] = 0.0;
            }
        }
        catch (...) { /* ignore */ }
    }
    
    // Decide whether this note change should glide based on:
    // - Glide time
    // - Global Legato Glide parameter
    // - Whether this specific note is a legato overlap
    //
    // Behaviour:
    // - Legato Glide ON  (legatoGlideEnabled = true):
    //     * In Mono OR Legato voice mode: glide ONLY on overlapping notes
    //       (isLegatoNote == true)  â†’ classic "fingered glide" / portamento
    //     * In Poly mode: always glide when glide time > 0 (no overlap concept)
    // - Legato Glide OFF (legatoGlideEnabled = false):
    //     * Glide applies to every note change whenever glideTimeSeconds > 0
    //
    // Envelope retrigger is mode-specific and independent of this gate:
    //     * Mono   (voiceMode == 1): hard restart envelope on every new note
    //     * Legato (voiceMode == 2): preserve envelope on overlapping notes
    const bool glideTimeActive = (glideTimeSeconds > 0.0f && sampleRate > 0.0);
    const bool inLegatoMode = (voiceMode == 2);
    const bool inMonoMode   = (voiceMode == 1);
    // Mono/legato retrigger: same voice is being reused (already active).
    // We preserve oscillator phases for click-free transitions.
    const bool isMonoRetrigger = (voiceMode != 0) && isActive;
    // wasVoiceActive: if the voice was idle, currentPitch must not leak from the previous session
    // (mono + glide would glide from a stale Hz on the next note after stack release / repetition).
    bool shouldGlideForThisNote = false;

    if (glideTimeActive)
    {
        if (legatoGlideEnabled && (inLegatoMode || inMonoMode))
            shouldGlideForThisNote = isLegatoNote;     // fingered glide: only overlapping notes glide
        else
            shouldGlideForThisNote = true;             // normal glide: every note change glides
    }

    // Poly mode only: when this voice has no valid currentPitch yet, use max pitch from other
    // voices for glide "from". Mono/Legato must NEVER use that â€” it picked another voice's Hz
    // and caused random detuning on every note change (regression from aggressive pitch resync).
    const bool allowCrossVoiceGlideFrom = (voiceMode == 0);

    auto computeGlideFromPitch = [this, allowCrossVoiceGlideFrom, wasVoiceActive, voiceMode]
                                  (double fallBackTarget) -> double
    {
        double fromPitch = currentPitch;
        // Poly mode: don't reuse currentPitch when the voice was idle â€” that voice
        // might have last played a completely unrelated note.
        if (!wasVoiceActive && voiceMode == 0)
            fromPitch = 0.0;
        // Mono/Legato fallback: render zeroes currentPitch when the ADSR fully
        // releases. lastPlayedPitch survives that wipe so "Legato Glide OFF /
        // always glide" can still glide on sequential (non-overlapping) notes.
        if (fromPitch <= 0.0 && (voiceMode == 1 || voiceMode == 2) && lastPlayedPitch > 0.0)
            fromPitch = lastPlayedPitch;
        if (fromPitch <= 0.0 && allowCrossVoiceGlideFrom && synthesiser != nullptr)
            fromPitch = synthesiser->getMaxCurrentPitch();
        if (fromPitch <= 0.0)
            fromPitch = fallBackTarget;
        return fromPitch;
    };

    if (isLegatoNote && inLegatoMode)
    {
        // Legato overlapping in Legato mode:
        // - Envelopes and filter stay in their current stage (single-trigger behaviour)
        // - Pitch either glides (if shouldGlideForThisNote) or jumps instantly
        if (shouldGlideForThisNote)
        {
            const double fromPitch = computeGlideFromPitch(targetPitch);
            currentPitch = fromPitch;
            const double pitchDifference = targetPitch - currentPitch;
            const double samplesToGlide = glideTimeSeconds * sampleRate;
            glideDelta = (samplesToGlide > 0.0) ? (pitchDifference / samplesToGlide) : 0.0;
            updateOsc1Frequency(currentPitch);
            updateOsc2Frequency(currentPitch);
        }
        else
        {
            // Decide 3ms vs snap using resynced "from" pitch (not raw currentPitch alone), so
            // stack-return cases don't mis-route before computeGlideFromPitch runs.
            const double fromPitch = computeGlideFromPitch(targetPitch);
            if (fromPitch > 0.0 && std::abs(fromPitch - targetPitch) > 0.01)
            {
                // Anti-click: legato with no user glide â€” tiny 3ms auto-glide
                currentPitch = fromPitch;
                const double samplesToGlide = 0.003 * sampleRate;
                glideDelta = (targetPitch - currentPitch) / samplesToGlide;
                updateOsc1Frequency(currentPitch);
                updateOsc2Frequency(currentPitch);
            }
            else
            {
                currentPitch = targetPitch;
                glideDelta = 0.0;
                updateOsc1Frequency(currentPitch);
                updateOsc2Frequency(currentPitch);
            }
        }

        updateFilter();
        isActive = true;
        debugLogPitchAfterStartNote(midiNoteNumber);
        return; // IMPORTANT: do NOT re-trigger ADSR on legato overlaps
    }

    // Non-legato start (or non-Legato voice mode): set pitch and optionally start a glide.
    if (shouldGlideForThisNote)
    {
        const double fromPitch = computeGlideFromPitch(targetPitch);
        currentPitch = fromPitch;
        const double pitchDifference = targetPitch - currentPitch;
        const double samplesToGlide = glideTimeSeconds * sampleRate;
        glideDelta = (samplesToGlide > 0.0) ? (pitchDifference / samplesToGlide) : 0.0;
        updateOsc1Frequency(currentPitch);
        updateOsc2Frequency(currentPitch);
    }
    else if (isMonoRetrigger && currentPitch > 0.0 && std::abs(currentPitch - targetPitch) > 0.01)
    {
        // Anti-click: mono retrigger with no user glide â€” apply a tiny 3ms auto-glide
        // to prevent the abrupt frequency change that causes clicks at low frequencies.
        // 3ms is imperceptible as a glide but smooths the waveform transition.
        const double samplesToGlide = 0.003 * sampleRate;
        glideDelta = (targetPitch - currentPitch) / samplesToGlide;
        updateOsc1Frequency(currentPitch);
        updateOsc2Frequency(currentPitch);
    }
    else
    {
        currentPitch = targetPitch;
        glideDelta = 0.0;
        updateOsc1Frequency(currentPitch);
        updateOsc2Frequency(currentPitch);
    }

    debugLogPitchAfterStartNote(midiNoteNumber);

    // Full retrigger: new envelope, filter, and modulator cycles
    pitchEnvSamplesElapsed = 0.0f;

    // Anti-click strategy (updated after 7s poly chord transition investigation):
    // - Mono/Legato: phases preserved, smoothedEnvelope catches retrigger jumps.
    // - Poly fresh voice: full reset is acceptable (no previous audio from this voice).
    // - Poly voice steal (chord change while previous notes are still sounding):
    //   Hard-resetting phases + zeroing smoothers on a voice that was contributing
    //   audio is the root cause of the observed polarity-flip clicks at beat-grid
    //   aligned chord transitions (see the two-step noteStopped(false) â†’
    //   noteStarted() sequence that defeats the voiceFade).
    //   We now:
    //     * Leave any pending voiceFade running.
    //     * Keep oscillator phases continuous.
    //     * Do NOT call adsr.reset() / filterAdsr.reset() on steal (just noteOn()).
    //     * Do NOT zero smoothedEnvelope / smoothedFilterEnvelope / outputSmoothers.
    //     * Let the 3 ms lowpasses naturally blend the old decaying contribution
    //       into the new note's attack. This gives clean poly note starts while
    //       preventing the amplitude cliff / sign reversal.
    //
    // We also added smoothedFilterEnvelope (matching the 3ms amplitude smoother)
    // because raw filterAdsr jumps into the log-space cutoff math were perturbing
    // the StateVariableTPTFilter internal state (especially at high resonance).
    const bool shouldHardResetForPoly = !isMonoRetrigger && !isPolySteal;

    // A non-looping sample starts again on every note, by every path into this
    // function -- a fresh voice, a stolen one, a mono retrigger. A note that is
    // played is a note that is meant to be heard, and a one-shot that stayed
    // finished from the last note would answer it with silence.
    //
    // Above the branch, because only one of the three paths resets the phases and
    // the other two carry on from wherever theirs are. Zero is a safe place to
    // start counting from either way: a phase is never below it, so the first
    // sample after this can never be mistaken for a turn.
    osc1OneShot.reset();
    osc2OneShot.reset();
    subOscOneShot.reset();
    noiseOneShot.reset();

    // A WHOLE SAMPLE starts again on every note, legato included.
    //
    // The phase of a built-in shape is a position in a cycle, and carrying it
    // across a legato note is exactly what makes legato smooth -- so the branch
    // below deliberately leaves it alone on a retrigger or a steal.
    //
    // The phase of a Full Sample slot is not that. It is a position in a
    // RECORDING, so carrying it across means the new note picks the sample up
    // half way through and plays whatever was left of the last one
    // (Giuseppe, 2026-08-26). Reset here, above the branch, so it happens by
    // every path into this function.
    //
    // A slot may not be resolved yet on a voice's very first note -- the bank is
    // looked up per block -- and null is a built-in, which wants the old
    // behaviour anyway.
    const auto restartWholeSample = [] (const UserWaveSlot* slot, double& angle)
    {
        if (slot != nullptr && slot->mode == UserWave::Mode::FullSample)
            angle = 0.0;
    };

    restartWholeSample (osc1UserSlot, osc1Angle);
    restartWholeSample (osc2UserSlot, osc2Angle);
    restartWholeSample (subOscUserSlot, subOscAngle);
    restartWholeSample (noiseUserSlot, noiseWaveAngle);

    if (shouldHardResetForPoly)
    {
        // Truly new poly voice: safe to do full reset
        osc1Angle = 0.0;
        osc2Angle = 0.0;
        subOscAngle = 0.0;
        noiseWaveAngle = 0.0;
        pinkState.fill(0.0f);
        pinkSum = 0.0f;
        pinkNoiseCounter = 0;
        for (auto& copy : pinkCopies)
        {
            copy.state.fill(0.0f);
            copy.sum = 0.0f;
            copy.counter = 0;
        }
        random.setSeed(static_cast<juce::int64>(reinterpret_cast<uintptr_t>(this)) + juce::Time::getHighResolutionTicks());
        for (auto& val : pinkState)
            val = (random.nextFloat() * 2.0f - 1.0f) * 0.0625f;
        pinkSum = std::accumulate(pinkState.begin(), pinkState.end(), 0.0f);
        adsr.reset();
        smoothedEnvelope = 0.0f;
        smoothedFilterEnvelope = 0.0f;
        outputSmootherL = 0.0f;
        outputSmootherR = 0.0f;
        prevSmoothedL = 0.0f;
        prevSmoothedR = 0.0f;
        meanAbsDeltaL = 0.0f;
        meanAbsDeltaR = 0.0f;
        postStealCutoffSlowdownSamples = 0;
        filter.reset();
        // CRITICAL: also clear the filter OVERSAMPLERS. filter.reset() only zeroes the
        // SVF state; the oversampler keeps a ~17-sample FIR history of the PREVIOUS note.
        // Left un-reset, a fresh voice's first samples convolve out that stale tail â€”
        // a step from 0 to Â±0.1+ at note-start while the amp envelope is still 0 (the
        // "Sunset Beach" snap; loudest on fast playing, which leaves big residue in the FIR).
        masterFilterOS.reset();
        oscOsc12OS.reset(); oscSubOS.reset();
        // Clean start: latch which filters actually need oversampling for THIS note
        // and apply the matching sample-rate scale before updateFilter() recomputes g.
        updateOversampleLatch();
        filterAdsr.reset();

        adsr.noteOn();
        inReleasePhase = false;
        isActive = true;
        updateFilter();
        filterAdsr.noteOn();
        // Fresh voice: put the cutoff at the new note's value immediately (no slew
        // sweep of the resonant peak into the note). See snapFilterCutoffOnNote.
        snapFilterCutoffOnNote = true;
        lastStartPath_ = "fresh";
    }
    else if (isPolySteal)
    {
        // Poly voice steal during chord change: keep phases continuous to avoid
        // the polarity flip. Still retrigger envelopes (desired poly behaviour),
        // but the existing smoothedEnvelope + any remaining voiceFade will
        // prevent a hard click. Filter state is also left running.
        SAFETY_LOG_VOICE_NOTE(note.noteID, this, midiNoteNumber,
                              (float)note.getFrequencyInHertz(),
                              "POLY_VOICE_STEAL_NO_PHASE_RESET");

        // Per the analysis: do NOT call reset() here. ...
        // filterAdsr is left untouched for the same reason.
        adsr.noteOn();
        filterAdsr.noteOn();

        // Extra diagnostic: mark exactly when the filter envelope restarts on a steal.
        // Combined with AUDIO_CLICK_DETECTED + current cutoff, this will show if
        // the click is tightly correlated with filterAdsr.noteOn() during chord changes.
        SAFETY_LOG_VOICE_NOTE(note.noteID, this, midiNoteNumber,
                              smoothedFilterCutoffHz,
                              "POLY_STEAL_FILTER_ENV_RESTART");
        inReleasePhase = false;
        isActive = true;
        lastStartPath_ = "steal";
        // phases, pink state, filter, outputSmoother*, and smoothed*Envelopes
        // are deliberately left running from the stolen voice's previous state.
    }
    else
    {
        // Mono (voiceMode == 1): HARD restart envelope from 0 on every new note.
        // This is the defining mono behaviour and matches classic mono synth feel.
        // Oscillator phases stay continuous, and smoothedEnvelope (~3ms lowpass)
        // catches the jump so the level transition is click-free.
        //
        // Legato (voiceMode == 2, non-overlapping start that still found the voice
        // active): continue envelope from its current level for smooth re-entry.
        // Legato OVERLAPS were handled by the early-return above, which preserves
        // the envelope completely.
        if (inMonoMode)
        {
            // Read the voice's CURRENT loudness before we reset the amp envelope.
            // (smoothedEnvelope still holds the last sample of the previous note.)
            // The filter's resonant / self-osc output is NOT gated by the amplitude
            // envelope, so a still-loud previous note means the filter is ringing at
            // an AUDIBLE level â€” and you cannot move that ring abruptly without an
            // artifact: resetting it steps the output to zero (POP), and snapping its
            // cutoff jumps the resonant peak to a new frequency (CLICK, worst with
            // key-tracking, where every note has a different cutoff).
            const bool prevNoteQuiet = (smoothedEnvelope < kMonoFilterResetMaxLevel);

            adsr.reset();
            filterAdsr.reset();

            if (prevNoteQuiet)
            {
                // Previous note has decayed near silence (gaps / short release): treat
                // this as a fresh note start. Clear any stale ring (inaudible at this
                // level) and SNAP the cutoff to the new note so the resonant peak
                // doesn't sweep into the attack (the note-on "zip").
                if (filter.isRinging()) filter.reset();
                // Clear the oversampler FIR history too (see fresh-voice path above) so a
                // clean mono start can't dump the previous note's stale tail samples.
                masterFilterOS.reset();
                oscOsc12OS.reset(); oscSubOS.reset();
                // Clean mono start: re-latch oversampling for this note (see fresh path).
                updateOversampleLatch();
                snapFilterCutoffOnNote = true;
            }
            else
            {
                // Previous note still loud (long Release + fast "running bass"). Leave
                // the ringing filter running (no reset â†’ no pop) and move the cutoff
                // GENTLY to the new note via the slow-slew window (no snap â†’ no click).
                // A smooth filter glide between crashing notes instead of an abrupt
                // peak jump. snapFilterCutoffOnNote stays false here.
                postStealCutoffSlowdownSamples = kPostStealCutoffSlowdownLength;
            }
        }
        adsr.noteOn();
        filterAdsr.noteOn();
        inReleasePhase = false;
        isActive = true;
        lastStartPath_ = inMonoMode ? "mono" : "legato";
    }

    // AFTER the three branches above, and that position is the whole point.
    //
    // Each copy is seeded from its oscillator's angle and, with Phase up, from
    // the voice's random generator. The hard-reset branch sets those angles to
    // zero and re-seeds that generator -- so seeding the copies before it, which
    // is where this used to sit, scattered them around an angle the note was
    // about to abandon, and drew their random offsets from the generator state
    // the note was about to replace. The draw was then the SAME on every fresh voice: five different
    // notes came back at identical levels to two decimals, which is not what a
    // random phase is for (measured, tools/unisonaudit).
    seedUnisonPhases (osc1Unison, osc1Angle);
    seedUnisonPhases (osc2Unison, osc2Angle);
    seedUnisonPhases (subUnison, subOscAngle);
    seedUnisonPhases (noiseUnison, noiseWaveAngle);

    dbgSamplesSinceStart_ = 0;
#if SPACEDUST_CLICK_DEBUG
    clickDbgLog("NOTE_START v=" + juce::String::toHexString((juce::pointer_sized_int) this).getLastCharacters(4)
                + " note=" + juce::String(midiNoteNumber)
                + " path=" + juce::String(lastStartPath_)
                + " wasActive=" + juce::String((int) wasVoiceActive)
                + " prevEnv=" + juce::String(smoothedEnvelope, 4)
                + " cutHz=" + juce::String(smoothedFilterCutoffHz, 1)
                + " fAdsrAct=" + juce::String((int) filterAdsr.isActive())
                + " mode=" + juce::String(voiceMode));
#endif
}

void SynthVoice::noteStopped(bool allowTailOff)
{
    // MPE replacement for juce::SynthesiserVoice::stopNote(velocity, allowTailOff).
    // The semantics are identical: allowTailOff=true â†’ release the envelope normally;
    // allowTailOff=false â†’ hard stop (we apply a short voice fade to avoid clicks).

    // Memory-safety logger: voice stop. Only log when the voice was actually
    // active â€” turnOffAllVoices()/prepare sweeps call noteStopped() on every
    // voice including idle ones, which previously flooded the log with thousands
    // of redundant "HARD" entries per transport edge.
    if (isActive)
    {
        SAFETY_LOG_VOICE_NOTE(currentlyPlayingNote.noteID, this,
                              (int) currentlyPlayingNote.initialNote,
                              allowTailOff ? 1.0f : 0.0f,
                              allowTailOff ? "noteStopped tailOff"
                                           : "noteStopped HARD (voice steal/allNotesOff)");
    }

    // Preserving-voice / legato handoff: noteStarted follows immediately inside
    // MPESynthesiser::startVoice.  Do NOT start a fade or touch ADSR â€” just
    // clear the currentlyPlayingNote so the synth can reassign.  The voice keeps
    // producing audio with all DSP state intact (oscillator phases, ADSR level,
    // filter).  This is the standard JUCE mono/legato approach.
    if (synthesiser != nullptr
        && (synthesiser->isPreservingVoice() || synthesiser->isNextNoteLegato()))
    {
        SAFETY_LOG_VOICE(currentlyPlayingNote.noteID, this,
                         "noteStopped: legato/preserve handoff (DSP state kept)");
        clearCurrentNote();
        return;
    }

    if (allowTailOff)
    {
        // Normal release: ADSR handles the tail naturally
        adsr.noteOff();
        filterAdsr.noteOff();
        inReleasePhase = true;
    }
    else
    {
        // Hard stop (turnOffAllVoices, voice stealing, etc.): start a short linear
        // fade-out instead of instantly killing the signal.  The voice keeps
        // running with all DSP intact while voiceFade ramps 1â†’0.  Only when
        // it reaches zero does renderNextBlock do the full cleanup.
        voiceFade = 1.0f;
        voiceFadeSamplesRemaining = kVoiceFadeLength;
    }
}

//==============================================================================
void SynthVoice::forceFadeOut()
{
    // Nothing to cut if the voice is neither sounding nor mid-fade.
    if (! isActive && voiceFadeSamplesRemaining <= 0)
        return;

    // Mono/Legato single-voice guarantee: this is a STRAY voice (e.g. a long
    // release left over from a previous note or a polyâ†’mono switch) that must
    // not keep ringing under the new note.  Bypass the legato/preserve handoff
    // (we are NOT reusing this voice) and start the short click-safe fade; the
    // existing fadeâ†’cleanup path in renderNextBlock finishes the job.
    voiceFade = 1.0f;
    voiceFadeSamplesRemaining = kVoiceFadeLength;
}

//==============================================================================
juce::String SynthVoice::getDebugState() const
{
    juce::String s;
    s << (isActive ? "ACT" : "idle");
    s << " n=" << (int) currentlyPlayingNote.initialNote
      << " id=" << (int) currentlyPlayingNote.noteID;
    s << (adsr.isActive() ? " adsrOn" : " adsrOff");
    if (inReleasePhase) s << " REL";
    if (voiceFadeSamplesRemaining > 0) s << " FADE(" << voiceFadeSamplesRemaining << ")";
    return s;
}

//==============================================================================
// -- MPE Expression Callbacks --
//
// Fired from MPESynthesiser when the controller updates pressure / pitch-bend /
// timbre / key state for this voice's currently playing MPENote.  These are
// real-time safe (called in the audio rendering callback by MPEInstrument),
// so we keep them lock-free â€” just cache the new value, the audio thread will
// pick it up in the next renderNextBlock sample loop.

void SynthVoice::notePressureChanged()
{
    // pressure is 0..1; we apply it as multiplicative amplitude boost in renderNextBlock.
    mpePressure01 = currentlyPlayingNote.pressure.asUnsignedFloat();
}

void SynthVoice::notePitchbendChanged()
{
    // totalPitchbendInSemitones combines master + per-note bend and is already
    // weighted by the active zone's bend range (or legacy-mode bend range).
    // Replaces the old pitchWheelMoved logic.
    mpeBendSemitones = currentlyPlayingNote.totalPitchbendInSemitones;
}

void SynthVoice::noteTimbreChanged()
{
    // timbre (CC74 / Z) is 0..1; mapped to filter cutoff offset in renderNextBlock.
    mpeTimbre01 = currentlyPlayingNote.timbre.asUnsignedFloat();
}

void SynthVoice::noteKeyStateChanged()
{
    // No-op: sustain/sostenuto pedal state changes don't require any audio-side
    // action here â€” the ADSR is already in its sustain/release stage as appropriate.
}

void SynthVoice::debugLogPitchAfterStartNote(int midiNoteNumber)
{
#if SPACE_DUST_LOG_MONO_LEGATO_PITCH
    if (synthesiser == nullptr || synthesiser->getVoiceModeIndex() == 0)
        return;
    ++pitchTraceSeq;
    const int mode = synthesiser->getVoiceModeIndex();
    const double midiHz = juce::MidiMessage::getMidiNoteInHertz(midiNoteNumber);
    // MPE: getCurrentlyPlayingNote() now returns MPENote (not int).
    const int jn = currentlyPlayingNote.isValid() ? static_cast<int>(currentlyPlayingNote.initialNote) : -1;
    juce::String extra;
    if (std::abs(targetPitch - midiHz) > 0.5)
        extra += "target_neq_midiHz;";
    if (jn >= 0 && jn != midiNoteNumber)
        extra += "juceNote_neq_param;";

    appendSpaceDustPitchCsvRow(
        juce::String("START,") + juce::String((int)pitchTraceSeq) + "," + juce::String(midiNoteNumber) + ","
        + juce::String(midiHz, 6) + "," + juce::String(targetPitch, 6) + "," + juce::String(currentPitch, 6) + ","
        + juce::String(glideDelta, 6) + "," + juce::String(isLegatoNote ? 1 : 0) + "," + juce::String(mode) + ","
        + juce::String(jn) + ",\"" + extra + "\"");

    DBG("Space Dust [pitch] #" << pitchTraceSeq << " START  MIDI=" << midiNoteNumber
        << " (" << juce::MidiMessage::getMidiNoteName(midiNoteNumber, true, true, 3) << ")"
        << "  targetHz=" << targetPitch << "  currentHz=" << currentPitch << "  glideDelta=" << glideDelta
        << "  legato=" << (isLegatoNote ? "y" : "n") << "  mode=" << mode
        << "  jucePlayingNote=" << jn << "  midiHz(ref)=" << midiHz);
#else
    juce::ignoreUnused(midiNoteNumber);
#endif
}

void SynthVoice::debugLogPitchRenderSample0(double osc1HzFinal, double baseHzAfterPitchEnv)
{
#if SPACE_DUST_LOG_MONO_LEGATO_PITCH
    if (synthesiser == nullptr || synthesiser->getVoiceModeIndex() == 0)
        return;
    const int n = currentlyPlayingNote.isValid() ? static_cast<int>(currentlyPlayingNote.initialNote) : -1;
    const double midiHz = (n >= 0 ? juce::MidiMessage::getMidiNoteInHertz(n) : 0.0);
    // MPE bend = mpeBendSemitones (from controller / wheel) + manual UI bend.
    const float manualBendSt = juce::jlimit(-1.0f, 1.0f, pitchBend) * pitchBendAmountFloat;
    const double bendSt = mpeBendSemitones + static_cast<double>(manualBendSt);
    const double bendRatio = std::pow(2.0, bendSt / 12.0);
    juce::String extra = "mpeBendSt=" + juce::String(mpeBendSemitones, 4)
                       + ";manualBendSt=" + juce::String((double)manualBendSt, 4);

    appendSpaceDustPitchCsvRow(
        juce::String("RENDER,") + juce::String((int)pitchTraceSeq) + "," + juce::String(n) + ","
        + juce::String(midiHz, 6) + "," + juce::String(baseHzAfterPitchEnv, 6) + "," + juce::String(osc1HzFinal, 6) + ","
        + juce::String(targetPitch, 6) + "," + juce::String(currentPitch, 6) + "," + juce::String(glideDelta, 6) + ","
        + juce::String(bendRatio, 6) + ",\"" + extra + "\"");

    const juce::String noteName = (n >= 0 ? juce::MidiMessage::getMidiNoteName(n, true, true, 3) : juce::String("?"));
    DBG("Space Dust [pitch] #" << pitchTraceSeq << " RENDER  MIDI=" << n << " (" << noteName << ")"
        << "  baseHz(slew+env)=" << baseHzAfterPitchEnv << "  osc1Hz(final)=" << osc1HzFinal
        << "  targetHz=" << targetPitch << "  currentHz=" << currentPitch << "  glideDelta=" << glideDelta
        << "  bendRatio=" << bendRatio << "  midiHz(ref)=" << midiHz);
#else
    juce::ignoreUnused(osc1HzFinal, baseHzAfterPitchEnv);
#endif
}

//==============================================================================
// -- MIDI Controllers --
//
// MPESynthesiser routes all expression (pitch bend, channel pressure, CC74 timbre)
// through the MPENote dimensions, surfaced via the notePitchbendChanged /
// notePressureChanged / noteTimbreChanged callbacks above.  We therefore no
// longer need controllerMoved() or pitchWheelMoved() overrides â€” they belonged
// to the old juce::SynthesiserVoice API.  Generic non-MPE MIDI CCs would arrive
// at SpaceDustSynthesiser::handleController if we ever choose to override it.

//==============================================================================
// -- Waveform Generation --

void SynthVoice::refreshUserWaveSelection() noexcept
{
    osc1UserSlot = nullptr;
    osc2UserSlot = nullptr;
    subOscUserSlot = nullptr;
    noiseUserSlot = nullptr;
    osc1PhaseScale = 1.0;
    osc2PhaseScale = 1.0;
    subOscPhaseScale = 1.0;
    noisePhaseScale = 1.0;

    if (userWaveBank == nullptr)
        return;

    // The sub reads the same base as the two oscillators: it offers the same four
    // shapes in the same order, so a slot sits at the same index in all three.
    // The GROUP still differs for all four -- each keeps its own eight slots, so
    // a sample imported for Oscillator 1 does not become the sub's waveform too.
    osc1UserSlot = userWaveBank->slotForChoice(osc1Waveform, UserWave::oscUserBase,
                                               UserWave::Group::Osc1);
    osc2UserSlot = userWaveBank->slotForChoice(osc2Waveform, UserWave::oscUserBase,
                                               UserWave::Group::Osc2);
    subOscUserSlot = userWaveBank->slotForChoice(subOscWaveform, UserWave::oscUserBase,
                                                 UserWave::Group::Sub);
    noiseUserSlot = userWaveBank->slotForChoice(noiseType, UserWave::noiseUserBase,
                                                UserWave::Group::Noise);

    if (osc1UserSlot != nullptr) osc1PhaseScale = osc1UserSlot->phaseIncrementScale;
    if (osc2UserSlot != nullptr) osc2PhaseScale = osc2UserSlot->phaseIncrementScale;

    // Whether either oscillator has two channels to play. Resolved once per block,
    // here where the slots are looked up, because the answer cannot change between
    // one sample and the next -- and the per-sample path branches on it to avoid
    // running a second decimation filter over a copy of the first.
    osc1Stereo = osc1UserSlot != nullptr && osc1UserSlot->isStereo();
    osc2Stereo = osc2UserSlot != nullptr && osc2UserSlot->isStereo();
    if (subOscUserSlot != nullptr) subOscPhaseScale = subOscUserSlot->phaseIncrementScale;
    if (noiseUserSlot != nullptr) noisePhaseScale = noiseUserSlot->phaseIncrementScale;
}

double SynthVoice::scatteredPhase (double base, float scatter) noexcept
{
    constexpr double twoPi = 2.0 * juce::MathConstants<double>::pi;

    if (scatter <= 0.0f)
        return base;

    double a = base + (double) scatter * random.nextDouble() * twoPi;

    while (a >= twoPi) a -= twoPi;
    while (a < 0.0)    a += twoPi;

    return a;
}

void SynthVoice::updateUnison (UnisonState& state, int voices, float detune, float width,
                               float phase) noexcept
{
    const int wanted = juce::jlimit (1, Unison::maxVoices, voices);

    // A change in the COUNT needs the new copies given phases; a change in detune
    // or width does not, and re-seeding on every block would restart the copies
    // sixty times a second and kill the beating that is the whole effect.
    const int had = state.voices;

    state.voices = wanted;
    state.phaseScatter = juce::jlimit (0.0f, 1.0f, phase);
    state.compensation = Unison::layout (wanted, (double) detune, (double) width, state.copies,
                                         (double) state.phaseScatter);

    // Only the copies that were not there before get a phase. Re-seeding all of
    // them would snap the running ones back together every time Voices moved,
    // which restarts the beating mid-note and is heard as a jump.
    for (int i = had; i < wanted; ++i)
        state.angle[i] = scatteredPhase (state.angle[0], state.phaseScatter);
}

void SynthVoice::seedUnisonPhases (UnisonState& state, double startAngle) noexcept
{
    constexpr double twoPi = 2.0 * juce::MathConstants<double>::pi;

    double a = startAngle;
    while (a >= twoPi) a -= twoPi;
    while (a < 0.0)    a += twoPi;

    // Copy 0 stays exactly where the plain oscillator would be, whatever Phase
    // says. The played note stays anchored to its own phase, and Voices 1 is
    // untouched by this knob rather than being a special case to remember.
    state.angle[0] = a;

    const int n = state.voices;

    if (n <= 1)
        return;

    if (state.phaseScatter <= 0.0f)
    {
        // Phase at zero: every copy on top of copy 0. What the unison did before
        // this knob existed, and what every preset already written gets.
        for (int i = 1; i < n; ++i)
            state.angle[i] = a;

        return;
    }

    // What the copies OUGHT to sum to at the first instant.
    //
    // N when they are stacked, sqrt(N) when they are scattered -- and the knob
    // asks for somewhere between the two, so the target runs between the two with
    // it. This is the same journey the compensation in Unison::layout makes, read
    // the same way, which is what keeps the attack and the steady state agreeing
    // at every setting rather than only at the ends.
    const double target = std::pow ((double) n, 1.0 - 0.5 * (double) state.phaseScatter);

    double best[Unison::maxVoices] {};
    double bestError = -1.0;

    for (int attempt = 0; attempt < phaseDrawAttempts; ++attempt)
    {
        double candidate[Unison::maxVoices] {};

        // The resultant of the whole set, as phasors. Its length IS what the
        // copies sum to while they are still together, so it can be checked
        // before a sample is generated.
        candidate[0] = a;
        double re = std::cos (a);
        double im = std::sin (a);

        for (int i = 1; i < n; ++i)
        {
            candidate[i] = scatteredPhase (a, state.phaseScatter);
            re += std::cos (candidate[i]);
            im += std::sin (candidate[i]);
        }

        const double error = std::abs (std::sqrt (re * re + im * im) - target);

        if (bestError < 0.0 || error < bestError)
        {
            bestError = error;

            for (int i = 0; i < n; ++i)
                best[i] = candidate[i];
        }
    }

    // Note what this is NOT: an even spread around the turn. That is what stood
    // here first, to stop the note starting with the copies stacked, and it is
    // exactly the arrangement that cancels -- N copies evenly spaced around one
    // cycle sum to zero, so Voices up with Detune down was SILENT and stayed more
    // than 10 dB down until the detune had pulled them clear (measured,
    // tools/unisonaudit). Every candidate above is drawn at random, so that
    // arrangement is never reached for; picking between them only declines the
    // unluckiest draws, it does not steer towards any fixed set.
    for (int i = 0; i < n; ++i)
        state.angle[i] = best[i];
}

float SynthVoice::renderUnison (UnisonState& state, int waveform, const UserWaveSlot* slot,
                                double freqHz, double baseDelta,
                                const PhaseShaper::Amounts& shaping,
                                bool slotIsStereo, float& rightOut) noexcept
{
    constexpr double twoPi = 2.0 * juce::MathConstants<double>::pi;

    float left = 0.0f;
    float right = 0.0f;

    for (int i = 0; i < state.voices; ++i)
    {
        const auto& copy = state.copies[i];

        // Each copy is the SAME oscillator read at its own phase. No one-shot
        // state is passed: a one-shot belongs to the note, and seven copies each
        // deciding the note had finished would silence it as the first one wrapped.
        float channelRight = 0.0f;
        const float value = generateWaveform (state.angle[i], waveform, slot, freqHz, nullptr,
                                              shaping, slotIsStereo ? &channelRight : nullptr);

        if (! slotIsStereo)
            channelRight = value;

        left += value * copy.gainLeft;
        right += channelRight * copy.gainRight;

        state.angle[i] += baseDelta * copy.ratio;

        if (state.angle[i] >= twoPi) state.angle[i] -= twoPi;
        if (state.angle[i] < 0.0)    state.angle[i] += twoPi;
    }

    rightOut = right * state.compensation;
    return left * state.compensation;
}

float SynthVoice::nextPinkSample (std::array<float, 16>& state, float& sum,
                                  std::uint32_t& counter) noexcept
{
    // Voss-McCartney update (16 rows). The row index is the index of the lowest
    // set bit of a running counter. With only state[0..15], that index must stay
    // at 15 or below. The old int counter could reach 65536 -> bitPos 16 ->
    // out-of-bounds writes and intermittent digital garbage (often bright and
    // harsh) after about 1.3 s at 48 kHz per voice.
    //
    // Lifted out of renderNextBlock so each unison copy can be handed its own
    // rows. The arithmetic is unchanged, so one voice is what it always was.
    counter = (counter + 1u) & 0xFFFFu;
    std::uint32_t p = counter;
    if (p == 0u)
        p = 1u;

    const std::uint32_t lowestChangedBitU = p & static_cast<std::uint32_t>(-static_cast<std::int32_t>(p));
    int bitPos = 0;
    for (std::uint32_t t = lowestChangedBitU; (t >>= 1u) != 0u;)
        ++bitPos;
    bitPos = juce::jmin(bitPos, static_cast<int>(state.size()) - 1);

    const float newVal = (random.nextFloat() * 2.0f - 1.0f) * 0.0625f;
    sum -= state[bitPos];
    sum += newVal;
    state[bitPos] = newVal;

    return sum * 3.8f;   // scaling to roughly match white noise RMS level
}

void SynthVoice::advanceShapingSmoothing() noexcept
{
    const double c = shapingSmoothingCoeff;

    const auto ease = [c] (PhaseShaper::Amounts& now, const PhaseShaper::Amounts& target)
    {
        now.bendPlus      += (target.bendPlus      - now.bendPlus)      * c;
        now.bendMinus     += (target.bendMinus     - now.bendMinus)     * c;
        now.bendPlusMinus += (target.bendPlusMinus - now.bendPlusMinus) * c;
        now.spectrum      += (target.spectrum      - now.spectrum)      * c;
        now.sync          += (target.sync          - now.sync)          * c;
    };

    ease(osc1Shaping, osc1ShapingTarget);
    ease(osc2Shaping, osc2ShapingTarget);
    ease(subOscShaping, subOscShapingTarget);
}

float SynthVoice::generateWaveform(double angle, int waveform, const UserWaveSlot* userSlot,
                                   double freqHz, OneShotState* oneShot,
                                   const PhaseShaper::Amounts& shapingAmounts,
                                   float* rightOut)
{
    // Whether any of Bend, Spectrum or Sync would change anything. A patch that
    // uses none of them takes exactly the path it took before they existed.
    const bool shaping = PhaseShaper::isActive(shapingAmounts);

    // An imported waveform replaces the shape entirely. The angle still means the
    // same thing, so everything that drives the angle -- tuning, glide, LFO pitch
    // modulation, the oversampler -- is unaffected.
    if (userSlot != nullptr)
    {
        constexpr double oneOverTwoPi = 1.0 / (2.0 * juce::MathConstants<double>::pi);
        const double phase = angle * oneOverTwoPi;

        // A sample with its loop turned off is heard once and then no more.
        //
        // Counted here, where the phase is READ, rather than where it is advanced:
        // it is advanced in two different places -- once normally and once per
        // sub-step while the oscillator is oversampling -- and both of them wrap
        // it, so this is the one point that sees every turn of it exactly once.
        //
        // The phase only ever runs forwards (an angle delta is a frequency, and a
        // frequency is positive), so coming back smaller means it has come round.
        if (oneShot != nullptr && ! userSlot->loop
            && userSlot->mode == UserWave::Mode::FullSample)
        {
            if (phase < oneShot->previousPhase)
                oneShot->finished = true;

            oneShot->previousPhase = phase;

            if (oneShot->finished)
                return 0.0f;
        }

        // The right channel of a stereo slot, for a caller that asked for one.
        // Filled on every path below, so no branch can forget it.
        if (! shaping)
        {
            if (rightOut != nullptr)
            {
                float l = 0.0f, r = 0.0f;
                userSlot->readStereo (phase, freqHz, sampleRate, l, r);
                *rightOut = r;
                return l;
            }

            return userSlot->read (phase, freqHz, sampleRate);
        }

        // Sync is withheld from a Full Sample slot, and only from that.
        //
        // Sync works by fitting several turns of the cycle into one turn of the
        // note. On a single cycle that is the hard-sync tear and exactly what is
        // wanted. On a whole recorded sample it would restart the recording eight
        // times a note, which is a stutter rather than a timbre -- and it would
        // fight the one-shot counter above, which exists to let such a sample play
        // through once and stop.
        //
        // The bend is given to both. It only ever moves a position within one
        // turn, and it never runs backwards, so a sample bent is a sample played
        // at an uneven speed -- which is a sound, not a fault.
        PhaseShaper::Amounts forSlot = shapingAmounts;

        if (userSlot->mode == UserWave::Mode::FullSample)
            forSlot.sync = 0.0;

        const double shapedPhase = PhaseShaper::shapedPhase (phase, forSlot);

        float sampled = 0.0f;
        float sampledRight = 0.0f;

        if (rightOut != nullptr)
            userSlot->readStereo (shapedPhase, freqHz, sampleRate, sampled, sampledRight);
        else
            sampled = userSlot->read (shapedPhase, freqHz, sampleRate);

        if (forSlot.spectrum <= 0.0)
        {
            if (rightOut != nullptr)
                *rightOut = sampledRight;

            return sampled;
        }

        // Spectrum on an imported waveform means the same as it does on a built-in
        // one: fade towards the fundamental, which is a sine at the same phase.
        const double amount = forSlot.spectrum > 1.0 ? 1.0 : forSlot.spectrum;
        const float sine = (float) std::sin (OscShape::twoPi * shapedPhase);

        // The same sine into both sides, and the same weight. Fading each channel
        // towards its own copy of the fundamental is what keeps the image where it
        // was: at full Spectrum a stereo slot collapses to one centred sine, which
        // is correct -- one fundamental is one signal, and it has no width.
        if (rightOut != nullptr)
            *rightOut = (float) (sampledRight * (1.0 - amount) + sine * amount);

        return (float) (sampled * (1.0 - amount) + sine * amount);
    }

    // Every built-in shape, from the one place the maths is written down. The
    // Waveforms picture calls the same function, so what is drawn in the list and
    // what comes out of the oscillator cannot drift apart -- which mattered
    // little with four shapes and would be a certainty with twenty-one.
    //
    // The four original shapes are unchanged in there, to the sample. See
    // OscillatorShapes.h, and the shape test that compares them against the maths
    // this switch used to hold.
    // A built-in shape is one signal. Both sides get it, so an oscillator on a
    // built-in behaves exactly as it always did and its pan does all the placing.
    const float builtIn = shaping
        ? PhaseShaper::shapedValue(waveform, angle / OscShape::twoPi, shapingAmounts)
        : OscShape::shapeValueFromAngle(waveform, angle);

    if (rightOut != nullptr)
        *rightOut = builtIn;

    return builtIn;
}

//==============================================================================
// -- Oscillator Frequency Updates --

//==============================================================================
// -- Oscillator Pitch Tuning --
// Each oscillator has independent coarse tuning (Â±24 semitones) and fine detuning (Â±50 cents)
// Final pitch calculation: midiNote + coarseTune + (detune / 100) [all in semitones]
// Convert cents to semitones by dividing by 100

/**
    Update Osc1 frequency with coarse tune and detune.
    Final pitch = midiNote + osc1CoarseTune + (osc1Detune / 100) [all in semitones]
*/
void SynthVoice::updateOsc1Frequency(double baseFrequency)
{
    // Calculate total semitones: coarse tune + detune (convert cents to semitones)
    double totalSemitones = osc1CoarseTune + (osc1Detune / 100.0);
    double tunedFrequency = baseFrequency * std::pow(2.0, totalSemitones / 12.0);
    
    // CRITICAL: Use stored sampleRate member, not getSampleRate()
    // The stored sampleRate is set in prepareToPlay() and is guaranteed to be valid
    // Using getSampleRate() can return 0 if called before sample rate is set
    if (sampleRate > 0.0)
    {
        auto cyclesPerSample = tunedFrequency / sampleRate;
        // The scale is 1 for every built-in shape, and only differs for a Full
        // Sample slot, where one turn of the phase covers the file rather than a
        // period. See UserWaveSlot::phaseIncrementScale.
        osc1AngleDelta = cyclesPerSample * 2.0 * juce::MathConstants<double>::pi * osc1PhaseScale;
    }
    else
    {
        // Sample rate not set yet - angle delta will be 0 (no sound)
        osc1AngleDelta = 0.0;
    }
}

/**
    Update Osc2 frequency with coarse tune and detune.
    Final pitch = midiNote + osc2CoarseTune + (osc2Detune / 100) [all in semitones]
*/
void SynthVoice::updateOsc2Frequency(double baseFrequency)
{
    // Calculate total semitones: coarse tune + detune (convert cents to semitones)
    double totalSemitones = osc2CoarseTune + (osc2Detune / 100.0);
    double tunedFrequency = baseFrequency * std::pow(2.0, totalSemitones / 12.0);
    
    // CRITICAL: Use stored sampleRate member, not getSampleRate()
    // The stored sampleRate is set in prepareToPlay() and is guaranteed to be valid
    // Using getSampleRate() can return 0 if called before sample rate is set
    if (sampleRate > 0.0)
    {
        auto cyclesPerSample = tunedFrequency / sampleRate;
        osc2AngleDelta = cyclesPerSample * 2.0 * juce::MathConstants<double>::pi * osc2PhaseScale;
    }
    else
    {
        // Sample rate not set yet - angle delta will be 0 (no sound)
        osc2AngleDelta = 0.0;
    }
}

//==============================================================================
// -- Filter Updates --

void SynthVoice::updateFilter()
{
    // Clamp cutoff to valid range
    float clampedCutoff = juce::jlimit(20.0f, 20000.0f, filterCutoff);

    // Resonance is passed normalized (0.0-1.0); NonlinearSVF owns the Q curve and
    // the self-oscillation region at the top of the knob.
    //
    // velocityResonanceScale is 1.0 unless the patch asked for velocity, and it is
    // per VOICE: the knob is shared by every note, how hard THIS note was played
    // is not.
    filter.setMode(juce::jlimit(0, NonlinearSVF::numModes - 1, filterMode));
    filter.setCutoffFrequency(clampedCutoff);
    filter.setResonanceNormalized(juce::jlimit(0.0f, 1.0f, filterResonance * velocityResonanceScale));
}

void SynthVoice::updateNoiseEqFilters()
{
    if (sampleRate <= 0.0)
        return;
    
    // Low shelf: affects frequencies below 200 Hz
    // When amount > 0: strong boost (low shelf, +24 dB max)
    // When amount < 0: steep cut to near silence (high-pass with very steep Q)
    const float lowShelfFreq = 200.0f;
    
    if (lowShelfAmount < 0.0f)
    {
        // Negative: use high-pass filter with very steep Q for dramatic cut
        // Q of 2.0 creates a much steeper slope, cutting lows to near silence
        *lowShelfFilter.coefficients = *juce::dsp::IIR::Coefficients<float>::makeHighPass(
            sampleRate, lowShelfFreq, 2.0f);
    }
    else if (lowShelfAmount > 0.0f)
    {
        // Positive: use low shelf for strong boost (+24 dB max)
        float lowGainDb = lowShelfAmount * 24.0f; // Â±24 dB range for dramatic effect
        *lowShelfFilter.coefficients = *juce::dsp::IIR::Coefficients<float>::makeLowShelf(
            sampleRate, lowShelfFreq, 0.707f, lowGainDb);
    }
    else
    {
        // Zero: bypass (all-pass)
        *lowShelfFilter.coefficients = *juce::dsp::IIR::Coefficients<float>::makeAllPass(sampleRate, 1.0f);
    }
    
    // High shelf: affects frequencies above 1.5 kHz
    // When amount > 0: strong boost (high shelf, +24 dB max)
    // When amount < 0: steep cut to near silence (low-pass with very steep Q)
    const float highShelfFreq = 1500.0f;
    
    if (highShelfAmount < 0.0f)
    {
        // Negative: use low-pass filter with very steep Q for dramatic cut
        // Q of 2.0 creates a much steeper slope, cutting highs to near silence
        *highShelfFilter.coefficients = *juce::dsp::IIR::Coefficients<float>::makeLowPass(
            sampleRate, highShelfFreq, 2.0f);
    }
    else if (highShelfAmount > 0.0f)
    {
        // Positive: use high shelf for strong boost (+24 dB max)
        float highGainDb = highShelfAmount * 24.0f; // Â±24 dB range for dramatic effect
        *highShelfFilter.coefficients = *juce::dsp::IIR::Coefficients<float>::makeHighShelf(
            sampleRate, highShelfFreq, 0.707f, highGainDb);
    }
    else
    {
        // Zero: bypass (all-pass)
        *highShelfFilter.coefficients = *juce::dsp::IIR::Coefficients<float>::makeAllPass(sampleRate, 1.0f);
    }

    // The right-hand pair points at the SAME coefficient objects, so it can never
    // drift from the left. The pointer is assigned, not the numbers: every branch
    // above writes THROUGH the left filter's pointer, so the right sees each
    // change without this function having to remember to copy it.
    lowShelfFilterR.coefficients = lowShelfFilter.coefficients;
    highShelfFilterR.coefficients = highShelfFilter.coefficients;
}

//==============================================================================
// -- ADSR Envelope Logic --

/**
    Update ADSR parameters from stored timing values.
    
    This method must be called whenever envelope parameters change to ensure
    JUCE's ADSR uses the correct values. JUCE's ADSR requires all parameters
    to be set together via setParameters() - individual parameter changes
    won't take effect until this is called.
    
    CRITICAL: This must be called:
    - In the voice constructor (initial setup)
    - When any envelope parameter changes (via setEnvAttack, setEnvDecay, etc.)
    - When sample rate changes (to recalculate sample-based timing)
    
    Real-time Safety: This method is safe to call from the audio thread as it
    only updates the ADSR's internal parameters (no allocations, lock-free).
    
    Parameter Ranges:
    - Attack, Decay, Release: 0.01s to 20.0s (skewed, midpoint at 2.0s)
    - Sustain: 0.0 to 1.0 (linear, represents amplitude level)
    
    Common Pitfalls:
    - Sustain must be 0.0-1.0 (represents amplitude, not time)
    - All times are in seconds (JUCE converts to samples internally)
    - Parameters are set atomically - all four must be set together
*/
void SynthVoice::updateAdsrParameters()
{
    //==============================================================================
    // -- Safety Check: Sample Rate Must Be Valid --
    // CRITICAL: ADSR parameters cannot be set until sample rate is known.
    // This prevents assertions when voices are created before prepareToPlay().
    // 
    // If sample rate is invalid (0 or negative), skip parameter update.
    // The parameters will be set correctly once setCurrentPlaybackSampleRate() is called.
    if (sampleRate <= 0.0)
    {
        // Sample rate not yet set - parameters will be applied when prepareToPlay() is called
        return;
    }
    
    // Clamp to legal ranges. Times must be > 0.01s to prevent JUCE assertions.
    const float attack  = juce::jmax(0.01f, envAttackTime);       // 0.01-20.0s, skewed
    const float decay   = juce::jmax(0.01f, envDecayTime);        // 0.01-20.0s, skewed
    const float sustain = juce::jlimit(0.0f, 1.0f, envSustainLevel); // 0.0-1.0, linear amplitude
    const float release = juce::jmax(0.01f, envReleaseTime);      // 0.01-20.0s, skewed - long cosmic tails!

    // Only push when something actually changed. We are called every processBlock,
    // and setParameters() -> recalculateRates() rewrites releaseRate from sustain,
    // which silences an in-progress release tail (esp. with low sustain). Skipping
    // no-op pushes lets the release run from the level captured at note-off.
    if (attack == lastAdsrAttack && decay == lastAdsrDecay
        && sustain == lastAdsrSustain && release == lastAdsrRelease)
        return;

    // Mid-release handling. We can't push a full setParameters() here: juce::ADSR's
    // (and our faithful copy's) recalculateRates() rewrites releaseRate = sustain/
    // (release*sr); when sustain == 0 (plucks/bass) that rate is 0 and the envelope
    // is forced straight to idle â€” instantly cutting the tail. Instead retarget the
    // live release click-free: setReleaseRetainingLevel() re-derives the slope from
    // the CURRENT level (level/(release*sr)) the way noteOff() does, so shortening or
    // lengthening Release re-shapes the ringing note itself (and lets you audition the
    // tail while dragging), not just future notes. Attack/Decay/Sustain don't affect
    // an in-progress release, so we defer them â€” leaving their lastAdsr* sentinels
    // unchanged so they're applied as soon as the voice leaves release (next note-on).
    if (inReleasePhase)
    {
        if (release != lastAdsrRelease)
        {
            adsr.setReleaseRetainingLevel(release);
            lastAdsrRelease = release;
        }
        return;
    }

    lastAdsrAttack = attack;
    lastAdsrDecay = decay;
    lastAdsrSustain = sustain;
    lastAdsrRelease = release;

    juce::ADSR::Parameters params;
    params.attack = attack;
    params.decay = decay;
    params.sustain = sustain;
    params.release = release;

    // Apply parameters to ADSR (real-time safe, no allocations)
    adsr.setParameters(params);
}

void SynthVoice::updateFilterAdsrParameters()
{
    // Safety check: Sample rate must be valid
    if (sampleRate <= 0.0)
    {
        return;
    }
    
    const float attack  = juce::jmax(0.01f, filterEnvAttackTime);
    const float decay   = juce::jmax(0.01f, filterEnvDecayTime);
    const float sustain = juce::jlimit(0.0f, 1.0f, filterEnvSustainLevel);
    const float release = juce::jmax(0.01f, filterEnvReleaseTime);

    // Same guard as the amp envelope: only re-push on a real change so a running
    // filter-envelope release isn't reset to idle by recalculateRates() each block.
    if (attack == lastFilterAdsrAttack && decay == lastFilterAdsrDecay
        && sustain == lastFilterAdsrSustain && release == lastFilterAdsrRelease)
        return;

    // Same mid-release handling as the amp envelope (see updateAdsrParameters):
    // retarget the live filter-envelope release from its current level instead of
    // pushing setParameters() (which, at sustain == 0, would let recalculateRates()
    // kill the tail and jump the cutoff to its resting value â€” a click/zip). The amp
    // + filter envelopes enter release together, so inReleasePhase gates both.
    if (inReleasePhase)
    {
        if (release != lastFilterAdsrRelease)
        {
            filterAdsr.setReleaseRetainingLevel(release);
            lastFilterAdsrRelease = release;
        }
        return;
    }

    lastFilterAdsrAttack = attack;
    lastFilterAdsrDecay = decay;
    lastFilterAdsrSustain = sustain;
    lastFilterAdsrRelease = release;

    juce::ADSR::Parameters params;
    params.attack = attack;
    params.decay = decay;
    params.sustain = sustain;
    params.release = release;

    // Apply parameters to Filter Envelope ADSR (real-time safe, no allocations)
    filterAdsr.setParameters(params);
}

//==============================================================================
// -- Audio Rendering --

void SynthVoice::renderNextBlock(juce::AudioBuffer<float>& outputBuffer,
                                 int startSample, int numSamples)
{
    // RT-safe bounds guard: warn if the host ever hands us a slice that
    // extends past the buffer (would manifest as an out-of-bounds write).
    SAFETY_CHECK_BOUNDS(outputBuffer.getReadPointer(0),
                        startSample + numSamples,
                        outputBuffer.getNumSamples() + 1,
                        "SynthVoice::renderNextBlock slice past buffer end");

    //==============================================================================
    // -- CRITICAL: Complete Signal Chain --
    // 
    // Signal Flow:
    // 1. Generate Osc1 waveform (Sine/Triangle/Saw/Square)
    // 2. Generate Osc2 waveform (Sine/Triangle/Saw/Square)
    // 3. Generate Noise (white noise)
    // 4. Apply independent levels (osc1Level, osc2Level, noiseLevel)
    // 5. Additive mixing: sum all sources
    // 6. Apply ADSR envelope (Attack â†’ Decay â†’ Sustain â†’ Release)
    // 7. Process through multimode filter (LowPass/BandPass/HighPass/Notch/Peak)
    // 8. Write to output buffer (stereo)
    //
    // Real-time Safety: All operations are allocation-free and lock-free.
    
    // Early return if no note is playing (angle deltas are 0 when no note is active)
    if (osc1AngleDelta == 0.0 && osc2AngleDelta == 0.0)
        return;
    
    // Denormal prevention: FTZ/DAZ for oscillators, filters, envelopes (per JUCE best practice)
    juce::ScopedNoDenormals noDenormals;

    // Resolve the imported waveforms once for the whole block. The bank only
    // changes when the user imports something, so doing this per sample would be
    // three pointer chases per oscillator per voice to reach the same answer.
    refreshUserWaveSelection();

    // SAFETY: a host may render a block LARGER than it declared to prepareToPlay
    // (Ableton does this during freeze/bounce/render). The per-voice scratch buffer
    // is sized to the prepared max; the per-sample writes and clear() below address
    // up to `numSamples`, so grow it if this block exceeds its capacity â€” otherwise
    // they overrun the buffer and corrupt the heap (ASan-confirmed). Grows only on
    // the first oversized block, then stays grown.
    if (voiceTempBuffer.getNumSamples() < numSamples)
        voiceTempBuffer.setSize(2, numSamples, false, false, true);

    //==============================================================================
    // -- DEBUG LOGGING: Voice Rendering (completely removed for production) --
    // Verbose logging removed entirely to prevent CPU spam and crashes.
    // Only startNote/stopNote logs remain for confirmation (see startNote/stopNote methods).
    // This ensures clean, non-crackly cosmic sound with minimal CPU usage.
    // Excessive logging was causing CPU overload and contributing to Ableton crashes.
    
    // Use pre-allocated buffer (no allocation in audio thread - sized in prepareToPlay)
    const int maxSamples = juce::jmin(numSamples, voiceTempBuffer.getNumSamples());
    if (maxSamples <= 0)
        return;
    voiceTempBuffer.clear();
    
    // Track how many samples we actually process (in case ADSR finishes mid-block)
    int samplesProcessed = 0;

    // -- Keyboard tracking --
    // The cutoff shift is computed once per block for a STATIC (non-gliding) note â€”
    // bit-identical to before â€” and re-derived per sample from the live gliding pitch
    // while a portamento is in progress (see the per-sample block below) so the filter
    // tracks the glide instead of snapping to the destination note. keyTrackLogOffset
    // is added in log-frequency space, and only when the filter's key-track flag is
    // on. Default 0 = no change (full backward compatibility).
    const int keyTrackNote = currentlyPlayingNote.isValid()
                                 ? static_cast<int>(currentlyPlayingNote.initialNote)
                                 : kFilterKeyTrackRefNote;
    // STATIC key-track (used when the note is NOT gliding): bit-identical to before.
    const float keyTrackLogOffsetStatic = static_cast<float>(keyTrackNote - kFilterKeyTrackRefNote)
                                        * kFilterKeyTrackLogPerSemi;
    const float keyTrackMultiplierStatic = std::exp(keyTrackLogOffsetStatic);
    // GLIDE-AWARE key-track: during a portamento the cutoff must follow the live
    // gliding pitch (currentPitch) instead of snapping to the destination note â€” the
    // frozen-at-target tracking made note transitions sound disconnected. We derive
    // the shift from currentPitch / refNoteHz per sample (below). When not gliding,
    // currentPitch == the note's Hz so the static values above are used verbatim (no
    // behaviour change for held/non-glide notes). keyTrackRefHz = Hz of note 60.
    const double keyTrackRefHz = juce::MidiMessage::getMidiNoteInHertz(kFilterKeyTrackRefNote);
    const bool anyKeyTrackOn = filterKeyTrack;

    //==============================================================================
    // -- Per-block constants (hoisted out of the per-sample loop) --
    // These depend only on values that are fixed for the whole render segment:
    // oscillator tuning, pan, and MPE pitch bend (the MPESynthesiser splits the
    // block at MIDI / expression events, so mpeBendSemitones is constant within a
    // single renderNextBlock call). Computing them once instead of per sample
    // removes ~10 std::pow / std::sin / std::cos / std::log calls per sample per
    // voice with bit-identical output (same operands, same operations).
    const double osc1TuneRatio = std::pow(2.0, (osc1CoarseTune + osc1Detune / 100.0) / 12.0);
    const double osc2TuneRatio = std::pow(2.0, (osc2CoarseTune + osc2Detune / 100.0) / 12.0);
    const double subCoarseRatio = std::pow(2.0, static_cast<double>(subOscCoarse) / 12.0);

    // Pitch bend (MPE wheel/per-note bend + manual UI slider), constant per segment.
    const float  manualBendStBlock = juce::jlimit(-1.0f, 1.0f, pitchBend) * pitchBendAmountFloat;
    const double totalBendStBlock  = mpeBendSemitones + static_cast<double>(manualBendStBlock);
    const double blockBendRatio     = std::pow(2.0, totalBendStBlock / 12.0);

    // Constant-power pan gains (-1 = full left, 0 = center, 1 = full right).
    const float pi4 = static_cast<float>(juce::MathConstants<double>::pi / 4.0);
    const float gainL1 = std::cos((osc1Pan + 1.0f) * pi4);
    const float gainR1 = std::sin((osc1Pan + 1.0f) * pi4);
    const float gainL2 = std::cos((osc2Pan + 1.0f) * pi4);
    const float gainR2 = std::sin((osc2Pan + 1.0f) * pi4);
    const float centerGain = 0.70710678f;  // 1/sqrt(2) for centered sources

    // Log-frequency constants for the filter cutoff math.
    const float logMin = std::log(20.0f);
    const float logMax = std::log(20000.0f);
    const float timbreLogScale = static_cast<float>(std::log(4.0)); // Â±2 octaves of timbre sweep

    // Every LFO's block of values, resolved ONCE here rather than through
    // lfoBufferFor() on every sample. The read pointer and the length are taken
    // together so the per-sample test is an int compare, exactly the guard the
    // old two-LFO code did inline.
    const float* lfoRead[spacedust::numLfos] = { };
    int          lfoLen [spacedust::numLfos] = { };

    if (processor != nullptr)
    {
        for (int lfo = 0; lfo < spacedust::numLfos; ++lfo)
        {
            const auto& b = processor->lfoBufferFor(lfo);

            if (b.getNumSamples() > 0)
            {
                lfoRead[lfo] = b.getReadPointer(0);
                lfoLen [lfo] = b.getNumSamples();
            }
        }
    }

    // Generate oscillator waveforms, mix, and apply envelope
    for (int i = 0; i < maxSamples; ++i)
    {
        // Glide (portamento): applied at END of each sample iteration so this sample's
        // pitch matches startNote/currentPitch; advancing here first made the first output
        // sample one glide step sharp/flat vs the requested transition (audible in mono/legato).

        //==============================================================================
        // -- PITCH CURVE (separate from pitch bend) --
        // A shape drawn by hand, in place of the old three-knob straight fall.
        // Time is in seconds (0-10 from the Time parameter); the curve's own 0..1
        // axis maps across that duration.
        double pitchForOscillators = currentPitch;
        bool   pitchEnvShapingNow  = false;  // true while the curve is actively bending the pitch

        // A flat curve is skipped entirely, so a patch that draws nothing costs
        // nothing and sounds exactly as it did. Note isFlat() is a STATIC,
        // whole-object property (was any point ever nonzero) -- it only decides
        // whether it is worth evaluating the curve at all, never whether to pin
        // the pitch right now. That decision is pitchEnvShapingNow below, which
        // is a per-instant reading of valueAt() and is what must gate the pin:
        // gating on isFlat() instead pinned pitchForOscillators to targetPitch
        // for the ENTIRE remaining life of any note with a non-flat curve, long
        // after valueAt() had settled to zero -- silently defeating glide,
        // portamento, and the mono anti-click auto-glide for that whole note.
        if (pitchCurve != nullptr && ! pitchCurve->isFlat()
            && pitchCurveTime >= 0.0001f && sampleRate > 0.0f)
        {
            const float elapsedSec = pitchEnvSamplesElapsed / static_cast<float>(sampleRate);
            const float t01 = elapsedSec / pitchCurveTime;

            const float semitones = juce::jlimit(-48.0f, 48.0f, pitchCurve->valueAt(t01));
            pitchEnvShapingNow = (semitones != 0.0f);

            if (pitchEnvShapingNow)
            {
                const double ratio = std::pow(2.0, static_cast<double>(semitones) / 12.0);

                // Anchored to the intended note (targetPitch), not the
                // glide-tracking currentPitch, for the same reason the old ramp
                // was: rapid notes whose glide has not finished would otherwise
                // bend a meaningless mid-glide pitch.
                pitchForOscillators = targetPitch * ratio;
            }
            // else: the curve is holding at exactly zero right now (settled, or
            // simply passing through zero) -- pitchForOscillators keeps its
            // currentPitch default above, handing control back to glide for as
            // long as the curve stays at zero. If it later rises again,
            // pitchEnvShapingNow goes true and the pin above resumes.
        }
        // Cap pitchEnvSamplesElapsed to avoid float precision loss on very long holds
        if (pitchEnvSamplesElapsed < 1e7f)
            pitchEnvSamplesElapsed += 1.0f;

        //==============================================================================
        // -- PITCH BEND (MPE-aware, separate from pitch envelope) --
        //
        // Total pitch bend = MPE bend (master wheel + per-note bend, in semitones)
        //                  + manual UI slider (pitchBend in -1..+1, scaled by
        //                    pitchBendAmountFloat semitones).
        //
        // mpeBendSemitones is populated by notePitchbendChanged() AND by noteStarted()
        // â€” it's already in semitones, already correctly weighted by the active
        // zone's per-note bend range (or the legacy-mode pitchbend range).  For a
        // Seaboard sending per-note pitch CC on its own channel this captures the
        // smooth glissando perfectly; for a regular keyboard sending master pitch
        // bend on channel 1, the legacy-mode bend range applies (48 semitones by
        // default â€” see SpaceDustSynthesiser).
        //
        // The manual UI bend slider is still useful for users who want a software
        // pitch bend independent of any hardware wheel.
        // Pitch bend ratio is constant per render segment (computed once above).
        const double bendRatio = blockBendRatio;
        double osc1Freq = pitchForOscillators * bendRatio;
        double osc2Freq = pitchForOscillators * bendRatio;
        
        // Apply LFO modulation per-sample. WHICH LFO reaches what is now the
        // modulation matrix's answer -- setLfoModAmounts() copied this block's
        // amounts in once, on the audio thread, from a list the message thread
        // compiled -- but the SHAPES below are the ones the deleted Destination
        // drop-down used, so a patch migrated to a routing of +1.0 moves exactly
        // as it always did.
        float filterMod = 0.0f;        // master cutoff is scaled by (1 + filterMod * 0.5)
        float osc1PitchMod = 0.0f;     // in LFO units; the frequency ratio is 2^mod
        float osc2PitchMod = 0.0f;
        float osc1VolMod = 0.0f, osc2VolMod = 0.0f, noiseVolMod = 0.0f;  // gains scale by (1 + mod)

        if (anyLfoModAmount)
        {
            for (int lfo = 0; lfo < spacedust::numLfos; ++lfo)
            {
                if (i >= lfoLen[lfo])
                    continue;

                const float v = lfoRead[lfo][i];

                osc1PitchMod += lfoModAmount[spacedust::psm_osc1Pitch]   [lfo] * v;
                osc2PitchMod += lfoModAmount[spacedust::psm_osc2Pitch]   [lfo] * v;
                filterMod    += lfoModAmount[spacedust::psm_filterCutoff][lfo] * v;
                osc1VolMod   += lfoModAmount[spacedust::psm_osc1Level]   [lfo] * v;
                osc2VolMod   += lfoModAmount[spacedust::psm_osc2Level]   [lfo] * v;
                noiseVolMod  += lfoModAmount[spacedust::psm_noiseLevel]  [lfo] * v;
            }

            // Pitch. The drop-down's Pitch entry moved BOTH oscillators by
            // 2^(LFO value) -- 1200 cents at full swing. Each oscillator now has
            // its own Coarse knob to be pointed at, so each carries its own
            // ratio; a patch that routes both at +1.0 is the old sound exactly.
            //
            // std::pow only when something actually reaches it: an unrouted
            // pitch costs a float compare, not a transcendental, per sample.
            if (osc1PitchMod != 0.0f)
                osc1Freq *= std::pow(2.0, static_cast<double>(osc1PitchMod));

            if (osc2PitchMod != 0.0f)
                osc2Freq *= std::pow(2.0, static_cast<double>(osc2PitchMod));
        }
        
        // Apply oscillator tuning (constant per block â€” ratios cached above).
        osc1Freq = osc1Freq * osc1TuneRatio;
        osc2Freq = osc2Freq * osc2TuneRatio;

        // Analog Drift: emulates hardware component tolerance and slow oscillator/filter drift
        if (analogDriftAmount > 0.0f)
        {
            const float a = analogDriftAmount;
            // Two INDEPENDENT slow random walks so the oscillators wander apart and
            // back over a few seconds â€” the slow evolving beating of a real analog
            // pair (the old code shared one walk, so both moved in lockstep with no
            // beating). Static per-note offset Â±12 cents + Â±6 cents wander at max.
            analogOscWalk  += analogDriftWalkCoeff * ((random.nextFloat() * 2.0f - 1.0f) - analogOscWalk);
            analogOscWalk2 += analogDriftWalkCoeff * ((random.nextFloat() * 2.0f - 1.0f) - analogOscWalk2);
            const float cents1 = osc1DriftOffset * 12.0f * a + analogOscWalk  * 6.0f * a;
            const float cents2 = osc2DriftOffset * 12.0f * a + analogOscWalk2 * 6.0f * a;
            osc1Freq *= std::pow(2.0, static_cast<double>(cents1) / 1200.0);
            osc2Freq *= std::pow(2.0, static_cast<double>(cents2) / 1200.0);
        }
        
        // CRITICAL: Clamp frequencies to prevent runaway pitch on long holds/legato.
        // jlimit does not fix NaN/Inf â€” those would propagate into phase and blow up the output.
        osc1Freq = juce::jlimit(20.0, 20000.0, osc1Freq);
        osc2Freq = juce::jlimit(20.0, 20000.0, osc2Freq);
        if (!std::isfinite(osc1Freq) || !std::isfinite(osc2Freq))
        {
            reportDspSanitize(processor);
            // MPE: getCurrentlyPlayingNote() returns MPENote â€” use initialNote when valid.
            const int n = currentlyPlayingNote.isValid()
                             ? static_cast<int>(currentlyPlayingNote.initialNote)
                             : -1;
            const double safeHz = juce::jlimit(20.0, 20000.0,
                                               n >= 0 ? (double) juce::MidiMessage::getMidiNoteInHertz(n) : 440.0);
            osc1Freq = safeHz;
            osc2Freq = safeHz;
        }

#if SPACE_DUST_LOG_MONO_LEGATO_PITCH
        if (i == 0 && pitchTraceSeq != pitchTraceLastRenderLogSeq)
        {
            pitchTraceLastRenderLogSeq = pitchTraceSeq;
            debugLogPitchRenderSample0(osc1Freq, pitchForOscillators);
        }
#endif
        
        // Update oscillator angle deltas with modulated frequencies
        double noiseWaveFreq = osc1Freq;
        // The pitch the sub is actually running at. Kept out here so the generator
        // below can be told the truth about it -- an imported waveform picks how
        // much bandwidth to read from the frequency it is handed, and the sub's
        // own Coarse knob can put it two octaves away from half of Osc 1.
        double subOscFreq = osc1Freq * 0.5;
        if (sampleRate > 0.0)
        {
            osc1AngleDelta = (osc1Freq / sampleRate) * 2.0 * juce::MathConstants<double>::pi * osc1PhaseScale;
            osc2AngleDelta = (osc2Freq / sampleRate) * 2.0 * juce::MathConstants<double>::pi * osc2PhaseScale;
            double baseFreq = osc1Freq / osc1TuneRatio;
            double subFreq = baseFreq * 0.5 * subCoarseRatio;
            subFreq = juce::jlimit(20.0, 20000.0, subFreq);
            subOscFreq = subFreq;
            subOscAngleDelta = (subFreq / sampleRate) * 2.0 * juce::MathConstants<double>::pi * subOscPhaseScale;

            // An imported waveform in the noise slot has no tuning controls of its
            // own, so it runs at the played note before Osc 1's coarse and detune
            // are applied -- the same pitch the sub oscillator is derived from.
            noiseWaveFreq = baseFreq;
            noiseWaveAngleDelta = (baseFreq / sampleRate) * 2.0 * juce::MathConstants<double>::pi * noisePhaseScale;
        }
        
        // Bend, Spectrum and Sync one sample closer to where the knobs are.
        //
        // Once per output sample and not per oversampled sub-step: the glide is
        // measured in milliseconds and the sub-steps of one sample are the same
        // instant as far as it is concerned.
        advanceShapingSmoothing();

        // Step 1-2: Generate oscillator waveforms
        //
        // Two values each for the oscillators, one for the sub and the noise. On a
        // built-in shape or a mono import the two are the same number and every
        // sum below is what it always was; only a stereo import makes them differ.
        float osc1Sample, osc2Sample, subOscSample;
        float osc1Right = 0.0f, osc2Right = 0.0f, subOscRight = 0.0f;

        if (!oscOSActive)
        {
            // Base rate. Phases are advanced at the bottom of the loop, as always.
            // rightOut only where there is a second channel to fetch. On anything
            // mono the two sides are the same number, and asking for both makes
            // the slot do a second interpolation to produce a copy.
            if (osc1Unison.active())
            {
                osc1Sample = renderUnison(osc1Unison, osc1Waveform, osc1UserSlot, osc1Freq,
                                          osc1AngleDelta, osc1Shaping, osc1Stereo, osc1Right);
            }
            else
            {
                osc1Sample = generateWaveform(osc1Angle, osc1Waveform, osc1UserSlot, osc1Freq, &osc1OneShot, osc1Shaping,
                                              osc1Stereo ? &osc1Right : nullptr);

                if (! osc1Stereo) osc1Right = osc1Sample;
            }

            if (osc2Unison.active())
            {
                osc2Sample = renderUnison(osc2Unison, osc2Waveform, osc2UserSlot, osc2Freq,
                                          osc2AngleDelta, osc2Shaping, osc2Stereo, osc2Right);
            }
            else
            {
                osc2Sample = generateWaveform(osc2Angle, osc2Waveform, osc2UserSlot, osc2Freq, &osc2OneShot, osc2Shaping,
                                              osc2Stereo ? &osc2Right : nullptr);

                if (! osc2Stereo) osc2Right = osc2Sample;
            }
            if (! subOscOn)
            {
                subOscSample = 0.0f;
                subOscRight = 0.0f;
            }
            else if (subUnison.active())
            {
                subOscSample = renderUnison(subUnison, subOscWaveform, subOscUserSlot, subOscFreq,
                                            subOscAngleDelta, subOscShaping, false, subOscRight) * subOscLevel;
                subOscRight *= subOscLevel;
            }
            else
            {
                subOscSample = generateWaveform(subOscAngle, subOscWaveform, subOscUserSlot, subOscFreq,
                                                &subOscOneShot, subOscShaping) * subOscLevel;
                subOscRight = subOscSample;
            }
        }
        else
        {
            // 4x. Each sub-step advances its own phase by a quarter of the base-rate
            // delta and generates there; the stage's FIR decimates back to one sample.
            // The angle deltas already carry this sample's pitch modulation, and it is
            // held across the four sub-steps -- the LFO buffer only exists at base rate.
            //
            // The phases are fully advanced by one base-rate period here, so the usual
            // advance at the bottom of the loop is skipped (see oscOSActive there).
            const double d1 = osc1AngleDelta / kOscOSFactor;
            const double d2 = osc2AngleDelta / kOscOSFactor;
            const double ds = subOscAngleDelta / kOscOSFactor;
            const double twoPi = 2.0 * juce::MathConstants<double>::pi;

            auto wrap = [twoPi] (double& a)
            {
                if (a >= twoPi) a -= twoPi;
                if (a < 0.0)    a += twoPi;
            };

            // x is ignored: there is no input signal, only generation. The stage's
            // upsampler still runs on zeros, which costs a little and changes nothing.
            // The stereo path ONLY for an oscillator actually playing a stereo
            // slot. It runs the decimation filter a second time -- a thirty-three
            // tap dot product per sample -- and on a built-in shape or a mono
            // import both channels hold the same number, so that whole second
            // filter would compute a copy of the first.
            //
            // That is not a small waste: it is the common case, it is per voice,
            // and paying it everywhere pushed the audio thread over at high
            // polyphony, which sounded like notes refusing to play
            // (Giuseppe, 2026-08-26).
            // Unison sums its copies INSIDE the oversampler's loop, not around it.
            // The expensive part of the stage is the filter, and one pass filters
            // the sum of seven copies for the price of filtering one. Seven copies
            // each through their own pass would be seven times the filter work,
            // which is what makes unison affordable at all.
            if (osc1Unison.active())
            {
                oscOsc12OS.processStereo(0, 2, 0.0f, [&] (float, float& right) -> float
                {
                    const float v = renderUnison(osc1Unison, osc1Waveform, osc1UserSlot, osc1Freq,
                                                 d1, osc1Shaping, osc1Stereo, right);

                    // The voice's own angle still runs, so switching unison off
                    // mid-note picks up where the plain oscillator would be.
                    osc1Angle += d1; wrap(osc1Angle);
                    return v;
                }, osc1Sample, osc1Right);
            }
            else if (osc1Stereo)
            {
                // Both channels through ONE pass. The lambda advances the phase,
                // so a second pass would step the oscillator twice per sample and
                // play it an octave up.
                oscOsc12OS.processStereo(0, 2, 0.0f, [&] (float, float& right) -> float
                {
                    const float v = generateWaveform(osc1Angle, osc1Waveform, osc1UserSlot, osc1Freq, &osc1OneShot, osc1Shaping, &right);
                    osc1Angle += d1; wrap(osc1Angle);
                    return v;
                }, osc1Sample, osc1Right);
            }
            else
            {
                osc1Sample = oscOsc12OS.process(0, 0.0f, [&] (float) -> float
                {
                    const float v = generateWaveform(osc1Angle, osc1Waveform, osc1UserSlot, osc1Freq, &osc1OneShot, osc1Shaping);
                    osc1Angle += d1; wrap(osc1Angle);
                    return v;
                });

                osc1Right = osc1Sample;
            }

            if (osc2Unison.active())
            {
                oscOsc12OS.processStereo(1, 3, 0.0f, [&] (float, float& right) -> float
                {
                    const float v = renderUnison(osc2Unison, osc2Waveform, osc2UserSlot, osc2Freq,
                                                 d2, osc2Shaping, osc2Stereo, right);

                    osc2Angle += d2; wrap(osc2Angle);
                    return v;
                }, osc2Sample, osc2Right);
            }
            else if (osc2Stereo)
            {
                oscOsc12OS.processStereo(1, 3, 0.0f, [&] (float, float& right) -> float
                {
                    const float v = generateWaveform(osc2Angle, osc2Waveform, osc2UserSlot, osc2Freq, &osc2OneShot, osc2Shaping, &right);
                    osc2Angle += d2; wrap(osc2Angle);
                    return v;
                }, osc2Sample, osc2Right);
            }
            else
            {
                osc2Sample = oscOsc12OS.process(1, 0.0f, [&] (float) -> float
                {
                    const float v = generateWaveform(osc2Angle, osc2Waveform, osc2UserSlot, osc2Freq, &osc2OneShot, osc2Shaping);
                    osc2Angle += d2; wrap(osc2Angle);
                    return v;
                });

                osc2Right = osc2Sample;
            }

            if (! subOscOn)
            {
                subOscSample = 0.0f;
                subOscRight = 0.0f;
            }
            else if (subUnison.active())
            {
                // Summed INSIDE the stage, for the same reason the oscillators'
                // copies are: one decimation pass filters the sum of eight copies
                // for the price of filtering one. Channels 0 and 1 of this stage,
                // which the sub had to itself and used only half of.
                oscSubOS.processStereo(0, 1, 0.0f, [&] (float, float& right) -> float
                {
                    const float v = renderUnison(subUnison, subOscWaveform, subOscUserSlot, subOscFreq,
                                                 ds, subOscShaping, false, right);

                    // The voice's own angle still runs, so switching unison off
                    // mid-note picks up where the plain sub would be -- and the
                    // Waveforms panel's playhead keeps a phase to draw.
                    subOscAngle += ds; wrap(subOscAngle);
                    return v;
                }, subOscSample, subOscRight);

                subOscSample *= subOscLevel;
                subOscRight *= subOscLevel;
            }
            else
            {
                subOscSample = oscSubOS.process(0, 0.0f, [&] (float) -> float
                {
                    const float v = generateWaveform(subOscAngle, subOscWaveform, subOscUserSlot, subOscFreq, &subOscOneShot, subOscShaping);
                    subOscAngle += ds; wrap(subOscAngle);
                    return v;
                }) * subOscLevel;

                subOscRight = subOscSample;
            }
        }
        
        // Step 3: Generate the noise source (white, pink, or an imported waveform)
        //
        // Two values now, not one. The noise source was summed to both sides at
        // the same gain and could not be anywhere but the middle; with Width up
        // its copies are spread, and on built-in noise that spread IS the effect
        // -- see setNoiseUnison.
        float noiseSample = 0.0f;
        float noiseRight = 0.0f;

        if (noiseUserSlot != nullptr)
        {
            // An imported waveform in this slot is not noise at all -- it is a
            // third oscillator, tracking the played note, with the noise level
            // knob as its volume and the noise shelves as its tone controls. So it
            // goes through generateWaveform like the others, and a non-looping
            // sample in it stops after one pass like the others.
            //
            // Being a real oscillator, its unison is the ordinary one: detuned
            // copies that beat against each other, compensated by how far apart
            // they are.
            if (noiseUnison.active())
            {
                noiseSample = renderUnison(noiseUnison, noiseType, noiseUserSlot, noiseWaveFreq,
                                           noiseWaveAngleDelta, {}, false, noiseRight);
            }
            else
            {
                noiseSample = generateWaveform(noiseWaveAngle, noiseType, noiseUserSlot,
                                               noiseWaveFreq, &noiseOneShot);
                noiseRight = noiseSample;
            }
        }
        else if (noiseUnison.active())
        {
            // Built-in noise, several copies. Each is its OWN stream -- white
            // draws again from the same generator, which is what a generator is
            // for; pink gets its own rows, because sharing one would run it N
            // times too fast and make the noise brighter as Voices went up.
            //
            // Detune is not read here and cannot be: there is no pitch to pull
            // apart. Width is the whole point -- independent streams panned to
            // different places is a stereo noise field, where one stream panned
            // anywhere is still mono.
            float l = 0.0f;
            float r = 0.0f;

            for (int i = 0; i < noiseUnison.voices; ++i)
            {
                const float v = (noiseType == White)
                              ? (random.nextFloat() * 2.0f - 1.0f)
                              : (i == 0 ? nextPinkSample (pinkState, pinkSum, pinkNoiseCounter)
                                        : nextPinkSample (pinkCopies[i - 1].state,
                                                          pinkCopies[i - 1].sum,
                                                          pinkCopies[i - 1].counter));

                l += v * noiseUnison.copies[i].gainLeft;
                r += v * noiseUnison.copies[i].gainRight;
            }

            // 1/sqrt(N), not UnisonState::compensation. These copies are
            // decorrelated at every detune setting, so the coherence term that
            // one carries would be answering a question noise never asks.
            noiseSample = l * noiseIncoherentComp;
            noiseRight  = r * noiseIncoherentComp;
        }
        else if (noiseType == White)
        {
            noiseSample = random.nextFloat() * 2.0f - 1.0f;
            noiseRight = noiseSample;
        }
        else // Pink
        {
            noiseSample = nextPinkSample (pinkState, pinkSum, pinkNoiseCounter);
            noiseRight = noiseSample;
        }

        // Apply noise EQ filters (low shelf and high shelf)
        // Process through low shelf first, then high shelf
        // Use a small temporary buffer for processing
        //
        // BOTH sides, through their own filter state. One filter over the left
        // only would leave the right unshelved, and the two shelf knobs would
        // tilt a spread noise field off to one side.
        float filteredNoise = noiseSample;
        float* channelData[1] = { &filteredNoise };
        juce::dsp::AudioBlock<float> noiseBlock(channelData, 1, 1);
        juce::dsp::ProcessContextReplacing<float> noiseContext(noiseBlock);
        if (lowShelfAmount != 0.0f)
            lowShelfFilter.process(noiseContext);
        if (highShelfAmount != 0.0f)
            highShelfFilter.process(noiseContext);
        noiseSample = filteredNoise;

        // The right side only when the two actually differ. With Width at zero
        // they hold the same number, and a second filter would compute a copy of
        // the first -- per voice, per sample, for nothing.
        if (noiseUnison.active())
        {
            float filteredRight = noiseRight;
            float* rightData[1] = { &filteredRight };
            juce::dsp::AudioBlock<float> rightBlock(rightData, 1, 1);
            juce::dsp::ProcessContextReplacing<float> rightContext(rightBlock);
            if (lowShelfAmount != 0.0f)
                lowShelfFilterR.process(rightContext);
            if (highShelfAmount != 0.0f)
                highShelfFilterR.process(rightContext);
            noiseRight = filteredRight;
        }
        else
        {
            noiseRight = noiseSample;
        }
        
        // Step 4: Apply independent levels to each source (real-time safe: atomic parameter reads)
        // LFO volume modulation: multiply by (1 + mod) so depth scales 0-2x at full LFO swing
        const float osc1Gain = osc1Level * juce::jlimit(0.0f, 2.0f, 1.0f + osc1VolMod);
        const float osc2Gain = osc2Level * juce::jlimit(0.0f, 2.0f, 1.0f + osc2VolMod);

        float osc1Out = osc1Sample * osc1Gain;
        float osc2Out = osc2Sample * osc2Gain;
        float osc1OutR = osc1Right * osc1Gain;
        float osc2OutR = osc2Right * osc2Gain;
        const float noiseGain = noiseLevel * 0.75f * juce::jlimit(0.0f, 2.0f, 1.0f + noiseVolMod);
        float noiseOut = noiseSample * noiseGain;
        float noiseOutR = noiseRight * noiseGain;

        // Step 5: Stereo mixing with per-oscillator pan (gains cached per block above)
        //
        // Each side of a stereo oscillator is placed by the pan gain for THAT side.
        // On a mono source the two sides hold the same number, so this is exactly
        // the sum it always was; on a stereo one the pan moves the whole image
        // rather than collapsing it, and a centred pan leaves the recording's own
        // width untouched.
        float leftMix = osc1Out * gainL1 + osc2Out * gainL2 + noiseOut * centerGain + subOscSample * centerGain;
        float rightMix = osc1OutR * gainR1 + osc2OutR * gainR2 + noiseOutR * centerGain + subOscRight * centerGain;
        
        // Step 6: Process envelopes (returns current amplitude value 0.0-1.0)
        // JUCE's ADSR handles all four stages automatically: Attack â†’ Decay â†’ Sustain â†’ Release
        float rawEnvelope = adsr.getNextSample();
        // Anti-click: one-pole lowpass smooths any discontinuity from ADSR retrigger
        smoothedEnvelope += envSmoothCoeff * (rawEnvelope - smoothedEnvelope);
        float envelope = smoothedEnvelope;

        // Same treatment for filter envelope (prevents raw jumps from slamming
        // the nonlinear log-space cutoff calculation and ringing the SVF).
        float rawFilterEnv = filterAdsr.getNextSample();
        smoothedFilterEnvelope += filterEnvSmoothCoeff * (rawFilterEnv - smoothedFilterEnvelope);
        float filterEnvOutput = smoothedFilterEnvelope;

        //==============================================================================
        // -- MPE PRESSURE â†’ AMPLITUDE MODULATION --
        // Pressure (Y-axis on a Seaboard / channel aftertouch on a normal keyboard) is
        // 0..1.  Map it to a smooth multiplicative amplitude boost (1.0 .. 2.0): at
        // rest the pressure is 0 â†’ no change, at full pressure the envelope is
        // doubled.  This is real-time safe (single mul) and feels natural on Roli /
        // Sensel / Linnstrument controllers without any extra parameter wiring.
        //
        // For non-MPE controllers that don't send channel pressure, mpePressure01
        // stays at 0 â†’ no effect, full backward compatibility.
        // mpePressureDepth (0..1) scales the modulation: 0 = pressure ignored, 1 = full boost.
        envelope *= juce::jlimit(0.0f, 2.0f, 1.0f + mpePressure01 * mpePressureDepth);
        
        // Modulate filter cutoff with filter envelope and LFO.
        // Amount blends unmodulated cutoff (0%) with full-range envelope sweep (100%):
        // E=1 â†’ top of range, E=0 â†’ bottom; sustain/decay set where E lands vs the knob.
        // Log-frequency space for perceptually even motion across octaves.
        // (logMin / logMax computed once per block above.)
        float filterBaseHz = baseFilterCutoff;
        if (analogDriftAmount > 0.0f)
        {
            analogFilterWalk += analogDriftWalkCoeff * ((random.nextFloat() * 2.0f - 1.0f) - analogFilterWalk);
            const float a = analogDriftAmount;
            // Proportional to cutoff (not a fixed Â±Hz, which vanished at high cutoffs)
            // so the wander is audible anywhere: Â±5% per-note offset + Â±3.5% slow
            // wander at max â€” the filter "breathes" like a warm hardware VCF.
            const float driftFrac = filterDriftOffset * 0.05f * a + analogFilterWalk * 0.035f * a;
            filterBaseHz = juce::jlimit(20.0f, 20000.0f, baseFilterCutoff * (1.0f + driftFrac));
        }
        float logKnob = std::log(juce::jmax(20.0f, filterBaseHz));
        float E = filterEnvOutput;
        const float amtNorm = juce::jlimit(-1.0f, 1.0f, filterEnvAmount / 100.0f);
        const float blend = std::abs(amtNorm);
        if (amtNorm < 0.0f)
            E = 1.0f - E;
        const float logFullRange = logMin + E * (logMax - logMin);
        float logModulated = (1.0f - blend) * logKnob + blend * logFullRange;

        const float lfoFilterScale = 0.5f;
        float lfoFactor = juce::jmax(0.0f, 1.0f + filterMod * lfoFilterScale);

        //==============================================================================
        // -- MPE TIMBRE â†’ FILTER CUTOFF MODULATION --
        // Timbre (Z-axis / CC74 / Seaboard slide) is 0..1.  Centre (no slide) = 0.5.
        // We map (timbre - 0.5) * 2 â†’ -1..+1 and treat it as a log-frequency offset of
        // up to Â±2 octaves (4 octaves total range) on the filter cutoff.  Adding it
        // *after* the env+drift logKnob computation in log-space means it sweeps the
        // filter cleanly across octaves regardless of where the cutoff knob sits.
        //
        // For non-MPE controllers that don't send CC74, mpeTimbre01 stays at 0.5 â†’
        // zero offset, full backward compatibility.
        // mpeTimbreDepth (0..1) scales the modulation: 0 = slide ignored, 1 = full Â±2 octaves.
        const float timbreBipolar = juce::jlimit(-1.0f, 1.0f, (mpeTimbre01 - 0.5f) * 2.0f) * mpeTimbreDepth;
        // Â±2 octaves = Â±ln(4) â‰ˆ Â±1.386 in natural-log units (timbreLogScale cached above).
        const float timbreLogOffset = timbreBipolar * timbreLogScale;

        // Glide-, pitch-env-, AND MPE-pitch-bend-aware key-track for all three filters.
        // The cutoff must follow the actual SOUNDING pitch, which is
        // pitchForOscillators (glide + pitch env) Ã— bendRatio (MPE per-note bend +
        // manual UI bend) â€” exactly the frequency fed to the oscillators above. Without
        // the bend term the filter snapped to the played key while a Seaboard glissando
        // (or the manual bend slider) slid the oscillators, so key-tracking lagged the
        // audible pitch. pitchForOscillators == currentPitch and bendRatio == 1.0 when
        // nothing is moving, so the static per-block path below stays bit-identical to
        // before (full backward compatibility for non-glide, non-bent, non-MPE notes).
        float keyTrackLogOffset  = keyTrackLogOffsetStatic;
        float keyTrackMultiplier = keyTrackMultiplierStatic;
        if (anyKeyTrackOn
            && (glideDelta != 0.0 || pitchEnvShapingNow || totalBendStBlock != 0.0)
            && pitchForOscillators > 0.0)
        {
            const double soundingPitch = pitchForOscillators * bendRatio;
            keyTrackMultiplier = static_cast<float>(soundingPitch / keyTrackRefHz);
            keyTrackLogOffset  = std::log(keyTrackMultiplier);
        }

        const float masterKeyTrackOffset = filterKeyTrack ? keyTrackLogOffset : 0.0f;
        // velocityLogOffset joins the other log-space offsets: it is zero at full
        // velocity and closes the filter for softer notes.
        float modulatedCutoff = std::exp(juce::jlimit(logMin, logMax, logModulated + timbreLogOffset + masterKeyTrackOffset + velocityLogOffset)) * lfoFactor;
        modulatedCutoff = juce::jlimit(20.0f, 20000.0f, modulatedCutoff);

        // Apply normal cutoff smoothing, but use a much slower slew (extra damping)
        // for a short time after a poly steal. This directly mitigates the fast
        // cutoff movement from the restarting filter envelope that the user
        // confirmed triggers the click when the filter is active.
        if (snapFilterCutoffOnNote)
        {
            // First sample of a fresh note: jump the cutoff straight to the new
            // note's target so the resonant peak doesn't sweep into the note (the
            // note-on click). Click-safe â€” the amplitude envelope is ~0 here.
            snapFilterCutoffOnNote = false;
            smoothedFilterCutoffHz = modulatedCutoff;
        }
        else
        {
            float cutoffSlew = filterCutoffSmoothCoeff;
            if (postStealCutoffSlowdownSamples > 0)
            {
                --postStealCutoffSlowdownSamples;
                // Very slow slew (precomputed ~35 ms time constant) for the first ~4 ms after steal
                cutoffSlew = postStealCutoffSlowCoeff;
            }
            smoothedFilterCutoffHz += cutoffSlew * (modulatedCutoff - smoothedFilterCutoffHz);
        }
        filter.setCutoffFrequency(smoothedFilterCutoffHz);
        
        // Check if envelope has completed (release phase finished)
        // If not active, clear the note and stop rendering
        // (Skip during voice fade â€” ADSR may hit zero before fade completes)
        if (!adsr.isActive() && voiceFadeSamplesRemaining <= 0)
        {
            // A self-oscillating filter rings under its own feedback, so its output
            // is NOT enveloped to zero. If it is still ringing when the amplitude
            // envelope completes here, a hard cut steps the output to zero and
            // clicks (worst on low notes â€” the residual sine sits far from a zero
            // crossing). Hand off to the existing voice-fade ramp, which declicks
            // the residual over kVoiceFadeLength and resets the filters when it
            // completes, instead of breaking now.
            const bool filterStillRinging = filter.isRinging();

            if (filterStillRinging)
            {
                voiceFade = 1.0f;
                voiceFadeSamplesRemaining = kVoiceFadeLength;
                // Do NOT break: fall through and keep rendering. The voice-fade
                // path below ramps the residual to zero and does the full cleanup.
            }
            else
            {
            inReleasePhase = false;
            clearCurrentNote();
            // Snapshot last pitch BEFORE zeroing so mono/legato "always glide" can
            // glide from it on the next sequential note.
            if (currentPitch > 0.0) lastPlayedPitch = currentPitch;
            currentPitch = 0.0;
            targetPitch = 0.0;
            glideDelta = 0.0;
            osc1AngleDelta = 0.0;
            osc2AngleDelta = 0.0;
            subOscAngleDelta = 0.0;
            isActive = false;
            outputSmootherL = 0.0f;
            outputSmootherR = 0.0f;
            prevSmoothedL = 0.0f;
            prevSmoothedR = 0.0f;
            meanAbsDeltaL = 0.0f;
            meanAbsDeltaR = 0.0f;

            // CRITICAL: Only process samples we've actually generated
            if (i < maxSamples)
            {
                voiceTempBuffer.clear(0, i, maxSamples - i);
                voiceTempBuffer.clear(1, i, maxSamples - i);
            }
            samplesProcessed = i; // Only process up to this point
            break;
            }
        }

        // Apply envelope to stereo mix
        // Velocity scales the note alongside the envelope. One multiply, and it is
        // 1.0 unless the patch asked for velocity, so nothing changes for a patch
        // that did not.
        const float envAndVelocity = envelope * velocityGain;

        float leftEnv = leftMix * envAndVelocity;
        float rightEnv = rightMix * envAndVelocity;

        // Step 7: Process through filter (per-sample so the self-oscillating SVF
        // tracks cutoff/key per sample and follows the amplitude envelope).
        float filtL = leftEnv;
        float filtR = rightEnv;

        // (Cutoff was already set above via smoothedFilterCutoffHz.)
        filter.setEnvelope(envelope);
        // The master filter's nonlinear chain (SVF + warm-sat tanh + per-stage hard
        // clip). This is the aliasing source under audio-rate modulation: the clip
        // squares off resonant/self-oscillating peaks for the gritty bite, but those
        // edges fold back below Nyquist. Wrapping the WHOLE chain (not just the SVF)
        // in the oversampler is what removes the fold-back.
        auto masterNL = [this](int ch, float s) noexcept -> float
        {
            float y = filter.processSample(ch, s);
            if (warmSaturationMaster)
            {
                const float drive = 1.0f + filterResonance * 2.0f;
                y = std::tanh(y * drive);
            }
            return juce::jlimit(-1.0f, 1.0f, y);
        };

        if (masterOSActive)
        {
            // 4x: filter maths already run at the oversampled rate (rate-scale set
            // by the per-note latch); the FIRs handle anti-imaging/anti-aliasing.
            filtL = masterFilterOS.process(0, filtL, [&](float s) noexcept { return masterNL(0, s); });
            filtR = masterFilterOS.process(1, filtR, [&](float s) noexcept { return masterNL(1, s); });
        }
        else
        {
            filtL = masterNL(0, filtL);   // host rate â€” bit-identical to before
            filtR = masterNL(1, filtR);
        }

        // Final safety clip (idempotent â€” each stage above already clipped).
        float outL = juce::jlimit(-1.0f, 1.0f, filtL);
        float outR = juce::jlimit(-1.0f, 1.0f, filtR);
        if (!std::isfinite(outL) || !std::isfinite(outR))
        {
            reportDspSanitize(processor);
            outL = 0.0f;
            outR = 0.0f;
        }

        // Voice fade: linear gain ramp to zero, then full cleanup.
        // This protects against ALL hard-stop clicks (allNotesOff, voice
        // stealing, legato safety net).  Applied to the FINAL output after
        // filter + ADSR so it catches every possible discontinuity source.
        if (voiceFadeSamplesRemaining > 0)
        {
            outL *= voiceFade;
            outR *= voiceFade;
            voiceFade -= 1.0f / static_cast<float>(kVoiceFadeLength);
            if (voiceFade < 0.0f) voiceFade = 0.0f;
            if (--voiceFadeSamplesRemaining <= 0)
            {
                // Fade complete: full cleanup â€” voice is now silent
                voiceFade = 0.0f;
                adsr.reset();
                smoothedEnvelope = 0.0f;
                smoothedFilterEnvelope = 0.0f;
                filterAdsr.reset();
                filter.reset();
                masterFilterOS.reset();
                oscOsc12OS.reset(); oscSubOS.reset();
                inReleasePhase = false;
                clearCurrentNote();
                // Snapshot last pitch BEFORE zeroing so mono/legato "always glide"
                // can glide from it on the next sequential note.
                if (currentPitch > 0.0) lastPlayedPitch = currentPitch;
                currentPitch = 0.0;
                targetPitch = 0.0;
                glideDelta = 0.0;
                osc1AngleDelta = 0.0;
                osc2AngleDelta = 0.0;
                subOscAngleDelta = 0.0;
                isActive = false;
                outputSmootherL = 0.0f;
                outputSmootherR = 0.0f;
                prevSmoothedL = 0.0f;
                prevSmoothedR = 0.0f;
                meanAbsDeltaL = 0.0f;
                meanAbsDeltaR = 0.0f;
                voiceTempBuffer.setSample(0, i, outL);
                voiceTempBuffer.setSample(1, i, outR);
                samplesProcessed = i + 1;
                if (i + 1 < numSamples)
                {
                    voiceTempBuffer.clear(0, i + 1, numSamples - i - 1);
                    voiceTempBuffer.clear(1, i + 1, numSamples - i - 1);
                }
                break;
            }
        }

        // Safety output smoother: one-pole lowpass on final output.
        // Catches any residual discontinuity from pitch/filter jumps during
        // legato handoff while the voiceFade is still decaying.
        outputSmootherL += kOutputSmoothCoeff * (outL - outputSmootherL);
        outputSmootherR += kOutputSmoothCoeff * (outR - outputSmootherR);

        voiceTempBuffer.setSample(0, i, outputSmootherL);
        voiceTempBuffer.setSample(1, i, outputSmootherR);
        samplesProcessed = i + 1; // Track that we processed this sample

        // ====================================================================
        // REAL-TIME CLICK / DISCONTINUITY DETECTOR (for tracking down pops)
        // ====================================================================
        // Goal: catch genuine discontinuities (pops) while IGNORING the natural
        // steepness of an ordinary waveform.  An absolute per-sample-step
        // threshold is wrong: the max step of a clean sine is AÂ·2Ï€Â·f/sr, so a
        // loud (A>0.75) or high note legitimately steps far past any audible
        // threshold every cycle and floods the log with false positives.
        //
        // Instead we compare each step against the LOCAL slope envelope â€” an EMA
        // of recent |step| (kClickSlopeEmaCoeff, ~5 ms time constant).  A clean
        // waveform's step stays within ~1.6x of this average (sine max/mean of
        // |slope| = Ï€/2); a real click is a single step many times larger.  We
        // flag only when the step is BOTH well above the local slope (ratio test)
        // AND above a small absolute floor (so near-silence, where the EMA ~0,
        // can't make a tiny step look like a huge ratio).
        constexpr float kClickSlopeEmaCoeff = 0.0045f; // ~5 ms @ 44.1 kHz
        constexpr float kClickRatio         = 8.0f;    // step vs. local slope
        constexpr float kClickAbsFloor      = 0.02f;   // ~ -34 dB, must be audible
        const float deltaL = std::abs(outputSmootherL - prevSmoothedL);
        const float deltaR = std::abs(outputSmootherR - prevSmoothedR);

        // Compare against the slope envelope as it was BEFORE this sample, so the
        // spike itself doesn't inflate the baseline it's measured against.
        const float slopeRefL = meanAbsDeltaL;
        const float slopeRefR = meanAbsDeltaR;
        meanAbsDeltaL += kClickSlopeEmaCoeff * (deltaL - meanAbsDeltaL);
        meanAbsDeltaR += kClickSlopeEmaCoeff * (deltaR - meanAbsDeltaR);

        const bool clickL = deltaL > kClickAbsFloor && deltaL > kClickRatio * slopeRefL;
        const bool clickR = deltaR > kClickAbsFloor && deltaR > kClickRatio * slopeRefR;

        if (clickL || clickR)
        {
            ++discontinuityCount;   // always count for accurate QA stats

            // Throttle the actual logging: the detector runs per-sample, so a click
            // storm (e.g. voice-steal thrashing) would otherwise emit thousands of
            // entries per buffer and bloat logs into the multi-GB range.  Log at most
            // once per ~100 ms per voice; discontinuityCount still reflects the truth.
            const int clickLogIntervalSamples = (int) std::max(1.0f, 0.1f * (float)sampleRate);
            if (samplesSinceClickLog >= clickLogIntervalSamples)
            {
                samplesSinceClickLog = 0;

                // Pack useful state into the logger fields so we can correlate with
                // the exact moment (envelope level, filter cutoff, voice mode, etc.)
                const int   noteId   = currentlyPlayingNote.isValid() ? (int)currentlyPlayingNote.noteID : -1;
                const int   midiNote = currentlyPlayingNote.isValid() ? (int)currentlyPlayingNote.initialNote : -1;
                const float envNow   = envelope;
                const float cutNow   = smoothedFilterCutoffHz;

                SAFETY_LOG_VOICE_NOTE(noteId, this, midiNote, cutNow,
                                      "AUDIO_CLICK_DETECTED");

#if SPACEDUST_CLICK_DEBUG
                clickDbgLog("CLICK   v=" + juce::String::toHexString((juce::pointer_sized_int) this).getLastCharacters(4)
                            + " note=" + juce::String(midiNote)
                            + " path=" + juce::String(lastStartPath_)
                            + " dL=" + juce::String(deltaL, 4)
                            + " dR=" + juce::String(deltaR, 4)
                            + " env=" + juce::String(envNow, 4)
                            + " cutHz=" + juce::String(cutNow, 1)
                            + " modCut=" + juce::String(modulatedCutoff, 1)
                            + " fEnv=" + juce::String(smoothedFilterEnvelope, 4)
                            + " fadeRem=" + juce::String(voiceFadeSamplesRemaining)
                            + " rel=" + juce::String((int) inReleasePhase)
                            + " adsrAct=" + juce::String((int) adsr.isActive())
                            + " ring=" + juce::String((int) filter.isRinging())
                            + " since=" + juce::String(dbgSamplesSinceStart_)
                            + " out=" + juce::String(outputSmootherL, 4)
                            + " disc=" + juce::String(discontinuityCount));
#endif

                // Also emit a plain DBG so it shows up even when safety logging is off
                DBG("Space Dust [CLICK] t=" << (samplesProcessed / std::max(1.0f, (float)sampleRate))
                    << "s  deltaL=" << deltaL << " deltaR=" << deltaR
                    << " env=" << envNow << " cutoff=" << cutNow
                    << " discCount=" << discontinuityCount
                    << " note=" << midiNote
                    << " voiceFadeRem=" << voiceFadeSamplesRemaining);
            }
        }

        if (samplesSinceClickLog < (1 << 30))
            ++samplesSinceClickLog;
        if (dbgSamplesSinceStart_ < (1 << 30))
            ++dbgSamplesSinceStart_;

        prevSmoothedL = outputSmootherL;
        prevSmoothedR = outputSmootherR;
        
        // Update oscillator phases for next sample using current angle deltas
        // (deltas are updated per-sample to support per-sample LFO modulation).
        // Skipped when oversampling: the sub-step loop up at generation already
        // advanced each phase by a full base-rate period, a quarter at a time.
        if (!oscOSActive)
        {
        osc1Angle += osc1AngleDelta;
        if (osc1Angle >= 2.0 * juce::MathConstants<double>::pi)
            osc1Angle -= 2.0 * juce::MathConstants<double>::pi;
        if (osc1Angle < 0.0)
            osc1Angle += 2.0 * juce::MathConstants<double>::pi;
            
        osc2Angle += osc2AngleDelta;
        if (osc2Angle >= 2.0 * juce::MathConstants<double>::pi)
            osc2Angle -= 2.0 * juce::MathConstants<double>::pi;
        if (osc2Angle < 0.0)
            osc2Angle += 2.0 * juce::MathConstants<double>::pi;
        
        if (subOscOn)
        {
            subOscAngle += subOscAngleDelta;
            if (subOscAngle >= 2.0 * juce::MathConstants<double>::pi)
                subOscAngle -= 2.0 * juce::MathConstants<double>::pi;
            if (subOscAngle < 0.0)
                subOscAngle += 2.0 * juce::MathConstants<double>::pi;
        }
        }   // end !oscOSActive

        // The noise slot is outside the block above on purpose: the oversampler
        // covers the two oscillators and the sub, not the noise source, so this
        // phase must advance whether oversampling is on or not.
        if (noiseUserSlot != nullptr)
        {
            noiseWaveAngle += noiseWaveAngleDelta;
            if (noiseWaveAngle >= 2.0 * juce::MathConstants<double>::pi)
                noiseWaveAngle -= 2.0 * juce::MathConstants<double>::pi;
            if (noiseWaveAngle < 0.0)
                noiseWaveAngle += 2.0 * juce::MathConstants<double>::pi;
        }

        // Advance glide after audio for this sample (see comment at loop top)
        if (glideDelta != 0.0)
        {
            currentPitch += glideDelta;
            if ((glideDelta > 0.0 && currentPitch >= targetPitch) ||
                (glideDelta < 0.0 && currentPitch <= targetPitch))
            {
                currentPitch = targetPitch;
                glideDelta = 0.0;
            }
            currentPitch = juce::jlimit(20.0, 20000.0, currentPitch);
        }
    }
    
    // CRITICAL: Only process and copy the samples we actually generated
    // This prevents buffer bounds assertions when ADSR finishes mid-block
    if (samplesProcessed > 0)
    {
        // Step 8: Copy filtered stereo signal to output
        if (outputBuffer.getNumChannels() >= 2 && voiceTempBuffer.getNumChannels() >= 2)
        {
            outputBuffer.addFrom(0, startSample, voiceTempBuffer, 0, 0, samplesProcessed);
            outputBuffer.addFrom(1, startSample, voiceTempBuffer, 1, 0, samplesProcessed);
        }
        else
        {
            for (int ch = 0; ch < outputBuffer.getNumChannels(); ++ch)
                outputBuffer.addFrom(ch, startSample, voiceTempBuffer, 0, 0, samplesProcessed);
        }
    }

    //==============================================================================
    // -- Where each imported sample has got to --
    // For the playhead in the Waveforms window. Once per block, not per sample:
    // the line is redrawn about twenty times a second, so a phase from the end of
    // the block is as current as anything on screen can be, and this costs one
    // store per source instead of one per source per sample.
    //
    // A one-shot that has finished publishes nothing, so its line disappears at
    // the moment its sound does rather than sitting at the end of the sample.
    if (processor != nullptr && samplesProcessed > 0 && processor->isUserWavePhaseWanted())
    {
        const auto publish = [this] (const UserWaveSlot* slot, const OneShotState& oneShot,
                                     double angle, UserWave::Group group)
        {
            if (slot == nullptr || oneShot.finished)
                return;

            constexpr double toPhase = 1.0 / (2.0 * juce::MathConstants<double>::pi);
            processor->publishUserWavePhase(group, (float) (angle * toPhase));
        };

        publish(osc1UserSlot, osc1OneShot, osc1Angle, UserWave::Group::Osc1);
        publish(osc2UserSlot, osc2OneShot, osc2Angle, UserWave::Group::Osc2);

        if (subOscOn)
            publish(subOscUserSlot, subOscOneShot, subOscAngle, UserWave::Group::Sub);

        publish(noiseUserSlot, noiseOneShot, noiseWaveAngle, UserWave::Group::Noise);
    }
}

//==============================================================================
// -- DSP Initialization --

void SynthVoice::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    //==============================================================================
    // -- CRITICAL: DSP Initialization with Valid Sample Rate --
    // 
    // This method MUST be called explicitly in PluginProcessor::prepareToPlay()
    // after voices are added but BEFORE synth.setCurrentPlaybackSampleRate() is called.
    //
    // Why DSP initialization MUST happen here, not in constructor:
    // 1. Sample rate is only known when host calls prepareToPlay()
    // 2. DSP objects (filter, ADSR) require valid sample rate for prepare()
    // 3. Constructor runs before sample rate is known (sampleRate = 0)
    // 4. Initializing DSP in constructor with sampleRate=0 causes:
    //    - StateVariableTPTFilter assertions (invalid sample rate)
    //    - ADSR assertions (invalid timing calculations)
    //    - Corrupted voice state in strict hosts like Ableton Live
    //
    // Initialization order (MUST be followed):
    // 1. Store sample rate (required for all DSP calculations)
    // 2. Safety check: ensure sample rate is valid
    // 3. Prepare filter with valid spec (sample rate, max block size, channels)
    // 4. Update filter parameters (cutoff, resonance, mode)
    // 5. Set ADSR sample rate (required for timing calculations)
    // 6. Update ADSR parameters (now that sample rate is known)
    // 7. Mark DSP as initialized (prevents re-initialization issues)
    //
    // This is the standard, bulletproof way to initialize voices in modern JUCE.
    
    // Safety check: ensure sample rate is valid
    if (sampleRate <= 0.0)
    {
        // CRITICAL: Logger calls removed to prevent LeakedObjectDetector assertions
        // DBG("Space Dust: ERROR - Invalid sample rate in prepareToPlay: " + safeStringFromNumber(sampleRate));
        return; // Skip DSP initialization if sample rate is invalid
    }
    
    // Store sample rate for voice calculations
    this->sampleRate = sampleRate;

    // How fast the shaping knobs glide, in samples, for a fixed time in
    // milliseconds -- so Sync sweeps the same at 44.1 kHz as it does at 192.
    shapingSmoothingCoeff = sampleRate > 0.0
        ? 1.0 - std::exp(-1.0 / (shapingSmoothingMs * 0.001 * sampleRate))
        : 1.0;
    
    // Re-seed Random with sample rate for additional uniqueness (already seeded in constructor with voice address)
    // This ensures each voice has different noise patterns even after sample rate changes
    random.setSeed(static_cast<juce::int64>(reinterpret_cast<uintptr_t>(this)) + static_cast<juce::int64>(sampleRate * 1000));
    
    // Prepare filter with sample rate (2 channels for stereo panning)
    const juce::uint32 maxBlockSize = static_cast<juce::uint32>(juce::jmax(4096, samplesPerBlock));
    filter.prepare({ sampleRate, maxBlockSize, 2 });

    // Filter oversamplers. Re-apply the current factor so each filter's rate-scale
    // matches after a sample-rate change.
    {
        masterFilterOS.prepare();
        oscOsc12OS.prepare(); oscSubOS.prepare();
        // Derive each stage's factor + scale from current params (keeps the latch
        // INVARIANT). The first note re-latches with the live params at note-start.
        updateOversampleLatch();
    }

    // Pre-allocate voice buffers (stereo for per-oscillator pan)
    voiceTempBuffer.setSize(2, static_cast<int>(maxBlockSize), false, false, false);
    voiceSingleSampleBuffer.setSize(2, 1, false, false, false);
    
    // Prepare noise EQ shelf filters
    // Low shelf: affects frequencies below 200 Hz
    // High shelf: affects frequencies above 1.5 kHz
    juce::dsp::ProcessSpec eqSpec{ sampleRate, maxBlockSize, 1 };
    lowShelfFilter.prepare(eqSpec);
    highShelfFilter.prepare(eqSpec);
    lowShelfFilterR.prepare(eqSpec);
    highShelfFilterR.prepare(eqSpec);
    updateNoiseEqFilters();
    
    // Update filter parameters AFTER filter is prepared
    // This ensures filter is ready to accept parameter changes
    updateFilter();
    
    // Prepare ADSR with sample rate (required for proper timing calculations)
    // JUCE's ADSR needs to know the sample rate to convert seconds to samples
    adsr.setSampleRate(sampleRate);
    
    // Prepare Filter Envelope ADSR with sample rate
    filterAdsr.setSampleRate(sampleRate);
    
    // Update ADSR parameters with new sample rate (recalculates internal timing)
    // Now that sample rate is valid, parameters can be safely applied
    updateAdsrParameters();
    updateFilterAdsrParameters();

    // Anti-click envelope smoother: ~3ms one-pole lowpass on ADSR output
    envSmoothCoeff = 1.0f - std::exp(-1.0f / (0.003f * static_cast<float>(sampleRate)));
    filterEnvSmoothCoeff = 1.0f - std::exp(-1.0f / (0.003f * static_cast<float>(sampleRate)));  // same time constant for filter env
    smoothedEnvelope = 0.0f;
    smoothedFilterEnvelope = 0.0f;
    // Base cutoff smoothing (~4 ms). This is a compromise:
    // - Fast enough for responsive LFO wobbles, manual filter sweeps, and MPE timbre.
    // - Slow enough to help tame most note-start transients.
    // The heavy lifting for poly steals (when the filter envelope restarts on a stolen voice)
    // is done by the postStealCutoffSlowdownSamples window + precomputed
    // postStealCutoffSlowCoeff (very slow slew) + smoothedFilterEnvelope.
    filterCutoffSmoothCoeff = 1.0f - std::exp(-1.0f / (0.004f * static_cast<float>(sampleRate)));
    smoothedFilterCutoffHz = juce::jlimit(20.0f, 20000.0f, baseFilterCutoff);
    snapFilterCutoffOnNote = false;
    // ~3 s time constant for perceptible (but still slow) analog-style wander
    analogDriftWalkCoeff = 1.0f - std::exp(-1.0f / (3.0f * static_cast<float>(sampleRate)));
    voiceFade = 1.0f;
    voiceFadeSamplesRemaining = 0;
    outputSmootherL = 0.0f;
    outputSmootherR = 0.0f;

    prevSmoothedL = 0.0f;
    prevSmoothedR = 0.0f;
    meanAbsDeltaL = 0.0f;
    meanAbsDeltaR = 0.0f;
    discontinuityCount = 0;
    postStealCutoffSlowdownSamples = 0;
    postStealCutoffSlowCoeff = 1.0f - std::exp(-1.0f / (0.035f * static_cast<float>(sampleRate)));

    // Mark DSP as properly initialized
    // This prevents issues if setCurrentPlaybackSampleRate() is called again
    isDspInitialized = true;
    
    // CRITICAL: Logger calls removed to prevent LeakedObjectDetector assertions
    // DBG("Space Dust: SynthVoice DSP initialized - sampleRate: " + safeStringFromNumber(sampleRate) + ", isDspInitialized: " + safeStringFromBool(isDspInitialized));
}

//==============================================================================
// -- Sample Rate Setup --

void SynthVoice::setCurrentSampleRate(double newRate)
{
    //==============================================================================
    // -- CRITICAL: Safe Sample Rate Update --
    //
    // MPE: replaces juce::SynthesiserVoice::setCurrentPlaybackSampleRate().
    // juce::MPESynthesiserVoice exposes this as a virtual method, called by
    // MPESynthesiser whenever its setCurrentPlaybackSampleRate() runs.
    //
    // This method is called:
    // 1. Automatically when voices are added (with sampleRate=0) â† we must ignore!
    // 2. When synth.setCurrentPlaybackSampleRate() is called explicitly
    //
    // IMPORTANT: DSP initialization happens in prepareToPlay(), NOT here.
    // This method should only update the stored sample rate if it's valid.

    // Let the base class store the new rate in its currentSampleRate member.
    juce::MPESynthesiserVoice::setCurrentSampleRate(newRate);
    
    // CRITICAL: Ignore invalid sample rates (especially 0 when voices are first added)
    // JUCE automatically calls this with 0 when voices are added, before we can
    // properly initialize them. We must skip DSP initialization in this case.
    if (newRate <= 0.0)
    {
        // Don't log warnings for the expected case (sampleRate=0 during voice creation)
        // This is normal - DSP will be initialized properly in prepareToPlay()
        return; // Skip DSP update if sample rate is invalid
    }
    
    // Update stored sample rate
    sampleRate = newRate;
    
    // Only re-initialize DSP if it was already initialized (sample rate change scenario)
    // If DSP hasn't been initialized yet, prepareToPlay() will handle it
    if (isDspInitialized)
    {
        // Re-initialize DSP with new sample rate (in case sample rate changed)
        // This ensures filter and ADSR use the correct rate
        const juce::uint32 maxBlockSize = 512; // Safe maximum
        filter.prepare({ newRate, maxBlockSize, 2 });
        {
            masterFilterOS.prepare();
            oscOsc12OS.prepare(); oscSubOS.prepare();
            updateOversampleLatch();  // derive factor + scale per stage (keeps INVARIANT)
        }
        updateFilter();
        juce::dsp::ProcessSpec eqSpec{ newRate, maxBlockSize, 1 };
        lowShelfFilter.prepare(eqSpec);
        highShelfFilter.prepare(eqSpec);
        lowShelfFilterR.prepare(eqSpec);
        highShelfFilterR.prepare(eqSpec);
        updateNoiseEqFilters();
        
        adsr.setSampleRate(newRate);
        filterAdsr.setSampleRate(newRate);
        updateAdsrParameters();
        updateFilterAdsrParameters();
        envSmoothCoeff = 1.0f - std::exp(-1.0f / (0.003f * static_cast<float>(newRate)));
        filterEnvSmoothCoeff = 1.0f - std::exp(-1.0f / (0.003f * static_cast<float>(newRate)));
        // Use the current (slower) base cutoff smoothing, not the old 1.5ms value
        filterCutoffSmoothCoeff = 1.0f - std::exp(-1.0f / (0.004f * static_cast<float>(newRate)));
        postStealCutoffSlowCoeff = 1.0f - std::exp(-1.0f / (0.035f * static_cast<float>(newRate)));
        analogDriftWalkCoeff = 1.0f - std::exp(-1.0f / (3.0f * static_cast<float>(newRate)));
    }
    // If DSP not initialized yet, prepareToPlay() will handle initialization
}

//==============================================================================
// -- Parameter Update Methods --

// The upper bound covers the four built-in shapes AND the eight import slots.
// It was 3 when Square was the last waveform there was, which silently turned
// every imported waveform into a square wave: the choice arrived as 4 or more,
// the clamp pulled it back to 3, and nothing anywhere reported a problem.
// Derived from the slot count rather than written out, so it cannot go stale
// again if the number of slots ever changes.
static constexpr int kHighestOscWaveformIndex = UserWave::oscUserBase + UserWave::numSlots - 1;
static constexpr int kHighestNoiseTypeIndex   = UserWave::noiseUserBase + UserWave::numSlots - 1;

void SynthVoice::setOsc1Waveform(int waveform)
{
    osc1Waveform = juce::jlimit(0, kHighestOscWaveformIndex, waveform);
}

void SynthVoice::setOsc2Waveform(int waveform)
{
    osc2Waveform = juce::jlimit(0, kHighestOscWaveformIndex, waveform);
}

//==============================================================================
// -- Oscillator Pitch Tuning Methods --
// Each oscillator has independent coarse tuning (Â±24 semitones) and fine detuning (Â±50 cents)
// Double-click any knob to reset to 0

void SynthVoice::setOsc1CoarseTune(float semitones)
{
    osc1CoarseTune = juce::jlimit(-24.0f, 24.0f, semitones);
    // Update frequency if note is playing â€” base Hz must match renderNextBlock's currentPitch
    // (glide / legato). getMidiNoteInHertz(getCurrentlyPlayingNote()) can disagree for a whole
    // block because updateVoicesWithParameters runs before MIDI is applied in renderNextBlock.
    if (isActive)
    {
        // MPE: currentlyPlayingNote is an MPENote; pull initialNote when valid.
        const int n = currentlyPlayingNote.isValid()
                          ? static_cast<int>(currentlyPlayingNote.initialNote)
                          : -1;
        const double baseHz = (currentPitch > 0.0)
                                  ? juce::jlimit(20.0, 20000.0, currentPitch)
                                  : (n >= 0 ? juce::MidiMessage::getMidiNoteInHertz(n) : 440.0);
        updateOsc1Frequency(baseHz);
    }
}

void SynthVoice::setOsc1Detune(float cents)
{
    osc1Detune = juce::jlimit(-50.0f, 50.0f, cents);
    if (isActive)
    {
        const int n = currentlyPlayingNote.isValid()
                          ? static_cast<int>(currentlyPlayingNote.initialNote)
                          : -1;
        const double baseHz = (currentPitch > 0.0)
                                  ? juce::jlimit(20.0, 20000.0, currentPitch)
                                  : (n >= 0 ? juce::MidiMessage::getMidiNoteInHertz(n) : 440.0);
        updateOsc1Frequency(baseHz);
    }
}

void SynthVoice::setOsc2CoarseTune(float semitones)
{
    osc2CoarseTune = juce::jlimit(-24.0f, 24.0f, semitones);
    if (isActive)
    {
        const int n = currentlyPlayingNote.isValid()
                          ? static_cast<int>(currentlyPlayingNote.initialNote)
                          : -1;
        const double baseHz = (currentPitch > 0.0)
                                  ? juce::jlimit(20.0, 20000.0, currentPitch)
                                  : (n >= 0 ? juce::MidiMessage::getMidiNoteInHertz(n) : 440.0);
        updateOsc2Frequency(baseHz);
    }
}

void SynthVoice::setOsc2Detune(float cents)
{
    osc2Detune = juce::jlimit(-50.0f, 50.0f, cents);
    if (isActive)
    {
        const int n = currentlyPlayingNote.isValid()
                          ? static_cast<int>(currentlyPlayingNote.initialNote)
                          : -1;
        const double baseHz = (currentPitch > 0.0)
                                  ? juce::jlimit(20.0, 20000.0, currentPitch)
                                  : (n >= 0 ? juce::MidiMessage::getMidiNoteInHertz(n) : 440.0);
        updateOsc2Frequency(baseHz);
    }
}

void SynthVoice::setOsc1Level(float level)
{
    osc1Level = juce::jlimit(0.0f, 1.0f, level);
}

void SynthVoice::setOsc2Level(float level)
{
    osc2Level = juce::jlimit(0.0f, 1.0f, level);
}

void SynthVoice::setOsc1Pan(float pan)
{
    osc1Pan = juce::jlimit(-1.0f, 1.0f, pan);
}

void SynthVoice::setOsc2Pan(float pan)
{
    osc2Pan = juce::jlimit(-1.0f, 1.0f, pan);
}

void SynthVoice::setNoiseLevel(float level)
{
    noiseLevel = juce::jlimit(0.0f, 1.0f, level);
}

void SynthVoice::setNoiseType(int type)
{
    // Anything past Pink selects an imported waveform. Collapsing every non-zero
    // value to Pink, as this used to, meant an imported noise source could never
    // reach the voice at all.
    noiseType = juce::jlimit(0, kHighestNoiseTypeIndex, type);
}

void SynthVoice::setSubOscOn(bool on)
{
    subOscOn = on;
}

void SynthVoice::setSubOscWaveform(int waveform)
{
    subOscWaveform = juce::jlimit(0, kHighestOscWaveformIndex, waveform);
}

void SynthVoice::setSubOscLevel(float level)
{
    subOscLevel = juce::jlimit(0.0f, 1.0f, level);
}

void SynthVoice::setSubOscCoarse(float semitones)
{
    subOscCoarse = juce::jlimit(-36.0f, 36.0f, semitones);
}

void SynthVoice::setLowShelfAmount(float amount)
{
    lowShelfAmount = juce::jlimit(-1.0f, 1.0f, amount);
    updateNoiseEqFilters();
}

void SynthVoice::setHighShelfAmount(float amount)
{
    highShelfAmount = juce::jlimit(-1.0f, 1.0f, amount);
    updateNoiseEqFilters();
}

void SynthVoice::setFilterMode(int mode)
{
    filterMode = juce::jlimit(0, NonlinearSVF::numModes - 1, mode);
    updateFilter();
}

void SynthVoice::setFilterCutoff(float cutoffHz)
{
    baseFilterCutoff = juce::jlimit(20.0f, 20000.0f, cutoffHz);
    // filterCutoff will be updated in renderNextBlock with envelope modulation
    updateFilter();
}

void SynthVoice::setFilterResonance(float resonance)
{
    filterResonance = juce::jlimit(0.0f, 1.0f, resonance);
    updateFilter();
}

void SynthVoice::setWarmSaturationMaster(bool enabled)
{
    warmSaturationMaster = enabled;
}

bool SynthVoice::anyFastLfoReaches (int destination) const noexcept
{
    // "Does a FAST LFO reach this destination?" -- the question the oversample
    // latch used to answer by comparing one cached drop-down index against a
    // number. It now walks four amounts, which asks the same question of the
    // modulation matrix instead: an amount is non-zero exactly when a routing
    // exists, because ModMatrix::setRouting REMOVES a routing set to zero rather
    // than leaving a dead entry behind.
    //
    // No string, no lookup, no allocation. lfoModAmount was filled by
    // setLfoModAmounts() from a list the message thread compiled, and lfoRateHz
    // by setLfoRates(), so this is four float compares.
    if (destination < 0 || destination >= spacedust::numVoicePerSampleMod)
        return false;

    for (int lfo = 0; lfo < spacedust::numLfos; ++lfo)
        if (lfoModAmount[destination][lfo] != 0.0f
            && lfoRateHz[lfo] >= kOversampleLfoHzThreshold)
            return true;

    return false;
}

void SynthVoice::updateOversampleLatch() noexcept
{
    // A filter needs oversampling when its nonlinear stage is engaged: warm
    // saturation, or resonance high enough to clip/ring. When the global
    // oversample param is off, nothing oversamples.
    //
    // It ALSO needs oversampling when an LFO is sweeping its cutoff at audio rate,
    // whatever the resonance. That is the case this parameter was added for in the
    // first place, but the test was only ever nonlinearity, so a fast LFO on a
    // clean, low-resonance filter folded back with oversampling sitting switched
    // off. Audible as soon as the LFO range was widened past a few hundred Hz.
    //
    // THE TEST MOVED WITH THE FEATURE. It used to read "the LFO Destination
    // drop-down says Filter". The drop-down is gone, so it now reads "some LFO
    // has a routing to filterCutoff" -- and it reads it out of the very amounts
    // renderNextBlock applies per sample, so the latch and the sound cannot
    // disagree about which LFO sweeps what. Deleting the drop-down without
    // moving this test would have left the latch permanently off, with nothing
    // to show for it but aliasing.
    const bool masterFastMod = anyFastLfoReaches (spacedust::psm_filterCutoff);

    const bool masterWant = oversampleFilter
        && (warmSaturationMaster || filterResonance >= kOversampleResThreshold || masterFastMod);

    masterOSActive = masterWant;

    // Oscillator oversampling: only for an LFO sweeping PITCH at audio rate, which is
    // the case that makes the naive shapes fold back audibly. Gated on the same global
    // anti-alias parameter as the filters so one switch turns all of it off.
    //
    // Either oscillator counts. The shapes fold back per oscillator, and the two
    // Coarse knobs are separately assignable now that the drop-down's single
    // "Pitch" entry has become one routing per oscillator.
    const bool fastPitch = anyFastLfoReaches (spacedust::psm_osc1Pitch)
                        || anyFastLfoReaches (spacedust::psm_osc2Pitch);
    const bool oscWant = oversampleFilter && fastPitch;

    if (oscWant != oscOSActive)
    {
        oscOSActive = oscWant;
        oscOsc12OS.setFactor(oscWant ? kOscOSFactor : 1);
        oscSubOS  .setFactor(oscWant ? kOscOSFactor : 1);
    }

    // Apply the matching scale to the master stage (maintains the INVARIANT).
    // setFactor clears the FIR; setSampleRateScale recomputes g at the
    // (over)sampled rate.
    const int mf = masterWant ? kFilterOSFactor : 1;
    masterFilterOS.setFactor(mf); filter.setSampleRateScale(mf);
}

void SynthVoice::setLfoRates (const double* hzPerLfo) noexcept
{
    if (hzPerLfo == nullptr)
        return;

    // Stored, not latched: the latch itself re-reads these at note start. Changing an
    // LFO rate mid-note therefore takes effect on the NEXT note, deliberately -- the
    // same rule the resonance test follows, because switching a filter's sample-rate
    // scale while it holds resonant energy re-introduces a note-onset click.
    for (int lfo = 0; lfo < spacedust::numLfos; ++lfo)
        lfoRateHz[lfo] = hzPerLfo[lfo];
}

void SynthVoice::setFilterOversample(bool enabled)
{
    if (enabled == oversampleFilter)
        return;
    oversampleFilter = enabled;
    // Idle voices re-derive immediately so their filter scale stays consistent with
    // the render routing. ACTIVE voices keep their per-note latch until the next note
    // start â€” switching the sample-rate scale on a ringing filter would click. The
    // param defaults ON and is rarely toggled live, so the one-note delay is benign.
    if (!isActive)
        updateOversampleLatch();
}

void SynthVoice::setFilterKeyTrack(bool enabled)
{
    // No filter rebuild needed: the offset is applied per-block in renderNextBlock,
    // where the played note number is known.
    filterKeyTrack = enabled;
}

//==============================================================================
// -- Filter Envelope Methods --

void SynthVoice::setFilterEnvAttack(float seconds)
{
    filterEnvAttackTime = juce::jmax(0.01f, seconds);
    updateFilterAdsrParameters();
}

void SynthVoice::setFilterEnvDecay(float seconds)
{
    filterEnvDecayTime = juce::jmax(0.01f, seconds);
    updateFilterAdsrParameters();
}

void SynthVoice::setFilterEnvSustain(float level)
{
    filterEnvSustainLevel = juce::jlimit(0.0f, 1.0f, level);
    updateFilterAdsrParameters();
}

void SynthVoice::setFilterEnvRelease(float seconds)
{
    filterEnvReleaseTime = juce::jmax(0.01f, seconds);
    updateFilterAdsrParameters();
}

void SynthVoice::setFilterEnvAmount(float amount)
{
    filterEnvAmount = juce::jlimit(-100.0f, 100.0f, amount);
}

//==============================================================================
// -- ADSR Envelope Methods --

void SynthVoice::setEnvAttack(float seconds)
{
    // Clamp to parameter range (0.01-20.0s, skewed)
    envAttackTime = juce::jlimit(0.01f, 20.0f, seconds);
    // CRITICAL: Must call updateAdsrParameters() to apply the change
    // JUCE's ADSR requires all parameters to be set together via setParameters()
    updateAdsrParameters();
}

void SynthVoice::setEnvDecay(float seconds)
{
    // Clamp to parameter range (0.01-20.0s, skewed)
    envDecayTime = juce::jlimit(0.01f, 20.0f, seconds);
    // CRITICAL: Must call updateAdsrParameters() to apply the change
    // This ensures decay time changes are reflected immediately, regardless of current envelope state
    updateAdsrParameters();
}

void SynthVoice::setEnvSustain(float level)
{
    // Clamp to parameter range (0.0-1.0, linear amplitude level)
    envSustainLevel = juce::jlimit(0.0f, 1.0f, level);
    // CRITICAL: Must call updateAdsrParameters() to apply the change
    // This ensures sustain level changes are reflected immediately, regardless of current envelope state
    // Note: Sustain is an amplitude level (0.0-1.0), not a time value
    updateAdsrParameters();
}

void SynthVoice::setEnvRelease(float seconds)
{
    // Clamp to parameter range (0.01-20.0s, skewed) - long cosmic tails!
    envReleaseTime = juce::jlimit(0.01f, 20.0f, seconds);
    // CRITICAL: Must call updateAdsrParameters() to apply the change
    // This ensures release time changes are reflected immediately, even during release phase
    updateAdsrParameters();
}

//==============================================================================
// -- Glide (Portamento) Parameter Update --

void SynthVoice::setGlideTime(float seconds)
{
    // Clamp to parameter range (0.0-5.0s, skewed with midpoint at 1.0s)
    glideTimeSeconds = juce::jlimit(0.0f, 5.0f, seconds);
    
    // If a USER glide is in progress, rescale its speed live as the knob moves.
    //
    // CRITICAL: only do this when glide is actually ON (glideTimeSeconds > 0). This
    // method is called every processBlock, and an in-progress glideDelta is NOT
    // necessarily a user glide â€” noteStarted() sets a short 3 ms anti-click auto-glide
    // on every mono/legato note change even when glide is off. The old code's else
    // branch zeroed glideDelta ("instant") for that auto-glide but left currentPitch
    // STRANDED partway between the two notes (it never set currentPitch = targetPitch),
    // so the note played at a wrong, in-between pitch â€” and it compounded across note
    // changes ("out of key"). It only bit in small-buffer hosts (FL Studio), where the
    // 3 ms auto-glide spans several blocks so this clobber landed mid-glide; large-
    // buffer hosts (Ableton) finished the auto-glide within one block, hiding it.
    // When glide is OFF we now leave the auto-glide untouched so it completes and
    // reaches targetPitch. (samplesToGlide is therefore always > 0 here.)
    if (glideDelta != 0.0 && sampleRate > 0.0 && glideTimeSeconds > 0.0f)
    {
        const double pitchDifference = targetPitch - currentPitch;
        const double samplesToGlide  = glideTimeSeconds * sampleRate;
        glideDelta = pitchDifference / samplesToGlide;
    }
}

void SynthVoice::setLegatoGlide(bool enabled)
{
    legatoGlideEnabled = enabled;
}

void SynthVoice::setPitchCurveTime(float seconds)
{
    pitchCurveTime = juce::jlimit(0.0f, 10.0f, seconds);
}

void SynthVoice::setPitchBendAmount(float semitones)
{
    pitchBendAmountFloat = juce::jlimit(0.0f, 24.0f, semitones);
}

void SynthVoice::setPitchBend(float value)
{
    pitchBend = juce::jlimit(-1.0f, 1.0f, value);
}

void SynthVoice::setLfoModAmounts (const float* amounts) noexcept
{
    if (amounts == nullptr)
        return;

    bool any     = false;
    bool changed = false;

    for (int d = 0; d < spacedust::numVoicePerSampleMod; ++d)
    {
        for (int lfo = 0; lfo < spacedust::numLfos; ++lfo)
        {
            const float a = amounts[d * spacedust::numLfos + lfo];

            changed = changed || (a != lfoModAmount[d][lfo]);
            lfoModAmount[d][lfo] = a;
            any = any || (a != 0.0f);
        }
    }

    anyLfoModAmount = any;

    // The oversample latch asks whether a FAST LFO reaches the cutoff or a
    // pitch. Re-derive it only when one of those amounts actually moved: this
    // runs once a block for every voice, and the latch touches each stage's FIR
    // and filter coefficients.
    //
    // Idle voices re-derive at once; an ACTIVE voice keeps the latch it took at
    // note start, which is the same rule resonance and LFO rate already follow
    // -- switching a filter's sample-rate scale while it holds resonant energy
    // re-introduces a note-onset click.
    if (changed && !isActive)
        updateOversampleLatch();
}

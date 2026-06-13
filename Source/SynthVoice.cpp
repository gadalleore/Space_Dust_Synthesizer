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
    // -- Read MPE note state --
    // currentlyPlayingNote is set by MPESynthesiser::startVoice() before this call.
    //   initialNote                       : MIDI note number 0..127
    //   noteOnVelocity.asUnsignedFloat()  : 0..1 velocity
    //   totalPitchbendInSemitones         : signed double — combined master +
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
    // overlaps — MPESynthesiser will fire notePitchbendChanged() if it actually
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
            analogFilterWalk = 0.0f;
        }
    }
    
    // LFO Retrigger: Reset LFO phases if retrigger is enabled
    if (processor != nullptr)
    {
        try
        {
            if (processor->lfo1Retrigger.load())
            {
                processor->lfo1CurrentPhase = 0.0;
            }
        }
        catch (...) { /* ignore */ }
        
        try
        {
            if (processor->lfo2Retrigger.load())
            {
                processor->lfo2CurrentPhase = 0.0;
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
    //       (isLegatoNote == true)  → classic "fingered glide" / portamento
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
    // voices for glide "from". Mono/Legato must NEVER use that — it picked another voice's Hz
    // and caused random detuning on every note change (regression from aggressive pitch resync).
    const bool allowCrossVoiceGlideFrom = (voiceMode == 0);

    auto computeGlideFromPitch = [this, allowCrossVoiceGlideFrom, wasVoiceActive, voiceMode]
                                  (double fallBackTarget) -> double
    {
        double fromPitch = currentPitch;
        // Poly mode: don't reuse currentPitch when the voice was idle — that voice
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
                // Anti-click: legato with no user glide — tiny 3ms auto-glide
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
        // Anti-click: mono retrigger with no user glide — apply a tiny 3ms auto-glide
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
    //   aligned chord transitions (see the two-step noteStopped(false) →
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

    if (shouldHardResetForPoly)
    {
        // Truly new poly voice: safe to do full reset
        osc1Angle = 0.0;
        osc2Angle = 0.0;
        subOscAngle = 0.0;
        pinkState.fill(0.0f);
        pinkSum = 0.0f;
        pinkNoiseCounter = 0;
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
        modFilter1.reset();
        modFilter2.reset();
        filterAdsr.reset();

        adsr.noteOn();
        inReleasePhase = false;
        isActive = true;
        updateFilter();
        filterAdsr.noteOn();
        // Fresh voice: put the cutoff at the new note's value immediately (no slew
        // sweep of the resonant peak into the note). See snapFilterCutoffOnNote.
        snapFilterCutoffOnNote = true;
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
            // an AUDIBLE level — and you cannot move that ring abruptly without an
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
                if (filter.isRinging())     filter.reset();
                if (modFilter1.isRinging()) modFilter1.reset();
                if (modFilter2.isRinging()) modFilter2.reset();
                snapFilterCutoffOnNote = true;
            }
            else
            {
                // Previous note still loud (long Release + fast "running bass"). Leave
                // the ringing filter running (no reset → no pop) and move the cutoff
                // GENTLY to the new note via the slow-slew window (no snap → no click).
                // A smooth filter glide between crashing notes instead of an abrupt
                // peak jump. snapFilterCutoffOnNote stays false here.
                postStealCutoffSlowdownSamples = kPostStealCutoffSlowdownLength;
            }
        }
        adsr.noteOn();
        filterAdsr.noteOn();
        inReleasePhase = false;
        isActive = true;
    }
}

void SynthVoice::noteStopped(bool allowTailOff)
{
    // MPE replacement for juce::SynthesiserVoice::stopNote(velocity, allowTailOff).
    // The semantics are identical: allowTailOff=true → release the envelope normally;
    // allowTailOff=false → hard stop (we apply a short voice fade to avoid clicks).

    // Memory-safety logger: voice stop. Only log when the voice was actually
    // active — turnOffAllVoices()/prepare sweeps call noteStopped() on every
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
    // MPESynthesiser::startVoice.  Do NOT start a fade or touch ADSR — just
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
        // running with all DSP intact while voiceFade ramps 1→0.  Only when
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
    // release left over from a previous note or a poly→mono switch) that must
    // not keep ringing under the new note.  Bypass the legato/preserve handoff
    // (we are NOT reusing this voice) and start the short click-safe fade; the
    // existing fade→cleanup path in renderNextBlock finishes the job.
    voiceFade = 1.0f;
    voiceFadeSamplesRemaining = kVoiceFadeLength;
}

//==============================================================================
// -- MPE Expression Callbacks --
//
// Fired from MPESynthesiser when the controller updates pressure / pitch-bend /
// timbre / key state for this voice's currently playing MPENote.  These are
// real-time safe (called in the audio rendering callback by MPEInstrument),
// so we keep them lock-free — just cache the new value, the audio thread will
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
    // action here — the ADSR is already in its sustain/release stage as appropriate.
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
// longer need controllerMoved() or pitchWheelMoved() overrides — they belonged
// to the old juce::SynthesiserVoice API.  Generic non-MPE MIDI CCs would arrive
// at SpaceDustSynthesiser::handleController if we ever choose to override it.

//==============================================================================
// -- Waveform Generation --

float SynthVoice::generateWaveform(double angle, int waveform)
{
    switch (waveform)
    {
        case Sine:
            return std::sin(angle);
            
        case Triangle:
            // Triangle wave: 2 * abs(2 * (phase - floor(phase + 0.5))) - 1
            {
                double normalized = angle / (2.0 * juce::MathConstants<double>::pi);
                double phase = normalized - std::floor(normalized + 0.5);
                return static_cast<float>(4.0 * std::abs(phase) - 1.0);
            }
            
        case Saw:
            // Simple sawtooth: 2 * (phase - floor(phase + 0.5))
            {
                double normalized = angle / (2.0 * juce::MathConstants<double>::pi);
                double phase = normalized - std::floor(normalized + 0.5);
                return static_cast<float>(2.0 * phase);
            }
            
        case Square:
            return std::sin(angle) > 0.0f ? 1.0f : -1.0f;
            
        default:
            return std::sin(angle);
    }
}

//==============================================================================
// -- Oscillator Frequency Updates --

//==============================================================================
// -- Oscillator Pitch Tuning --
// Each oscillator has independent coarse tuning (±24 semitones) and fine detuning (±50 cents)
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
        osc1AngleDelta = cyclesPerSample * 2.0 * juce::MathConstants<double>::pi;
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
        osc2AngleDelta = cyclesPerSample * 2.0 * juce::MathConstants<double>::pi;
    }
    else
    {
        // Sample rate not set yet - angle delta will be 0 (no sound)
        osc2AngleDelta = 0.0;
    }
}

//==============================================================================
// -- Filter Updates --

void SynthVoice::updateModFilter1()
{
    if (modFilter1Linked || sampleRate <= 0.0f) return;
    float clampedCutoff = juce::jlimit(20.0f, 20000.0f, modFilter1Cutoff);
    modFilter1.setMode(modFilter1Mode);
    modFilter1.setCutoffFrequency(clampedCutoff);
    modFilter1.setResonanceNormalized(modFilter1Resonance);
}

void SynthVoice::updateModFilter2()
{
    if (modFilter2Linked || sampleRate <= 0.0f) return;
    float clampedCutoff = juce::jlimit(20.0f, 20000.0f, modFilter2Cutoff);
    modFilter2.setMode(modFilter2Mode);
    modFilter2.setCutoffFrequency(clampedCutoff);
    modFilter2.setResonanceNormalized(modFilter2Resonance);
}

void SynthVoice::updateFilter()
{
    // Clamp cutoff to valid range
    float clampedCutoff = juce::jlimit(20.0f, 20000.0f, filterCutoff);

    // Resonance is passed normalized (0.0-1.0); NonlinearSVF owns the Q curve and
    // the self-oscillation region at the top of the knob.
    filter.setMode(juce::jlimit(0, 2, filterMode));
    filter.setCutoffFrequency(clampedCutoff);
    filter.setResonanceNormalized(filterResonance);
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
        float lowGainDb = lowShelfAmount * 24.0f; // ±24 dB range for dramatic effect
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
        float highGainDb = highShelfAmount * 24.0f; // ±24 dB range for dramatic effect
        *highShelfFilter.coefficients = *juce::dsp::IIR::Coefficients<float>::makeHighShelf(
            sampleRate, highShelfFreq, 0.707f, highGainDb);
    }
    else
    {
        // Zero: bypass (all-pass)
        *highShelfFilter.coefficients = *juce::dsp::IIR::Coefficients<float>::makeAllPass(sampleRate, 1.0f);
    }
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

    // Auditioning fix: do NOT push setParameters() while THIS voice's amp envelope
    // is in its release stage. juce::ADSR::setParameters() calls recalculateRates(),
    // which rewrites releaseRate = sustain/(release*sr); when sustain == 0 (common
    // for plucks/bass) that rate is 0 and recalculateRates() then forces the envelope
    // straight to idle (juce_ADSR.h: `state == release && releaseRate <= 0 ->
    // goToNextState()`), instantly cutting the release tail. Dragging the Release
    // knob pushes setParameters EVERY block, so every in-progress tail is killed and
    // the release sounds like 0 the whole time the mouse is held, snapping back only
    // on mouse-up when the pushes stop. Defer: leave lastAdsr* unchanged so the new
    // value is applied as soon as the voice leaves release (next note-on / when the
    // tail finishes). New notes pick up the live knob value, so you can audition.
    if (inReleasePhase)
        return;

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

    // Same release-stage deferral as the amp envelope (see updateAdsrParameters):
    // pushing setParameters() mid-release with sustain == 0 would let
    // recalculateRates() kill the filter-envelope tail, which jumps the cutoff to
    // its resting value — a click/zip while dragging the filter-env Release knob.
    // The amp + filter envelopes enter release together, so inReleasePhase gates both.
    if (inReleasePhase)
        return;

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
    // 6. Apply ADSR envelope (Attack → Decay → Sustain → Release)
    // 7. Process through multimode filter (LowPass/BandPass/HighPass)
    // 8. Write to output buffer (stereo)
    //
    // Real-time Safety: All operations are allocation-free and lock-free.
    
    // Early return if no note is playing (angle deltas are 0 when no note is active)
    if (osc1AngleDelta == 0.0 && osc2AngleDelta == 0.0)
        return;
    
    // Denormal prevention: FTZ/DAZ for oscillators, filters, envelopes (per JUCE best practice)
    juce::ScopedNoDenormals noDenormals;

    // SAFETY: a host may render a block LARGER than it declared to prepareToPlay
    // (Ableton does this during freeze/bounce/render). The per-voice scratch buffer
    // is sized to the prepared max; the per-sample writes and clear() below address
    // up to `numSamples`, so grow it if this block exceeds its capacity — otherwise
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
    // The played key is fixed for the whole block, so compute the cutoff shift once
    // here rather than per sample. keyTrackLogOffset is added in log-frequency space
    // for the master filter; keyTrackMultiplier (= e^offset = 2^((note-ref)/12)) scales
    // the unlinked mod-filter cutoffs. Each filter only applies these when its own
    // key-track flag is on. Default 0 / 1.0 = no change (full backward compatibility).
    const int keyTrackNote = currentlyPlayingNote.isValid()
                                 ? static_cast<int>(currentlyPlayingNote.initialNote)
                                 : kFilterKeyTrackRefNote;
    const float keyTrackLogOffset = static_cast<float>(keyTrackNote - kFilterKeyTrackRefNote)
                                        * kFilterKeyTrackLogPerSemi;
    const float keyTrackMultiplier = std::exp(keyTrackLogOffset);

    // Generate oscillator waveforms, mix, and apply envelope
    for (int i = 0; i < maxSamples; ++i)
    {
        // Glide (portamento): applied at END of each sample iteration so this sample's
        // pitch matches startNote/currentPitch; advancing here first made the first output
        // sample one glide step sharp/flat vs the requested transition (audible in mono/legato).

        //==============================================================================
        // -- PITCH ENVELOPE (separate from pitch bend) --
        // Computes base frequency from currentPitch + envelope. Independent of pitch bend.
        // Time is in seconds (0-5 from parameter). Linear ramp hits note at indicated time.
        double pitchForOscillators = currentPitch;
        if (pitchEnvTime >= 0.0001f && sampleRate > 0.0f && pitchEnvAmount != 0.0f && pitchEnvPitch != 0.0f)
        {
            float elapsedSec = pitchEnvSamplesElapsed / static_cast<float>(sampleRate);
            float T = pitchEnvTime;  // Seconds from parameter (0-10)
            float frac = juce::jmin(1.0f, elapsedSec / T);  // 0..1 over duration
            float curve = 1.0f - frac;  // Linear: 1 at start, 0 at end (hits note at indicated time)
            float pitchModSemitones = juce::jlimit(-48.0f, 48.0f, curve * (pitchEnvAmount / 100.0f) * pitchEnvPitch);
            double pitchEnvRatio = std::pow(2.0, static_cast<double>(pitchModSemitones) / 12.0);
            // Anchor pitch env to the intended note (targetPitch), not the glide-tracking
            // currentPitch. Rapid notes whose glide hasn't completed would otherwise produce
            // a meaningless attack of (mid-glide) × envRatio. Crossfade in Hz from the
            // env-shifted target back to currentPitch as the curve decays, so glide takes
            // over naturally once the env attack is past.
            const double envedTargetHz = targetPitch * pitchEnvRatio;
            const double curveD = static_cast<double>(curve);
            pitchForOscillators = envedTargetHz * curveD + currentPitch * (1.0 - curveD);
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
        // — it's already in semitones, already correctly weighted by the active
        // zone's per-note bend range (or the legacy-mode pitchbend range).  For a
        // Seaboard sending per-note pitch CC on its own channel this captures the
        // smooth glissando perfectly; for a regular keyboard sending master pitch
        // bend on channel 1, the legacy-mode bend range applies (48 semitones by
        // default — see SpaceDustSynthesiser).
        //
        // The manual UI bend slider is still useful for users who want a software
        // pitch bend independent of any hardware wheel.
        const float manualBendSt = juce::jlimit(-1.0f, 1.0f, pitchBend) * pitchBendAmountFloat;
        const double totalBendSt = mpeBendSemitones + static_cast<double>(manualBendSt);
        double bendRatio = std::pow(2.0, totalBendSt / 12.0);
        double osc1Freq = pitchForOscillators * bendRatio;
        double osc2Freq = pitchForOscillators * bendRatio;
        
        // Apply LFO modulation per-sample (if processor and buffers are available)
        float filterMod = 0.0f;           // Master filter modulation (only when Link to Master is ON)
        float modFilter1LfoMod = 0.0f;   // LFO1 modulation for modFilter1 (when Link OFF, LFO targets Filter)
        float modFilter2LfoMod = 0.0f;   // LFO2 modulation for modFilter2 (when Link OFF, LFO targets Filter)
        float osc1VolMod = 0.0f, osc2VolMod = 0.0f, noiseVolMod = 0.0f;  // Volume LFO modulation
        
        // Access LFO buffers per-sample (buffers are filled in processBlock for the entire block)
        if (processor != nullptr && 
            processor->lfo1Buffer.getNumSamples() > 0 && 
            processor->lfo2Buffer.getNumSamples() > 0 &&
            i < processor->lfo1Buffer.getNumSamples() && 
            i < processor->lfo2Buffer.getNumSamples())
        {
            float lfo1Val = processor->lfo1Buffer.getSample(0, i);
            float lfo2Val = processor->lfo2Buffer.getSample(0, i);
            
            // Use cached LFO targets (set per-block in updateVoicesWithParameters - avoids per-sample APVTS reads)
            const int lfo1Target = lfo1TargetCached;
            const int lfo2Target = lfo2TargetCached;
            
            // LFO1: 0=Pitch, 1=Filter, 2=MasterVol(processor), 3=Osc1, 4=Osc2, 5=Noise
            if (lfo1Target == 0)  // Pitch
            {
                float pitchModCents = lfo1Val * 1200.0f;  // ±24 semitones at full LFO swing
                osc1Freq *= std::pow(2.0, pitchModCents / 1200.0);
                osc2Freq *= std::pow(2.0, pitchModCents / 1200.0);
            }
            else if (lfo1Target == 1)  // Filter
            {
                if (modFilter1Linked)
                    filterMod += lfo1Val;  // Multiplicative: voice applies (1 + filterMod) to base
                else if (modFilter1Show)
                    modFilter1LfoMod = lfo1Val;  // Same for mod filter
            }
            else if (lfo1Target == 3) osc1VolMod += lfo1Val;
            else if (lfo1Target == 4) osc2VolMod += lfo1Val;
            else if (lfo1Target == 5) noiseVolMod += lfo1Val;
            
            // LFO2: same targets
            if (lfo2Target == 0)  // Pitch
            {
                float pitchModCents = lfo2Val * 1200.0f;  // ±24 semitones at full LFO swing
                osc1Freq *= std::pow(2.0, pitchModCents / 1200.0);
                osc2Freq *= std::pow(2.0, pitchModCents / 1200.0);
            }
            else if (lfo2Target == 1)  // Filter
            {
                if (modFilter2Linked)
                    filterMod += lfo2Val;  // Multiplicative: voice applies (1 + filterMod) to base
                else if (modFilter2Show)
                    modFilter2LfoMod = lfo2Val;  // Same for mod filter
            }
            else if (lfo2Target == 3) osc1VolMod += lfo2Val;
            else if (lfo2Target == 4) osc2VolMod += lfo2Val;
            else if (lfo2Target == 5) noiseVolMod += lfo2Val;
        }
        
        // Calculate oscillator frequencies with independent tuning and LFO modulation
        // Oscillator 1: base frequency + osc1 tuning + LFO modulation
        double osc1TotalSemitones = osc1CoarseTune + (osc1Detune / 100.0);
        osc1Freq = osc1Freq * std::pow(2.0, osc1TotalSemitones / 12.0);
        
        // Oscillator 2: base frequency + osc2 tuning + LFO modulation
        double osc2TotalSemitones = osc2CoarseTune + (osc2Detune / 100.0);
        osc2Freq = osc2Freq * std::pow(2.0, osc2TotalSemitones / 12.0);

        // Analog Drift: emulates hardware component tolerance and slow oscillator/filter drift
        if (analogDriftAmount > 0.0f)
        {
            const float a = analogDriftAmount;
            analogOscWalk += analogDriftWalkCoeff * ((random.nextFloat() * 2.0f - 1.0f) - analogOscWalk);
            const float walkCents = analogOscWalk * 1.25f * a;
            const float cents1 = osc1DriftOffset * 5.0f * a + walkCents;
            const float cents2 = osc2DriftOffset * 5.0f * a + walkCents;
            osc1Freq *= std::pow(2.0, static_cast<double>(cents1) / 1200.0);
            osc2Freq *= std::pow(2.0, static_cast<double>(cents2) / 1200.0);
        }
        
        // CRITICAL: Clamp frequencies to prevent runaway pitch on long holds/legato.
        // jlimit does not fix NaN/Inf — those would propagate into phase and blow up the output.
        osc1Freq = juce::jlimit(20.0, 20000.0, osc1Freq);
        osc2Freq = juce::jlimit(20.0, 20000.0, osc2Freq);
        if (!std::isfinite(osc1Freq) || !std::isfinite(osc2Freq))
        {
            reportDspSanitize(processor);
            // MPE: getCurrentlyPlayingNote() returns MPENote — use initialNote when valid.
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
        if (sampleRate > 0.0)
        {
            osc1AngleDelta = (osc1Freq / sampleRate) * 2.0 * juce::MathConstants<double>::pi;
            osc2AngleDelta = (osc2Freq / sampleRate) * 2.0 * juce::MathConstants<double>::pi;
            double baseFreq = osc1Freq / std::pow(2.0, (osc1CoarseTune + osc1Detune / 100.0) / 12.0);
            double subFreq = baseFreq * 0.5 * std::pow(2.0, static_cast<double>(subOscCoarse) / 12.0);
            subFreq = juce::jlimit(20.0, 20000.0, subFreq);
            subOscAngleDelta = (subFreq / sampleRate) * 2.0 * juce::MathConstants<double>::pi;
        }
        
        // Step 1-2: Generate oscillator waveforms
        float osc1Sample = generateWaveform(osc1Angle, osc1Waveform);
        float osc2Sample = generateWaveform(osc2Angle, osc2Waveform);
        float subOscSample = subOscOn ? generateWaveform(subOscAngle, subOscWaveform) * subOscLevel : 0.0f;
        
        // Step 3: Generate noise (white or pink)
        float noiseSample = 0.0f;
        
        if (noiseType == White)
        {
            noiseSample = random.nextFloat() * 2.0f - 1.0f;
        }
        else // Pink
        {
            // Voss-McCartney update (16 rows). The row index is the index of the lowest
            // set bit of a running counter. With only pinkState[0..15], that index must stay ≤15.
            // The old int counter could reach 65536 → bitPos 16 → out-of-bounds writes and
            // intermittent digital garbage (often bright/harsh) after ~1.3 s @ 48 kHz per voice.
            pinkNoiseCounter = (pinkNoiseCounter + 1u) & 0xFFFFu;
            std::uint32_t p = pinkNoiseCounter;
            if (p == 0u)
                p = 1u;
            const std::uint32_t lowestChangedBitU = p & static_cast<std::uint32_t>(-static_cast<std::int32_t>(p));
            int bitPos = 0;
            for (std::uint32_t t = lowestChangedBitU; (t >>= 1u) != 0u;)
                ++bitPos;
            bitPos = juce::jmin(bitPos, static_cast<int>(pinkState.size()) - 1);

            float newVal = (random.nextFloat() * 2.0f - 1.0f) * 0.0625f;
            pinkSum -= pinkState[bitPos];
            pinkSum += newVal;
            pinkState[bitPos] = newVal;
            
            noiseSample = pinkSum * 3.8f;  // scaling to roughly match white noise RMS level
        }
        
        // Apply noise EQ filters (low shelf and high shelf)
        // Process through low shelf first, then high shelf
        // Use a small temporary buffer for processing
        float filteredNoise = noiseSample;
        float* channelData[1] = { &filteredNoise };
        juce::dsp::AudioBlock<float> noiseBlock(channelData, 1, 1);
        juce::dsp::ProcessContextReplacing<float> noiseContext(noiseBlock);
        if (lowShelfAmount != 0.0f)
            lowShelfFilter.process(noiseContext);
        if (highShelfAmount != 0.0f)
            highShelfFilter.process(noiseContext);
        noiseSample = filteredNoise;
        
        // Step 4: Apply independent levels to each source (real-time safe: atomic parameter reads)
        // LFO volume modulation: multiply by (1 + mod) so depth scales 0-2x at full LFO swing
        float osc1Out = osc1Sample * osc1Level * juce::jlimit(0.0f, 2.0f, 1.0f + osc1VolMod);
        float osc2Out = osc2Sample * osc2Level * juce::jlimit(0.0f, 2.0f, 1.0f + osc2VolMod);
        float noiseOut = noiseSample * noiseLevel * 0.75f * juce::jlimit(0.0f, 2.0f, 1.0f + noiseVolMod);
        
        // Constant-power pan: -1 = full left, 0 = center, 1 = full right
        const float pi4 = static_cast<float>(juce::MathConstants<double>::pi / 4.0);
        float gainL1 = std::cos((osc1Pan + 1.0f) * pi4);
        float gainR1 = std::sin((osc1Pan + 1.0f) * pi4);
        float gainL2 = std::cos((osc2Pan + 1.0f) * pi4);
        float gainR2 = std::sin((osc2Pan + 1.0f) * pi4);
        const float centerGain = 0.70710678f;  // 1/sqrt(2) for centered sources
        
        // Step 5: Stereo mixing with per-oscillator pan
        float leftMix = osc1Out * gainL1 + osc2Out * gainL2 + noiseOut * centerGain + subOscSample * centerGain;
        float rightMix = osc1Out * gainR1 + osc2Out * gainR2 + noiseOut * centerGain + subOscSample * centerGain;
        
        // Step 6: Process envelopes (returns current amplitude value 0.0-1.0)
        // JUCE's ADSR handles all four stages automatically: Attack → Decay → Sustain → Release
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
        // -- MPE PRESSURE → AMPLITUDE MODULATION --
        // Pressure (Y-axis on a Seaboard / channel aftertouch on a normal keyboard) is
        // 0..1.  Map it to a smooth multiplicative amplitude boost (1.0 .. 2.0): at
        // rest the pressure is 0 → no change, at full pressure the envelope is
        // doubled.  This is real-time safe (single mul) and feels natural on Roli /
        // Sensel / Linnstrument controllers without any extra parameter wiring.
        //
        // For non-MPE controllers that don't send channel pressure, mpePressure01
        // stays at 0 → no effect, full backward compatibility.
        // mpePressureDepth (0..1) scales the modulation: 0 = pressure ignored, 1 = full boost.
        envelope *= juce::jlimit(0.0f, 2.0f, 1.0f + mpePressure01 * mpePressureDepth);
        
        // Modulate filter cutoff with filter envelope and LFO.
        // Amount blends unmodulated cutoff (0%) with full-range envelope sweep (100%):
        // E=1 → top of range, E=0 → bottom; sustain/decay set where E lands vs the knob.
        // Log-frequency space for perceptually even motion across octaves.
        const float logMin = std::log(20.0f);
        const float logMax = std::log(20000.0f);
        float filterBaseHz = baseFilterCutoff;
        if (analogDriftAmount > 0.0f)
        {
            analogFilterWalk += analogDriftWalkCoeff * ((random.nextFloat() * 2.0f - 1.0f) - analogFilterWalk);
            const float a = analogDriftAmount;
            const float driftHz = filterDriftOffset * 30.0f * a + analogFilterWalk * 10.0f * a;
            filterBaseHz = juce::jlimit(20.0f, 20000.0f, baseFilterCutoff + driftHz);
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
        // -- MPE TIMBRE → FILTER CUTOFF MODULATION --
        // Timbre (Z-axis / CC74 / Seaboard slide) is 0..1.  Centre (no slide) = 0.5.
        // We map (timbre - 0.5) * 2 → -1..+1 and treat it as a log-frequency offset of
        // up to ±2 octaves (4 octaves total range) on the filter cutoff.  Adding it
        // *after* the env+drift logKnob computation in log-space means it sweeps the
        // filter cleanly across octaves regardless of where the cutoff knob sits.
        //
        // For non-MPE controllers that don't send CC74, mpeTimbre01 stays at 0.5 →
        // zero offset, full backward compatibility.
        // mpeTimbreDepth (0..1) scales the modulation: 0 = slide ignored, 1 = full ±2 octaves.
        const float timbreBipolar = juce::jlimit(-1.0f, 1.0f, (mpeTimbre01 - 0.5f) * 2.0f) * mpeTimbreDepth;
        // ±2 octaves = ±ln(4) ≈ ±1.386 in natural-log units.
        const float timbreLogOffset = timbreBipolar * static_cast<float>(std::log(4.0));

        const float masterKeyTrackOffset = filterKeyTrack ? keyTrackLogOffset : 0.0f;
        float modulatedCutoff = std::exp(juce::jlimit(logMin, logMax, logModulated + timbreLogOffset + masterKeyTrackOffset)) * lfoFactor;
        modulatedCutoff = juce::jlimit(20.0f, 20000.0f, modulatedCutoff);

        // Apply normal cutoff smoothing, but use a much slower slew (extra damping)
        // for a short time after a poly steal. This directly mitigates the fast
        // cutoff movement from the restarting filter envelope that the user
        // confirmed triggers the click when the filter is active.
        if (snapFilterCutoffOnNote)
        {
            // First sample of a fresh note: jump the cutoff straight to the new
            // note's target so the resonant peak doesn't sweep into the note (the
            // note-on click). Click-safe — the amplitude envelope is ~0 here.
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
        // (Skip during voice fade — ADSR may hit zero before fade completes)
        if (!adsr.isActive() && voiceFadeSamplesRemaining <= 0)
        {
            // A self-oscillating filter rings under its own feedback, so its output
            // is NOT enveloped to zero. If it is still ringing when the amplitude
            // envelope completes here, a hard cut steps the output to zero and
            // clicks (worst on low notes — the residual sine sits far from a zero
            // crossing). Hand off to the existing voice-fade ramp, which declicks
            // the residual over kVoiceFadeLength and resets the filters when it
            // completes, instead of breaking now.
            const bool filterStillRinging =
                   filter.isRinging()
                || (modFilter1Show && !modFilter1Linked && modFilter1.isRinging())
                || (modFilter2Show && !modFilter2Linked && modFilter2.isRinging());

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
        float leftEnv = leftMix * envelope;
        float rightEnv = rightMix * envelope;

        // Step 7: Process through filter (per-sample so the self-oscillating SVF
        // tracks cutoff/key per sample and follows the amplitude envelope).
        float filtL = leftEnv;
        float filtR = rightEnv;

        // MASTER filter runs FIRST. When no mod filters are active it is the only
        // filter in the chain, so the master-only sound is bit-for-bit unchanged.
        // When an unlinked mod filter is active it now sits AFTER the master, making
        // the mod the last (dominant, untamed) voice — as powerful as the master,
        // instead of having its grit smoothed away by the master downstream.
        // (Master cutoff was already set above via smoothedFilterCutoffHz.)
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

        if (oversampleFilter)
        {
            // 4x: filter maths already run at the oversampled rate (rate-scale set
            // in setFilterOversample); the FIRs handle anti-imaging/anti-aliasing.
            filtL = masterFilterOS.process(0, filtL, [&](float s) noexcept { return masterNL(0, s); });
            filtR = masterFilterOS.process(1, filtR, [&](float s) noexcept { return masterNL(1, s); });
        }
        else
        {
            filtL = masterNL(0, filtL);   // host rate — bit-identical to before
            filtR = masterNL(1, filtR);
        }

        if (modFilter1Show && !modFilter1Linked)
        {
            float mod1LfoFactor = juce::jmax(0.0f, 1.0f + modFilter1LfoMod * 0.5f);
            float mod1KeyTrack = modFilter1KeyTrack ? keyTrackMultiplier : 1.0f;
            float modCutoff = juce::jlimit(20.0f, 20000.0f, modFilter1Cutoff * mod1LfoFactor * mod1KeyTrack);
            modFilter1.setCutoffFrequency(modCutoff);
            modFilter1.setEnvelope(envelope);
            auto mod1NL = [this](int ch, float s) noexcept -> float
            {
                float y = modFilter1.processSample(ch, s);
                if (warmSaturationMod1)
                    y = std::tanh(y * (1.0f + modFilter1Resonance * 2.0f));
                return juce::jlimit(-1.0f, 1.0f, y);
            };
            if (oversampleFilter)
            {
                filtL = modFilter1OS.process(0, filtL, [&](float s) noexcept { return mod1NL(0, s); });
                filtR = modFilter1OS.process(1, filtR, [&](float s) noexcept { return mod1NL(1, s); });
            }
            else
            {
                filtL = mod1NL(0, filtL);
                filtR = mod1NL(1, filtR);
            }
        }
        if (modFilter2Show && !modFilter2Linked)
        {
            float mod2LfoFactor = juce::jmax(0.0f, 1.0f + modFilter2LfoMod * 0.5f);
            float mod2KeyTrack = modFilter2KeyTrack ? keyTrackMultiplier : 1.0f;
            float modCutoff = juce::jlimit(20.0f, 20000.0f, modFilter2Cutoff * mod2LfoFactor * mod2KeyTrack);
            modFilter2.setCutoffFrequency(modCutoff);
            modFilter2.setEnvelope(envelope);
            auto mod2NL = [this](int ch, float s) noexcept -> float
            {
                float y = modFilter2.processSample(ch, s);
                if (warmSaturationMod2)
                    y = std::tanh(y * (1.0f + modFilter2Resonance * 2.0f));
                return juce::jlimit(-1.0f, 1.0f, y);
            };
            if (oversampleFilter)
            {
                filtL = modFilter2OS.process(0, filtL, [&](float s) noexcept { return mod2NL(0, s); });
                filtR = modFilter2OS.process(1, filtR, [&](float s) noexcept { return mod2NL(1, s); });
            }
            else
            {
                filtL = mod2NL(0, filtL);
                filtR = mod2NL(1, filtR);
            }
        }

        // Final safety clip (idempotent — each stage above already clipped).
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
                // Fade complete: full cleanup — voice is now silent
                voiceFade = 0.0f;
                adsr.reset();
                smoothedEnvelope = 0.0f;
                smoothedFilterEnvelope = 0.0f;
                filterAdsr.reset();
                filter.reset();
                modFilter1.reset();
                modFilter2.reset();
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
        // threshold is wrong: the max step of a clean sine is A·2π·f/sr, so a
        // loud (A>0.75) or high note legitimately steps far past any audible
        // threshold every cycle and floods the log with false positives.
        //
        // Instead we compare each step against the LOCAL slope envelope — an EMA
        // of recent |step| (kClickSlopeEmaCoeff, ~5 ms time constant).  A clean
        // waveform's step stays within ~1.6x of this average (sine max/mean of
        // |slope| = π/2); a real click is a single step many times larger.  We
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

        prevSmoothedL = outputSmootherL;
        prevSmoothedR = outputSmootherR;
        
        // Update oscillator phases for next sample using current angle deltas
        // (deltas are updated per-sample to support per-sample LFO modulation)
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
    
    // Re-seed Random with sample rate for additional uniqueness (already seeded in constructor with voice address)
    // This ensures each voice has different noise patterns even after sample rate changes
    random.setSeed(static_cast<juce::int64>(reinterpret_cast<uintptr_t>(this)) + static_cast<juce::int64>(sampleRate * 1000));
    
    // Prepare filter with sample rate (2 channels for stereo panning)
    const juce::uint32 maxBlockSize = static_cast<juce::uint32>(juce::jmax(4096, samplesPerBlock));
    filter.prepare({ sampleRate, maxBlockSize, 2 });
    modFilter1.prepare({ sampleRate, maxBlockSize, 2 });
    modFilter2.prepare({ sampleRate, maxBlockSize, 2 });

    // Filter oversamplers. Re-apply the current factor so each filter's rate-scale
    // matches after a sample-rate change.
    {
        const int osf = oversampleFilter ? kFilterOSFactor : 1;
        masterFilterOS.prepare(); masterFilterOS.setFactor(osf); filter.setSampleRateScale(osf);
        modFilter1OS.prepare();   modFilter1OS.setFactor(osf);   modFilter1.setSampleRateScale(osf);
        modFilter2OS.prepare();   modFilter2OS.setFactor(osf);   modFilter2.setSampleRateScale(osf);
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
    updateNoiseEqFilters();
    
    // Update filter parameters AFTER filter is prepared
    // This ensures filter is ready to accept parameter changes
    updateFilter();
    updateModFilter1();
    updateModFilter2();
    
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
    // ~10 s time constant for gentle analog-style wander (per sample)
    analogDriftWalkCoeff = 1.0f - std::exp(-1.0f / (10.0f * static_cast<float>(sampleRate)));
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
    // 1. Automatically when voices are added (with sampleRate=0) ← we must ignore!
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
        modFilter1.prepare({ newRate, maxBlockSize, 2 });
        modFilter2.prepare({ newRate, maxBlockSize, 2 });
        {
            const int osf = oversampleFilter ? kFilterOSFactor : 1;
            masterFilterOS.prepare(); masterFilterOS.setFactor(osf); filter.setSampleRateScale(osf);
            modFilter1OS.prepare();   modFilter1OS.setFactor(osf);   modFilter1.setSampleRateScale(osf);
            modFilter2OS.prepare();   modFilter2OS.setFactor(osf);   modFilter2.setSampleRateScale(osf);
        }
        updateFilter();
        updateModFilter1();
        updateModFilter2();
        juce::dsp::ProcessSpec eqSpec{ newRate, maxBlockSize, 1 };
        lowShelfFilter.prepare(eqSpec);
        highShelfFilter.prepare(eqSpec);
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
        analogDriftWalkCoeff = 1.0f - std::exp(-1.0f / (10.0f * static_cast<float>(newRate)));
    }
    // If DSP not initialized yet, prepareToPlay() will handle initialization
}

//==============================================================================
// -- Parameter Update Methods --

void SynthVoice::setOsc1Waveform(int waveform)
{
    osc1Waveform = juce::jlimit(0, 3, waveform);
}

void SynthVoice::setOsc2Waveform(int waveform)
{
    osc2Waveform = juce::jlimit(0, 3, waveform);
}

//==============================================================================
// -- Oscillator Pitch Tuning Methods --
// Each oscillator has independent coarse tuning (±24 semitones) and fine detuning (±50 cents)
// Double-click any knob to reset to 0

void SynthVoice::setOsc1CoarseTune(float semitones)
{
    osc1CoarseTune = juce::jlimit(-24.0f, 24.0f, semitones);
    // Update frequency if note is playing — base Hz must match renderNextBlock's currentPitch
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
    noiseType = (type == 0) ? White : Pink;
}

void SynthVoice::setSubOscOn(bool on)
{
    subOscOn = on;
}

void SynthVoice::setSubOscWaveform(int waveform)
{
    subOscWaveform = juce::jlimit(0, 3, waveform);
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
    filterMode = juce::jlimit(0, 2, mode);
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

void SynthVoice::setFilterOversample(bool enabled)
{
    if (enabled == oversampleFilter)
        return;
    oversampleFilter = enabled;
    const int factor = enabled ? kFilterOSFactor : 1;
    // Apply to all three filter stages. setFactor clears the FIR state and
    // setSampleRateScale recomputes g (and the rate-compensated AGC) at the
    // (over)sampled rate; reset avoids a transient on the switch.
    masterFilterOS.setFactor(factor);  filter.setSampleRateScale(factor);     filter.reset();
    modFilter1OS.setFactor(factor);    modFilter1.setSampleRateScale(factor); modFilter1.reset();
    modFilter2OS.setFactor(factor);    modFilter2.setSampleRateScale(factor); modFilter2.reset();
}

void SynthVoice::setFilterKeyTrack(bool enabled)
{
    // No filter rebuild needed: the offset is applied per-block in renderNextBlock,
    // where the played note number is known.
    filterKeyTrack = enabled;
}

void SynthVoice::setModFilter1(bool show, bool linkToMaster, int mode, float cutoffHz, float resonance)
{
    modFilter1Show = show;
    modFilter1Linked = linkToMaster;
    modFilter1Mode = juce::jlimit(0, 2, mode);
    modFilter1Cutoff = juce::jlimit(20.0f, 20000.0f, cutoffHz);
    modFilter1Resonance = juce::jlimit(0.0f, 1.0f, resonance);
    updateModFilter1();
}

void SynthVoice::setWarmSaturationMod1(bool enabled)
{
    warmSaturationMod1 = enabled;
}

void SynthVoice::setModFilter1KeyTrack(bool enabled)
{
    modFilter1KeyTrack = enabled;
}

void SynthVoice::setModFilter2(bool show, bool linkToMaster, int mode, float cutoffHz, float resonance)
{
    modFilter2Show = show;
    modFilter2Linked = linkToMaster;
    modFilter2Mode = juce::jlimit(0, 2, mode);
    modFilter2Cutoff = juce::jlimit(20.0f, 20000.0f, cutoffHz);
    modFilter2Resonance = juce::jlimit(0.0f, 1.0f, resonance);
    updateModFilter2();
}

void SynthVoice::setWarmSaturationMod2(bool enabled)
{
    warmSaturationMod2 = enabled;
}

void SynthVoice::setModFilter2KeyTrack(bool enabled)
{
    modFilter2KeyTrack = enabled;
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
    
    // If glide is active, recalculate glideDelta based on new time
    // This allows real-time adjustment of glide speed during playback
    if (glideDelta != 0.0 && sampleRate > 0.0)
    {
        double pitchDifference = targetPitch - currentPitch;
        double samplesToGlide = glideTimeSeconds * sampleRate;
        
        if (samplesToGlide > 0.0)
        {
            glideDelta = pitchDifference / samplesToGlide;
        }
        else
        {
            glideDelta = 0.0; // Instant if time is too short
        }
    }
}

void SynthVoice::setLegatoGlide(bool enabled)
{
    legatoGlideEnabled = enabled;
}

void SynthVoice::setPitchEnvAmount(float amount)
{
    pitchEnvAmount = juce::jlimit(-100.0f, 100.0f, amount);
}

void SynthVoice::setPitchEnvTime(float seconds)
{
    pitchEnvTime = juce::jlimit(0.0f, 10.0f, seconds);
}

void SynthVoice::setPitchEnvPitch(float semitones)
{
    pitchEnvPitch = juce::jlimit(0.0f, 24.0f, semitones);
}

void SynthVoice::setPitchBendAmount(float semitones)
{
    pitchBendAmountFloat = juce::jlimit(0.0f, 24.0f, semitones);
}

void SynthVoice::setPitchBend(float value)
{
    pitchBend = juce::jlimit(-1.0f, 1.0f, value);
}

void SynthVoice::setLfoTargets(int lfo1Target, int lfo2Target)
{
    lfo1TargetCached = juce::jlimit(0, 5, lfo1Target);  // 0=Pitch, 1=Filter, 2=MasterVol, 3=Osc1, 4=Osc2, 5=Noise
    lfo2TargetCached = juce::jlimit(0, 5, lfo2Target);
}

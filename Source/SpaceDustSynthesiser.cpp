#include "SpaceDustSynthesiser.h"
#include "MemorySafetyLogger.h"
#include <juce_core/juce_core.h>
#include <vector>

//==============================================================================
// -- UTF-8 String Validation Helper --
namespace
{
    // Safe string creation from number with UTF-8 validation
    juce::String safeStringFromNumber(double value, int numDecimalPlaces = 2)
    {
        // String from number is always valid UTF-8 (numbers are ASCII)
        return juce::String(value, numDecimalPlaces);
    }
}

//==============================================================================
int SpaceDustSynthesiser::getVoiceModeIndex() const
{
    auto* p = parameters.getParameter("voiceMode");
    auto* c = dynamic_cast<juce::AudioParameterChoice*>(p);
    return c ? c->getIndex() : 0;
}

double SpaceDustSynthesiser::getMaxCurrentPitch() const
{
    double mx = 0.0;
    // MPESynthesiser stores its voices in the same `voices` array; iterate via getVoice().
    for (int i = 0; i < getNumVoices(); ++i)
    {
        if (auto* v = dynamic_cast<SynthVoice*>(getVoice(i)))
        {
            double p = v->getCurrentPitch();
            // Only consider valid pitches (above 20 Hz) to avoid using invalid/very low values
            if (p > 20.0 && p > mx) mx = p;
        }
    }
    return mx;  // Returns 0.0 if no valid pitches found (will fall back to target pitch in voice)
}

//==============================================================================
// -- MPE noteAdded (replaces juce::Synthesiser::noteOn override) --
//
// MPESynthesiser drives voice allocation through this callback (called from
// MPEInstrument when a note-on arrives).  In Mono/Legato modes we forcibly
// reuse the same voice (lastMonoVoiceIndex) to prevent click-prone voice
// allocation switching across the note transition.
//
// In Poly mode we delegate to the base implementation, which uses
// findFreeVoice() + startVoice() (and findVoiceToSteal() when stealing).
void SpaceDustSynthesiser::noteAdded(juce::MPENote newNote)
{
    const int mode = getVoiceModeIndex();

    // Memory-safety logger: capture every note-allocation decision so we can
    // tell mono-reuse from fresh polyphonic allocation when scanning logs.
    SAFETY_LOG_VOICE_NOTE(newNote.noteID, this, (int) newNote.initialNote,
                          (float) newNote.getFrequencyInHertz(),
                          mode == 0 ? "noteAdded POLY"
                                    : (mode == 1 ? "noteAdded MONO" : "noteAdded LEGATO"));

    // Mono & Legato: reuse the same voice to prevent click from voice stealing.
    // noteStopped preserves ADSR/filter/oscillator state when isPreservingVoice()
    // or isNextNoteLegato() returns true, and noteStarted either retriggers ADSR
    // (mono) or continues the envelope (legato) — without resetting oscillator
    // phases, so sine/triangle waveforms remain continuous.
    if (mode != 0 && lastMonoVoiceIndex >= 0 && lastMonoVoiceIndex < getNumVoices())
    {
        if (auto* voice = getVoice(lastMonoVoiceIndex))
        {
            SAFETY_LOG_VOICE(newNote.noteID, voice,
                             "voice REUSE (mono/legato handoff)");
            nextNotePreservesVoice.store(true);

            // MPESynthesiser::startVoice() simply assigns the new MPENote to the
            // voice (voice->currentlyPlayingNote = newNote) and then calls
            // voice->noteStarted().  It does NOT call noteStopped() on the
            // previous note, so there is no hard stop to guard against (unlike
            // juce::Synthesiser::startVoice).  This is exactly what we want for
            // mono/legato handoff.
            startVoice(voice, newNote);

            nextNotePreservesVoice.store(false);

            // Single-voice guarantee: kill any other voice still ringing out a
            // long release so Mono/Legato never sounds two notes at once.
            cutStrayVoices(voice);
            return;
        }
    }

    // Default JUCE MPE voice allocation (Poly mode or mono fallback when
    // lastMonoVoiceIndex is invalid).
    juce::MPESynthesiser::noteAdded(newNote);

    // Track which voice was allocated so subsequent mono/legato note transitions
    // can reuse it.  isCurrentlyPlayingNote() compares by noteID so this is exact.
    if (mode != 0)
    {
        juce::MPESynthesiserVoice* allocated = nullptr;
        for (int i = 0; i < getNumVoices(); ++i)
        {
            if (auto* v = getVoice(i))
            {
                if (v->isCurrentlyPlayingNote(newNote))
                {
                    lastMonoVoiceIndex = i;
                    allocated = v;
                    break;
                }
            }
        }

        // Mono/Legato: a "first" note (lastMonoVoiceIndex was invalid) may be
        // allocated to a FREE voice while an earlier note is still releasing on
        // another voice — that stray long release is exactly the "first note's
        // release still rings out" bug.  Cut every voice except the new one.
        if (allocated != nullptr)
            cutStrayVoices(allocated);
    }
}

//==============================================================================
void SpaceDustSynthesiser::cutStrayVoices(juce::MPESynthesiserVoice* keep)
{
    for (int i = 0; i < getNumVoices(); ++i)
    {
        auto* v = getVoice(i);
        if (v == nullptr || v == keep)
            continue;
        if (auto* sv = dynamic_cast<SynthVoice*>(v))
            sv->forceFadeOut();   // click-safe; ignores legato/preserve handoff
    }
}

//==============================================================================
void SpaceDustSynthesiser::setMpeZoneLayoutLower(int memberChannels,
                                                int notePitchBendRange,
                                                int masterPitchBendRange)
{
    // Build a Lower-Zone layout: ch1 master + memberChannels (default 14) member channels.
    // setLowerZone signature: (numMemberChannels, perNotePitchbendRange, masterPitchbendRange).
    juce::MPEZoneLayout layout;
    layout.setLowerZone(juce::jlimit(0, 15, memberChannels),
                        juce::jlimit(0, 96, notePitchBendRange),
                        juce::jlimit(0, 96, masterPitchBendRange));

    // setZoneLayout() implicitly disables legacy mode and releases currently-playing
    // notes (mutates the MPE note array). MUST be called on the audio thread only —
    // the processor defers MPE reconfig to applyPendingMpeReconfig() at the top of
    // processBlock so this never races renderNextBlock.
    setZoneLayout(layout);
}

void SpaceDustSynthesiser::setLegacyModeWithPitchBendRange(int semitones)
{
    enableLegacyMode(juce::jlimit(0, 96, semitones), juce::Range<int>(1, 17));
}

//==============================================================================
void SpaceDustSynthesiser::processMidiBuffer(juce::MidiBuffer& midiMessages, int numSamples)
{
    juce::ignoreUnused(numSamples);
    const int mode = getVoiceModeIndex();

    if (mode == 0)
    {
        // Poly mode: pass MIDI through unchanged for full polyphony (chords, etc.)
        // MPESynthesiser will pick up the messages itself in renderNextBlock and
        // route them to MPEInstrument — including any MPE-specific pressure / CC74
        // / pitch-bend per-note expression coming from an MPE controller.
        for (const auto metadata : midiMessages)
        {
            auto message = metadata.getMessage();
            if (message.isNoteOn())
                activeNoteCount.fetch_add(1);
            else if (message.isNoteOff())
            {
                int c = activeNoteCount.load();
                if (c > 0) activeNoteCount.store(c - 1);
            }
        }
        return;
    }

    //==============================================================================
    // Mono (1) and Legato (2) modes:
    //
    // Implement a simple *monophonic, last-note priority* note stack with proper legato
    // behaviour.  This is custom non-MPE logic that runs BEFORE MPESynthesiser sees the
    // MIDI buffer — the rewritten note-on / note-off messages are then handed to MPE
    // as if they were normal MIDI, and MPESynthesiser's noteAdded / noteReleased route
    // them to our SynthVoice instances via our overridden noteAdded() (which forces
    // voice reuse via lastMonoVoiceIndex).
    //
    //  - Note stack keeps track of all currently held notes (oldest → newest).
    //  - currentNote is always the last (most recent) note in the stack.
    //  - In Mono mode (mode == 1):
    //      * Every change of currentNote retriggers the envelope (no legato).
    //  - In Legato mode (mode == 2):
    //      * First note: full ADSR attack.
    //      * Overlapping note-on: currentNote switches to the newest note WITHOUT
    //        retriggering ADSR (single-trigger). Pitch changes immediately or with
    //        glide depending on Glide/Legato Glide.
    //      * Releasing the newest note while others are still held returns to the
    //        previous note with the SAME legato behaviour (no ADSR retrigger).
    //
    juce::MidiBuffer out;
    bool addedNoteOnThisBuffer = false;  // True if we've already added a note-on in this buffer

    for (const auto metadata : midiMessages)
    {
        auto message = metadata.getMessage();
        const int pos = metadata.samplePosition;

        if (message.isNoteOn())
        {
            const int newNote = message.getNoteNumber();
            const int channel = message.getChannel();

            // Remove any existing instance of this note in the stack, then push as most recent.
            noteStack.erase(std::remove(noteStack.begin(), noteStack.end(), newNote), noteStack.end());
            const int previousNote = currentNote;

            noteStack.push_back(newNote);
            currentNote = newNote;
            activeNoteCount.store((int)noteStack.size());

            if (previousNote < 0)
            {
                // First note: full ADSR trigger in both Mono and Legato modes.
                nextNoteIsLegato.store(false);
                out.addEvent(message, pos);
                addedNoteOnThisBuffer = true;
            }
            else if (mode == 1)
            {
                // Mono: just send the noteOn.  noteAdded() reuses the same voice
                // (no voice stealing). Envelope is HARD retriggered in noteStarted
                // when voiceMode==1.
                //
                // We mark nextNoteIsLegato=true so the Legato Glide toggle can
                // gate glide in mono too: with Legato Glide ON, glide only happens
                // on overlapping notes; with it OFF, glide happens every note.
                // (We're inside the previousNote >= 0 branch, so this IS an overlap.)
                juce::MidiBuffer filtered;
                for (const auto meta : out)
                {
                    auto m = meta.getMessage();
                    if (!(m.isNoteOn() && m.getChannel() == channel))
                        filtered.addEvent(m, meta.samplePosition);
                }
                out.swapWith(filtered);
                nextNoteIsLegato.store(true);
                out.addEvent(message, pos);
            }
            else // mode == 2 (Legato)
            {
                if (addedNoteOnThisBuffer)
                {
                    // Two note-ons in SAME buffer = simultaneous key press (accidental double-hit).
                    // Use hard cut to prevent stuck notes from legato handoff confusion.
                    // turnOffAllVoices replaces juce::Synthesiser::allNotesOff for MPE.
                    turnOffAllVoices(false);
                    nextNoteIsLegato.store(false);
                    out.clear();
                    out.addEvent(message, pos);
                }
                else
                {
                    // Overlapping from previous buffer: proper legato (hold one key, press another).
                    nextNoteIsLegato.store(true);
                    out.addEvent(juce::MidiMessage::noteOff(channel, previousNote), pos);
                    out.addEvent(message, pos);
                }
                addedNoteOnThisBuffer = true;
            }
        }
        else if (message.isNoteOff())
        {
            const int releasedNote = message.getNoteNumber();
            const int channel = message.getChannel();

            const bool wasTop = (!noteStack.empty() && releasedNote == currentNote);

            noteStack.erase(std::remove(noteStack.begin(), noteStack.end(), releasedNote), noteStack.end());
            activeNoteCount.store((int)noteStack.size());

            if (!wasTop)
            {
                // Releasing a background note that is not currently sounding:
                // nothing changes audibly; the old top is still active.
                continue;
            }

            // The currently sounding note was released.
            const int newTop = noteStack.empty() ? -1 : noteStack.back();
            currentNote = newTop;

            if (newTop < 0)
            {
                // No notes left: release the envelope normally.
                nextNoteIsLegato.store(false);
                out.addEvent(message, pos);
            }
            else if (mode == 1)
            {
                // Mono: return to previous held note with full ADSR retrigger.
                // Mark as legato (overlap) so the Legato Glide toggle can gate
                // glide; envelope still hard-retriggers because voiceMode==1.
                nextNoteIsLegato.store(true);
                out.addEvent(juce::MidiMessage::noteOn(channel, newTop, (juce::uint8)127), pos);
            }
            else // mode == 2 (Legato)
            {
                // Legato: return to the previous held note with legato behaviour:
                // no ADSR retrigger, just pitch change (with optional glide).
                nextNoteIsLegato.store(true);
                out.addEvent(message, pos); // note-off for the old top
                out.addEvent(juce::MidiMessage::noteOn(channel, newTop, (juce::uint8)127), pos);
            }
        }
        else
        {
            // Pass through non-note events unchanged (pitch bend, CC, channel pressure,
            // CC74 timbre, etc.).  Critical: MPE expression messages MUST flow through
            // to MPEInstrument so per-note pressure/timbre/pitchbend continue to work
            // even in Mono/Legato modes.
            out.addEvent(message, pos);
        }
    }

    // JUCE MPESynthesiser::renderNextBlock chops the audio buffer at MIDI event
    // boundaries.  Note-on/note-off events with samplePosition > 0 cause the voice
    // to render the previous pitch until that offset.  Mono/legato stack rewriting
    // often inherits the host's non-zero position; force note-on/note-off to
    // sample 0 so pitch updates apply before any audio in the block.
    {
        juce::MidiBuffer coalesced;
        for (const auto metadata : out)
        {
            auto m = metadata.getMessage();
            int p = metadata.samplePosition;
            if (m.isNoteOn() || m.isNoteOff())
                p = 0;
            coalesced.addEvent(m, p);
        }
        out.swapWith(coalesced);
    }

    midiMessages.swapWith(out);
}

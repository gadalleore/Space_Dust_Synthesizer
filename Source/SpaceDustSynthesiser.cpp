#include "SpaceDustSynthesiser.h"
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
void SpaceDustSynthesiser::processMidiBuffer(juce::MidiBuffer& midiMessages, int numSamples)
{
    juce::ignoreUnused(numSamples);
    const int mode = getVoiceModeIndex();

    if (mode == 0)
    {
        // Poly mode: pass MIDI through unchanged for full polyphony (chords, etc.)
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
    // behaviour:
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
    // We achieve this by rewriting the MIDI buffer:
    //  - For legato transitions we set nextNoteIsLegato=true, send a note-off for
    //    the old note, then a note-on for the new note. SynthVoice::startNote/stopNote
    //    read this flag to keep the envelope running and only update pitch.
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

            //==========================================================================
            // Mono/Legato non-overlapping case:
            // If no keys are currently held (empty noteStack), but there may still be
            // voices in their release phase from a previous note, we want TRUE mono
            // behaviour: the new note should immediately steal any lingering tails
            // instead of layering release over the new attack (poly-like behaviour).
            //
            // To achieve this, we forcibly stop all voices *without* tail-off before
            // starting the new note. This guarantees only one voice/envelope is ever
            // active at a time for non-legato transitions.
            //
            // CRITICAL: This must happen BEFORE we add the new note to the stack, so
            // that any voices currently in release phase are immediately cut off.
            // This ensures non-overlapping notes don't stack release tails.
            if (noteStack.empty())
            {
                // Hard-cut any existing tails (no tail-off) to emulate analog mono synth
                // "voice stealing". ADSR will start cleanly from zero for the new note.
                // allNotesOff with allowTailOff=false immediately stops all voices without
                // allowing release tails to continue, preventing polyphonic stacking.
                allNotesOff(channel, false);
            }

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
                // Mono: retrigger ADSR on every new note (no legato).
                // CRITICAL: In true mono mode, only one note should play at a time.
                // Remove any note-ons we've already added this buffer (e.g. chord pressed)
                // so the synth only receives the LAST note - no chords in mono!
                juce::MidiBuffer filtered;
                for (const auto meta : out)
                {
                    auto m = meta.getMessage();
                    if (!(m.isNoteOn() && m.getChannel() == channel))
                        filtered.addEvent(m, meta.samplePosition);
                }
                out.swapWith(filtered);
                allNotesOff(channel, false);
                nextNoteIsLegato.store(false);
                out.addEvent(message, pos);
            }
            else // mode == 2 (Legato)
            {
                if (addedNoteOnThisBuffer)
                {
                    // Two note-ons in SAME buffer = simultaneous key press (accidental double-hit).
                    // Use hard cut to prevent stuck notes from legato handoff confusion.
                    allNotesOff(channel, false);
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

            // Remove the released note from the stack.
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
                out.addEvent(message, pos); // pass through the original note-off
            }
            else if (mode == 1)
            {
                // Mono: when the top note is released but others remain, treat the
                // return to the previous note as a new note (full ADSR retrigger).
                nextNoteIsLegato.store(false);
                out.addEvent(message, pos); // note-off for the old top
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
            // Pass through non-note events unchanged (pitch bend, CC, etc.)
            out.addEvent(message, pos);
        }
    }

    midiMessages.swapWith(out);
}









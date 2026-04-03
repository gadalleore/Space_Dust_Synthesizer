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
void SpaceDustSynthesiser::noteOn(int midiChannel, int midiNoteNumber, float velocity)
{
    const int mode = getVoiceModeIndex();

    // Mono & Legato: always reuse the same voice to prevent click from voice stealing.
    // stopNote preserves ADSR/filter/oscillator state; startNote either retriggers
    // ADSR (mono) or continues the envelope (legato), without resetting oscillator
    // phases — keeping sine/triangle waveforms continuous.
    if (mode != 0 && lastMonoVoiceIndex >= 0 && lastMonoVoiceIndex < getNumVoices())
    {
        auto* voice = getVoice(lastMonoVoiceIndex);
        if (voice != nullptr)
        {
            nextNotePreservesVoice.store(true);
            for (int s = 0; s < getNumSounds(); ++s)
            {
                auto sound = getSound(s);
                if (sound != nullptr
                    && sound->appliesToNote(midiNoteNumber)
                    && sound->appliesToChannel(midiChannel))
                {
                    startVoice(voice, sound.get(), midiChannel, midiNoteNumber, velocity);
                    nextNotePreservesVoice.store(false); // CRITICAL: clear after use
                    return;
                }
            }
            nextNotePreservesVoice.store(false); // clear if no matching sound found
        }
    }

    // Default JUCE voice allocation (poly, mono fresh voice, or legato fallback).
    juce::Synthesiser::noteOn(midiChannel, midiNoteNumber, velocity);

    // Track which voice was allocated for legato voice reuse.
    if (mode != 0)
    {
        for (int i = 0; i < getNumVoices(); ++i)
        {
            if (getVoice(i)->getCurrentlyPlayingNote() == midiNoteNumber
                && getVoice(i)->isPlayingChannel(midiChannel))
            {
                lastMonoVoiceIndex = i;
                break;
            }
        }
    }
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
            // If no keys are currently held (empty noteStack), any voices still
            // sounding are in their natural ADSR release phase.  We let them fade
            // out naturally rather than hard-cutting with allNotesOff (which causes
            // an audible pop by jumping the amplitude to 0 instantly).
            // The new note will start on a free voice; the brief overlap of the
            // release tail and the new attack is standard analog mono-synth
            // behaviour and sounds clean.  If all 8 voices are busy (unlikely),
            // JUCE voice-stealing reuses the oldest/quietest voice automatically.

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
                // Mono: just send the noteOn. The noteOn override reuses the same
                // voice (no voice stealing), and startNote retriggers ADSR from its
                // current value without resetting oscillator phases.
                juce::MidiBuffer filtered;
                for (const auto meta : out)
                {
                    auto m = meta.getMessage();
                    if (!(m.isNoteOn() && m.getChannel() == channel))
                        filtered.addEvent(m, meta.samplePosition);
                }
                out.swapWith(filtered);
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
                // Mono: return to previous held note with full ADSR retrigger.
                // Same voice is reused via noteOn override.
                nextNoteIsLegato.store(false);
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

    // JUCE Synthesiser::processNextBlock renders samples *before* each MIDI event when
    // samplePosition > 0, so the voice keeps the previous pitch until that offset.
    // Mono/legato stack rewriting often inherits the host's non-zero position; force
    // note-on/note-off to sample 0 so pitch updates apply before any audio in the block.
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









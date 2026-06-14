#pragma once

// MPE (MIDI Polyphonic Expression) support
// - juce_audio_basics provides juce::MPESynthesiser, juce::MPESynthesiserVoice,
//   juce::MPENote, juce::MPEValue, juce::MPEZoneLayout, juce::MPEInstrument
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include "SynthVoice.h"
#include "SynthSound.h"   // Kept for backwards-compat / preset loading; MPE doesn't use SynthesiserSound
#include <atomic>
#include <vector>

//==============================================================================
/**
    SpaceDust Custom Synthesiser  (MPE-aware)

    Inherits from juce::MPESynthesiser (previously juce::Synthesiser) so that the
    plugin works correctly with MPE controllers (ROLI Seaboard, LinnStrument,
    Sensel, Linnstrument, Roli Lightpad Block, etc.) while staying 100% backwards
    compatible with regular single-channel MIDI keyboards.

    MPE compatibility strategy:
    - By default we enable LEGACY MODE with a 48-semitone pitch-bend range
      (the de-facto MPE standard).  Legacy mode means:
        * All MIDI channels 1..16 are accepted as note channels.
        * MPESynthesiser still tracks per-note pitch-bend / pressure / timbre,
          so a Seaboard sending per-channel CC/PB/AT works perfectly.
        * A non-MPE keyboard transmitting on a single channel works exactly
          as before — pressure / timbre simply stay at their centre values.
    - The user can switch to a real Lower-Zone MPE layout at runtime by
      calling setMpeZoneLayoutLower() (see implementation).

    Voice modes (unchanged user-facing behaviour):
    - Poly:   multiple notes, envelope retriggers each note.
    - Mono:   one note at a time, envelope retriggers each note.
    - Legato: one note at a time; on overlapping note-on, no envelope retrigger
              (smooth pitch glide).  Mono/Legato are implemented by rewriting
              the MIDI buffer in processMidiBuffer() before MPE handles it; we
              additionally override noteAdded() to force voice reuse so the
              same MPESynthesiserVoice instance keeps playing across the
              note-off → note-on transition (preserving phase, ADSR, filter).
*/
class SpaceDustSynthesiser : public juce::MPESynthesiser
{
public:
    SpaceDustSynthesiser(juce::AudioProcessorValueTreeState& apvts)
        : parameters(apvts)
    {
        // Enable broad-compatibility MPE legacy mode by default.
        // - 48 semitones pitch-bend range is the MPE de-facto standard (ROLI/LinnStrument/Sensel).
        // - Channel range 1..16 lets a Seaboard's per-note channels through unchanged
        //   AND lets a normal keyboard (channel 1 only) work like before.
        enableLegacyMode(48, juce::Range<int>(1, 17));

        // We rely on JUCE's built-in voice stealing for Poly mode; mono/legato
        // re-uses the same voice via our noteAdded() override.
        setVoiceStealingEnabled(true);
    }

    juce::AudioProcessorValueTreeState& getParameters() { return parameters; }

    // Legato flag for the next note (set when Legato mode + overlapping note-on). Cleared when read.
    bool getAndClearNextNoteLegato() { return nextNoteIsLegato.exchange(false); }
    // Non-clearing peek (used by voice in noteStopped to detect legato handoff).
    bool isNextNoteLegato() const { return nextNoteIsLegato.load(); }

    // Voice-preservation flag: set when noteAdded() reuses the same voice for mono/legato.
    // Tells the voice's noteStopped() to skip ADSR/filter/phase reset so state is preserved
    // across the noteStopped → noteStarted transition done inside startVoice.
    bool isPreservingVoice() const { return nextNotePreservesVoice.load(); }

    // voiceMode: 0=Poly, 1=Mono, 2=Legato. Use getIndex() so Choice is read correctly.
    int getVoiceModeIndex() const;

    // Max currentPitch across all voices (for Poly glide when the new note uses a different voice).
    double getMaxCurrentPitch() const;

    //==============================================================================
    // MPE: replacement for juce::Synthesiser::noteOn override.
    //
    // juce::MPESynthesiser drives voices via the MPEInstrument::Listener callbacks
    // (noteAdded / noteReleased / notePressureChanged / notePitchbendChanged /
    // noteTimbreChanged / noteKeyStateChanged).  noteAdded() is the equivalent of
    // a note-on in the regular Synthesiser; we override it so that in Mono and
    // Legato voice modes we forcibly reuse the same voice (lastMonoVoiceIndex)
    // for click-free note transitions.
    void noteAdded(juce::MPENote newNote) override;

    // Process MIDI buffer with mono/legato handling and active-note count
    void processMidiBuffer(juce::MidiBuffer& midiMessages, int numSamples);

    // Mono/Legato single-voice guarantee: fade out every active voice EXCEPT
    // `keep` so a long release left over from a previous note (or a poly→mono
    // switch) cannot keep ringing under the new note.  No-op in Poly mode.
    void cutStrayVoices(juce::MPESynthesiserVoice* keep);

    /** Flush all mono/legato note-tracking state.

        MUST be called whenever the host transport stops or the playhead jumps
        (loop wrap / seek), and from prepareToPlay()/releaseResources().  The
        mono/legato note stack models "which keys does the host think are held";
        if it is never reset, a loop or transport stop leaves phantom held notes
        in the stack, and processMidiBuffer() then rewrites the MIDI stream with
        spurious note-on/note-off pairs → wrong notes, stuck notes and voice-steal
        thrashing.  vector::clear() keeps the capacity, so this is allocation-free
        and safe to call from the audio thread.

        @param keepMonoVoiceIndex  When true, the note STACK is flushed but
               lastMonoVoiceIndex is preserved. Use this at a loop-wrap / transport
               edge: the stack must be cleared (so processMidiBuffer can't emit
               phantom notes), but the new loop's first note should still REUSE the
               voice that is ringing out the previous note's release — a smooth mono
               retrigger — instead of being allocated a fresh voice while the old
               voice gets hard-faded by cutStrayVoices() (an audible POP on loop
               restart, worst in FL's tiny buffers). prepareToPlay / releaseResources
               / setStateInformation want a FULL reset, so they leave this false. */
    void resetNoteState(bool keepMonoVoiceIndex = false)
    {
        noteStack.clear();
        currentNote = -1;
        if (! keepMonoVoiceIndex)
            lastMonoVoiceIndex = -1;
        activeNoteCount.store(0);
        nextNoteIsLegato.store(false);
        nextNotePreservesVoice.store(false);
    }

    //==============================================================================
    // MPE convenience helpers (optional UI extension hooks).

    /** Switches the synth into a Lower-Zone MPE layout:
        - Channel 1 = master channel (master pitch-bend, sustain, etc.)
        - Channels 2..15 = 14 note (member) channels for per-note expression
        - Channel 16 = unused
        Master bend range defaults to 2 semitones, note bend range to 48 semitones,
        which is the de-facto MPE specification. */
    void setMpeZoneLayoutLower(int memberChannels = 14,
                               int notePitchBendRange = 48,
                               int masterPitchBendRange = 2);

    /** Re-enables legacy mode (single instrument, single bend range, channels 1..16). */
    void setLegacyModeWithPitchBendRange(int semitones);

private:
    juce::AudioProcessorValueTreeState& parameters;

    // Number of currently held notes (across all modes). In Poly this is purely informational;
    // in Mono/Legato this mirrors the size of noteStack.
    std::atomic<int> activeNoteCount{0};

    // Flag used to inform SynthVoice that the *next* noteStarted call is a legato transition
    // (i.e. we are changing pitch while at least one note is still held). The voice uses
    // this to avoid retriggering ADSR on legato transitions and to keep the envelope in
    // its current stage (single-trigger behaviour).
    std::atomic<bool> nextNoteIsLegato{false};
    std::atomic<bool> nextNotePreservesVoice{false};

    // Simple last-note stack for Mono and Legato modes.
    // - Stack is ordered by press time (front = oldest, back = most recent).
    // - currentNote always mirrors the back of the stack (or -1 if empty).
    std::vector<int> noteStack;
    int currentNote = -1;

    // Index of the voice last used in mono/legato mode.
    // noteAdded override always reuses this voice to prevent two-voice overlap.
    int lastMonoVoiceIndex = -1;
};


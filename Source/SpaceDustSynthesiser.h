#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "SynthVoice.h"
#include "SynthSound.h"
#include <atomic>
#include <vector>

//==============================================================================
/**
    SpaceDust Custom Synthesiser
    
    Extends juce::Synthesiser with mono and legato mode support.
    - Poly: multiple notes, envelope retriggers each note.
    - Mono: one note at a time, envelope retriggers each note.
    - Legato: one note at a time; on overlapping note-on, no envelope retrigger (smooth pitch glide).
*/
class SpaceDustSynthesiser : public juce::Synthesiser
{
public:
    SpaceDustSynthesiser(juce::AudioProcessorValueTreeState& apvts) : parameters(apvts) {}
    
    juce::AudioProcessorValueTreeState& getParameters() { return parameters; }
    
    // Legato flag for the next note (set when Legato mode + overlapping note-on). Cleared when read.
    bool getAndClearNextNoteLegato() { return nextNoteIsLegato.exchange(false); }
    // Non-clearing peek (used by voice in stopNote to detect legato handoff).
    bool isNextNoteLegato() const { return nextNoteIsLegato.load(); }
    
    // voiceMode: 0=Poly, 1=Mono, 2=Legato. Use getIndex() so Choice is read correctly.
    int getVoiceModeIndex() const;
    
    // Max currentPitch across all voices (for Poly glide when the new note uses a different voice).
    double getMaxCurrentPitch() const;
    
    // Custom note-on/off handlers for mono/legato mode
    void noteOn(const juce::MidiMessage& midiMessage);
    void noteOff(const juce::MidiMessage& midiMessage);
    
    // Process MIDI buffer with mono/legato handling and active-note count
    void processMidiBuffer(juce::MidiBuffer& midiMessages, int numSamples);
    
private:
    juce::AudioProcessorValueTreeState& parameters;
    
    // Number of currently held notes (across all modes). In Poly this is purely informational;
    // in Mono/Legato this mirrors the size of noteStack.
    std::atomic<int> activeNoteCount{0};
    
    // Flag used to inform SynthVoice that the *next* startNote call is a legato transition
    // (i.e. we are changing pitch while at least one note is still held). The voice uses
    // this to avoid retriggering ADSR on legato transitions and to keep the envelope in
    // its current stage (single-trigger behaviour).
    std::atomic<bool> nextNoteIsLegato{false};
    
    // Simple last-note stack for Mono and Legato modes.
    // - Stack is ordered by press time (front = oldest, back = most recent).
    // - currentNote always mirrors the back of the stack (or -1 if empty).
    std::vector<int> noteStack;
    int currentNote = -1;
};


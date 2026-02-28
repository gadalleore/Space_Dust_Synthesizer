#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

//==============================================================================
/**
    A simple sound that can be played by a SynthVoice.
*/
class SynthSound : public juce::SynthesiserSound
{
public:
    bool appliesToNote(int midiNoteNumber) override
    {
        return true; // This sound can play any note
    }

    bool appliesToChannel(int midiChannel) override
    {
        return true; // This sound can play on any channel
    }
};


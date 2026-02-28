#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include "SexiconReverb.h"

//==============================================================================
/**
    Reverb processor for Space Dust.
    Types: Schroeder (Freeverb), Sexicon take an L (Dattorro/Lexicon 480L style).
    Filter: toggle + HP/LP in series (like delay effect).
*/
class SpaceDustReverb
{
public:
    enum ReverbType { SchroederFreeverb = 0, SexiconTakeAnL = 1, NumTypes };

    struct Parameters
    {
        int type = 0;
        float wetMix = 0.33f;
        float decayTime = 2.0f;
        bool filterOn = false;
        bool filterWarmSaturation = false;
        float filterHPCutoff = 100.0f;
        float filterHPResonance = 0.3f;
        float filterLPCutoff = 8000.0f;
        float filterLPResonance = 0.3f;
    };

    SpaceDustReverb() = default;

    void prepare(const juce::dsp::ProcessSpec& spec);
    void reset();
    void setParameters(const Parameters& p);
    void process(juce::AudioBuffer<float>& buffer);

private:
    juce::dsp::ProcessSpec spec_;
    Parameters params_;

    juce::Reverb freeverb_;
    SexiconReverb sexicon_;
    juce::dsp::StateVariableTPTFilter<float> filterHP_;
    juce::dsp::StateVariableTPTFilter<float> filterLP_;

    juce::SmoothedValue<float> smoothedWet_;
    juce::SmoothedValue<float> smoothedDry_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SpaceDustReverb)
};

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>

//==============================================================================
/**
    Parametric EQ for Space Dust.
    Three-band EQ: Low Shelf, Peaking (Bell), High Shelf.
    Uses RBJ/Audio EQ Cookbook biquad coefficients via JUCE IIR::Coefficients.
*/
class SpaceDustParametricEQ
{
public:
    struct BandParams
    {
        float freqHz = 1000.0f;   // 20-20000
        float gainDb = 0.0f;      // -12 to +12
        float Q = 0.707f;         // 0.1 to 10
    };

    struct Parameters
    {
        bool enabled = false;
        BandParams lowShelf;   // Band 0
        BandParams peak;       // Band 1
        BandParams highShelf;  // Band 2
    };

    SpaceDustParametricEQ() = default;

    void prepare(const juce::dsp::ProcessSpec& spec);
    void reset();
    void setParameters(const Parameters& p);
    void process(juce::AudioBuffer<float>& buffer);

private:
    void updateCoefficients();

    juce::dsp::ProcessSpec spec_;
    Parameters params_;

    using Filter = juce::dsp::IIR::Filter<float>;
    using Coeffs = juce::dsp::IIR::Coefficients<float>;
    std::array<juce::dsp::ProcessorDuplicator<Filter, Coeffs>, 3> filters_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SpaceDustParametricEQ)
};

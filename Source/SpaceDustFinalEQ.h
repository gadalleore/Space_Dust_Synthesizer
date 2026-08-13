#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>

//==============================================================================
/**
    SpaceDust Final EQ – 5-band parametric EQ at the very end of the effects chain.

    Every band picks its own shape (see BandType); freq/gain are draggable in the
    editor and Q is scrollable. Defaults keep the classic layout:
      Band 1 – Low Shelf   (default  80 Hz, Q 0.707)
      Band 2 – Bell        (default 250 Hz, Q 1.0)
      Band 3 – Bell        (default 1000 Hz, Q 1.0)
      Band 4 – Bell        (default 4000 Hz, Q 1.0)
      Band 5 – High Shelf  (default 10000 Hz, Q 0.707)

    Uses JUCE IIR::Coefficients (RBJ / Audio EQ Cookbook biquad formulas).
    gainFactor is linear amplitude: pow(10, dBgain/20).
*/
class SpaceDustFinalEQ
{
public:
    /** Band shapes. The value doubles as the index of the "Type" dropdown choice
        (see typeChoices()), and that index is what a preset saves -- so append new
        shapes at the end of this list, never reorder it. */
    enum class BandType { LowShelf = 0, HighShelf, LowPass, HighPass, Bell };

    static constexpr int numTypes = 5;

    /** Dropdown labels for BandType, in enum order. */
    static juce::StringArray typeChoices()
    {
        return { "Low Shelf", "High Shelf", "Low Pass", "High Pass", "Bell" };
    }

    static BandType typeFromChoiceIndex(int index) noexcept
    {
        return static_cast<BandType>(juce::jlimit(0, numTypes - 1, index));
    }

    /** Low Pass and High Pass are cuts: they have a corner frequency and a slope
        (Q), but no gain. The editor greys the Gain knob out for them and pins the
        band's dot to the 0 dB line, and the DSP below ignores gainDb. */
    static bool typeUsesGain(BandType t) noexcept
    {
        return t != BandType::LowPass && t != BandType::HighPass;
    }

    struct BandParams
    {
        float freqHz = 1000.0f;   // 20 – 20000 Hz
        float gainDb = 0.0f;      // -15 to +15 dB
        float Q      = 1.0f;      // 0.1 to 10
        BandType type = BandType::Bell;
    };

    struct Parameters
    {
        bool enabled = false;
        std::array<BandParams, 5> bands;

        Parameters()
        {
            const float freqs[5] = { 80.0f, 250.0f, 1000.0f, 4000.0f, 10000.0f };
            const float qs[5]    = { 0.707f, 1.0f, 1.0f, 1.0f, 0.707f };
            const BandType types[5] = {
                BandType::LowShelf, BandType::Bell, BandType::Bell,
                BandType::Bell, BandType::HighShelf
            };
            for (int i = 0; i < 5; ++i)
            {
                bands[i].freqHz = freqs[i];
                bands[i].gainDb = 0.0f;
                bands[i].Q      = qs[i];
                bands[i].type   = types[i];
            }
        }
    };

    SpaceDustFinalEQ() = default;

    void prepare(const juce::dsp::ProcessSpec& spec);
    void reset();
    void setParameters(const Parameters& p);
    void process(juce::AudioBuffer<float>& buffer);

private:
    void updateCoefficients();

    double sampleRate_ = 44100.0;
    Parameters params_;

    static constexpr int numBands    = 5;
    static constexpr int maxChannels = 2;

    using Filter = juce::dsp::IIR::Filter<float>;
    using Coeffs = juce::dsp::IIR::Coefficients<float>;

    // filters_[band][channel] – direct per-channel biquad, no ProcessorDuplicator
    Filter filters_[numBands][maxChannels];

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SpaceDustFinalEQ)
};

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>

//==============================================================================
/**
    SpaceDust Phaser - High-quality stereo phaser modeled on MXR Phase 90.
    
    Circuit-inspired design based on:
    - MXR Phase 90: 4 cascaded first-order all-pass stages with RC networks,
      JFETs as voltage-controlled resistors, base notch frequencies ~58 Hz and ~341 Hz.
      Triangular LFO (vintage). Block Logo versions: feedback resistor R28 for
      mid-hump/gain boost. Script versions: no feedback (cleaner sweep).
    - musicdsp.org: 6 all-pass stages, sine LFO, feedback, depth + tanh saturation.
    
    All-pass filter theory:
    - First-order all-pass: H(z) = (a1 + z^-1) / (1 + a1*z^-1)
    - Coefficient: a1 = (1 - normalizedDelay) / (1 + normalizedDelay)
    - Normalized delay modulates sweep: min/max freq via LFO
    - Each stage creates phase shift; cascading creates comb-filter notches.
    - Notch depth increases with number of stages (4 = classic Phase 90, 6 = deeper swirl).
    
    LFO modulation:
    - Sine: smooth, modern (like EHX Small Stone)
    - Triangle: vintage Phase 90 character, sharper sweep edges
    - Vintage mode: triangular + JFET-like soft saturation on modulation for analog feel
*/
class SpaceDustPhaser
{
public:
    struct Parameters
    {
        bool enabled = false;
        float rateHz = 1.0f;         // LFO speed 0.05-10 Hz
        float depth = 0.7f;         // Modulation amount 0-1
        float feedback = 0.0f;      // -1 to 1; Script=0, Block Logo ~0.3-0.6
        bool scriptMode = true;      // true = Script (no fb), false = Block Logo (with fb)
        float mix = 0.5f;           // Dry/wet 0-1
        float centreHz = 400.0f;    // Base sweep center 50-2000 Hz
        int numStages = 4;          // 4 = Phase 90, 6 = deeper
        float stereoOffset = 0.5f;  // L/R LFO phase offset 0-1 (0.5 = 180°)
        bool vintageMode = false;   // Triangle LFO + JFET-like curve vs sine
    };

    SpaceDustPhaser() = default;

    void prepare(const juce::dsp::ProcessSpec& spec);
    void reset();
    void setParameters(const Parameters& p);
    void process(juce::AudioBuffer<float>& buffer);

private:
    /** First-order all-pass: y[n] = a1*x[n] + x[n-1] - a1*y[n-1] */
    struct FirstOrderAllPass
    {
        float a1{0.0f};
        float x1{0.0f};
        float y1{0.0f};

        void setCoefficient(float coeff)
        {
            a1 = juce::jlimit(-0.9999f, 0.9999f, coeff);
        }

        float process(float x)
        {
            float y = a1 * x + x1 - a1 * y1;
            x1 = x;
            y1 = y;
            // Denorm protection
            if (std::abs(y) < 1e-10f) y1 = 0.0f;
            return y;
        }

        void reset()
        {
            x1 = y1 = 0.0f;
        }
    };

    void updateAllPassCoefficients(int channel, float modLfo, float depth, float centre);

    juce::dsp::ProcessSpec spec_;
    Parameters params_;

    static constexpr int kMaxStages = 6;
    std::array<std::array<FirstOrderAllPass, kMaxStages>, 2> allPass_;  // [0]=L, [1]=R

    float lfoPhaseL_{0.0f};
    float prevWetL_{0.0f};
    float prevWetR_{0.0f};

    juce::SmoothedValue<float> smoothedMix_{0.5f};
    juce::SmoothedValue<float> smoothedDepth_{0.7f};
    juce::SmoothedValue<float> smoothedFeedback_{0.0f};
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> smoothedCentre_{400.0f};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SpaceDustPhaser)
};

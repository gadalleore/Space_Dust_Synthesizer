#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>

//==============================================================================
/**
    SpaceDust Soft Clipper - KClip 3-style mastering clipper with multiple modes.

    Modes (KClip 3-inspired):
    - Smooth:  tanh-based, transparent, natural-sounding
    - Crisp:   low-order harmonics (cubic), punchy
    - Tube:    even harmonics, warm tube emulation
    - Tape:    saturation with high-freq roll-off, glued sound
    - Guitar:  asymmetric diode clipping, amp-like character

    Features:
    - Oversampling (2x, 4x, 8x, 16x) to reduce aliasing
    - Variable knee (threshold) for transparent clipping
    - Drive (pre-gain) and dry/wet mix
*/
class SpaceDustSoftClipper
{
public:
    enum class Mode
    {
        Smooth = 0,   // tanh - tube/diode-like, symmetric
        Crisp = 1,     // cubic - low-order odd harmonics
        Tube = 2,      // asymmetric - even harmonics
        Tape = 3,      // saturation + high-freq roll-off
        Guitar = 4     // asymmetric diode-like
    };

    struct Parameters
    {
        bool enabled = false;
        int mode = 0;       // 0-4
        float drive = 1.0f; // pre-gain 0.5-5.0
        float knee = 0.666f; // threshold 0.3-1.0 (~2/3 default)
        int oversample = 2; // 2, 4, 8, 16
        float mix = 1.0f;   // dry/wet 0-1
    };

    SpaceDustSoftClipper() = default;
    ~SpaceDustSoftClipper() = default;

    void prepare(const juce::dsp::ProcessSpec& spec);
    void reset();
    void setParameters(const Parameters& p);
    void process(juce::AudioBuffer<float>& buffer);

private:
    using Oversampler = juce::dsp::Oversampling<float>;

    juce::dsp::ProcessSpec spec_;
    Parameters params_;

    juce::SmoothedValue<float> smoothedDrive_{1.0f};
    juce::SmoothedValue<float> smoothedKnee_{0.666f};
    juce::SmoothedValue<float> smoothedMix_{1.0f};

    std::unique_ptr<Oversampler> oversampler_;
    int currentOversampleFactor_{2};

    juce::dsp::StateVariableTPTFilter<float> tapeFilterL_;
    juce::dsp::StateVariableTPTFilter<float> tapeFilterR_;

    // Soft clip transfer functions (input x in [-1,1], output y)
    float clipSmooth(float x, float k, float t) const;
    float clipCrisp(float x, float k, float t) const;
    float clipTube(float x, float k, float t) const;
    float clipTape(float x, float k, float t) const;
    float clipGuitar(float x, float k, float t) const;

    void ensureOversampler(int factor);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SpaceDustSoftClipper)
};

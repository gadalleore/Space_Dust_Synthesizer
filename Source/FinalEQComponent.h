#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>

//==============================================================================
/**
    FinalEQComponent – interactive 5-band parametric EQ display.

    Shows a frequency response curve with five coloured draggable dots:
      • Drag horizontally  → adjusts band frequency (log scale, 20–20 kHz)
      • Drag vertically    → adjusts band gain (±15 dB)
      • Mouse-wheel on dot → adjusts Q (0.1–10) for peak bands

    Reads / writes parameters via AudioProcessorValueTreeState using the IDs:
      finalEQEnabled, finalEQB{1-5}Freq, finalEQB{1-5}Gain, finalEQB{1-5}Q
*/
class FinalEQComponent : public juce::Component,
                         private juce::AudioProcessorValueTreeState::Listener
{
public:
    static constexpr int numBands = 5;

    FinalEQComponent(juce::AudioProcessorValueTreeState& apvts, double sampleRate = 44100.0);
    ~FinalEQComponent() override;

    void setSampleRate(double sr);

    void paint(juce::Graphics& g) override;
    void resized() override;

    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseWheelMove(const juce::MouseEvent& e,
                        const juce::MouseWheelDetails& wheel) override;

private:
    // AudioProcessorValueTreeState::Listener
    void parameterChanged(const juce::String& parameterID, float newValue) override;

    // Response computation
    void recomputeResponse();

    // Coordinate mapping (operates in local pixel space within displayArea_)
    float freqToX(float freq)   const;
    float gainToY(float gainDb) const;
    float xToFreq(float x)      const;
    float yToGain(float y)      const;

    juce::Point<float> getBandDotPos(int band) const;
    int getBandNearPos(juce::Point<float> pos) const; // returns -1 if none close

    // Parameter ID helpers  (band is 0-indexed internally, 1-indexed in ID)
    static juce::String freqId(int band);
    static juce::String gainId(int band);
    static juce::String qId  (int band);

    // Band properties
    enum class BandType { LowShelf, Peak, HighShelf };
    static BandType getBandType(int band);

    void setParam(const juce::String& paramId, float value);

    //==============================================================================
    static constexpr float freqMin_  = 20.0f;
    static constexpr float freqMax_  = 20000.0f;
    static constexpr float gainRange_ = 15.0f;  // ±gainRange_ dB
    static constexpr float dotRadius_ = 7.0f;
    static constexpr float dotHitRadius_ = 12.0f; // larger hit target

    juce::AudioProcessorValueTreeState& apvts_;
    double sampleRate_ = 44100.0;

    juce::Rectangle<int> displayArea_;  // set in resized()

    // Pre-allocated response magnitude buffer (dB per pixel column)
    std::vector<float> responseMag_;

    // Drag state
    int   draggedBand_    = -1;
    float dragStartFreq_  = 0.0f;
    float dragStartGain_  = 0.0f;
    juce::Point<float> dragStartPos_;

    // Cached parameter values (updated in parameterChanged / constructor)
    std::array<float, numBands> cachedFreq_ {};
    std::array<float, numBands> cachedGain_ {};
    std::array<float, numBands> cachedQ_    {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FinalEQComponent)
};

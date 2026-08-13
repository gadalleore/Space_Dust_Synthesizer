#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>

#include "SpaceDustFinalEQ.h"
#include "SpectrumAnalyserComponent.h"

//==============================================================================
/**
    FinalEQComponent – interactive 5-band parametric EQ display.

    Shows a live spectrum, a frequency response curve, and five coloured draggable
    dots:
      • Drag horizontally  → adjusts band frequency (log scale, 20–20 kHz)
      • Drag vertically    → adjusts band gain (±15 dB; cuts have no gain)
      • Mouse-wheel on dot → adjusts Q (0.1–10)
      • Click a dot        → selects that band (onBandSelected)

    Reads / writes parameters via AudioProcessorValueTreeState using the IDs:
      finalEQEnabled, finalEQB{1-5}Freq, finalEQB{1-5}Gain, finalEQB{1-5}Q,
      finalEQB{1-5}Type

    The spectrum behind the curve is the Spectral tab's analyser, drawn as a
    backdrop over the same 20 Hz–20 kHz log axis the curve uses, so a peak in the
    audio sits directly under the part of the curve that shapes it.
*/
class FinalEQComponent : public juce::Component,
                         private juce::AudioProcessorValueTreeState::Listener,
                         private juce::AsyncUpdater
{
public:
    static constexpr int numBands = 5;

    using BandType = SpaceDustFinalEQ::BandType;

    FinalEQComponent(juce::AudioProcessorValueTreeState& apvts, double sampleRate = 44100.0);
    ~FinalEQComponent() override;

    void setSampleRate(double sr);

    /** Feeds the built-in spectrum analyser. Same source the Spectral tab uses. */
    void setSampleSource(std::function<void(float*, int)> fillSamples);

    /** Recolours the spectrum for the clipping state, as on the Spectral tab. */
    void setClipping(bool isClipping);

    /** Which band the editor's Node dropdown is on. Highlights that dot. */
    void setSelectedBand(int band);
    int  getSelectedBand() const noexcept { return selectedBand_; }

    /** Called when the user clicks or drags a band's dot, so the editor can move
        its Node dropdown (and the Quality / Frequency / Gain knobs) to that band. */
    std::function<void(int band)> onBandSelected;

    void paint(juce::Graphics& g) override;
    void resized() override;

    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseWheelMove(const juce::MouseEvent& e,
                        const juce::MouseWheelDetails& wheel) override;

private:
    //==========================================================================
    /** Curve, dots and grid, drawn on TOP of the spectrum.

        Child components always paint after their parent, so the spectrum -- being
        a child -- would cover anything the parent drew. The foreground therefore
        has to be a child of its own, added after it. */
    class CurveOverlay : public juce::Component
    {
    public:
        explicit CurveOverlay(FinalEQComponent& o) : owner_(o)
        {
            setInterceptsMouseClicks(false, false);   // the parent owns the mouse
            setAccessible(false);
        }
        void paint(juce::Graphics& g) override { owner_.paintCurve(g); }
    private:
        FinalEQComponent& owner_;
    };

    // AudioProcessorValueTreeState::Listener — fires on the AUDIO/automation thread.
    void parameterChanged(const juce::String& parameterID, float newValue) override;

    // AsyncUpdater — marshals the parameter-driven refresh back to the MESSAGE thread,
    // where it is safe to touch the Component (recompute + repaint). Coalesces a flood
    // of automation into a single update.
    void handleAsyncUpdate() override;

    // Response computation
    void recomputeResponse();

    // Foreground painting, called by the overlay child.
    void paintCurve(juce::Graphics& g);

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
    static juce::String typeId(int band);

    /** The band's shape, as the Type parameter currently has it. */
    BandType getBandType(int band) const;

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

    SpectrumAnalyserComponent spectrum_;   // backdrop (added first)
    CurveOverlay              curveOverlay_ { *this };   // foreground (added last)

    // Pre-allocated response magnitude buffer (dB per pixel column)
    std::vector<float> responseMag_;

    // Drag state
    int   draggedBand_    = -1;
    int   selectedBand_   = 0;
    float dragStartFreq_  = 0.0f;
    float dragStartGain_  = 0.0f;
    juce::Point<float> dragStartPos_;

    // Cached parameter values (updated in parameterChanged / constructor)
    std::array<float, numBands> cachedFreq_ {};
    std::array<float, numBands> cachedGain_ {};
    std::array<float, numBands> cachedQ_    {};
    std::array<int,   numBands> cachedType_ {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FinalEQComponent)
};

#pragma once

#include "AssignModeState.h"
#include "ModMatrix.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <string>

/** One knob's modulation behaviour, laid over the top of an existing Slider.

    Sits in front of the slider it wraps and is transparent to the mouse while
    assign mode is off, so the slider behaves exactly as it always did. While
    assign mode is on it takes the mouse instead: a drag sets the ROUTING
    amount, not the knob's own value.

    It also draws the indicator bar, which is visible whether or not assign mode
    is on, because what moves in a patch should always be readable. */
class ModulatableKnob : public juce::Component,
                        private juce::ChangeListener
{
public:
    ModulatableKnob (juce::Slider& knobToWrap,
                     spacedust::ModMatrix& matrixToEdit,
                     spacedust::AssignModeState& modeState,
                     std::function<void()> onRoutingChanged);

    ~ModulatableKnob() override;

    /** The APVTS parameter id this knob drives. */
    void setDestination (std::string parameterId) { destination = std::move (parameterId); }

    const std::string& getDestination() const noexcept { return destination; }

    /** Where the LFOs are in their cycles, 0..1, for the bar's marker.
        Pushed in by the editor's repaint timer rather than pulled, so one timer
        serves every knob. */
    void setLfoPhases (const float* phases01);

    void paint (juce::Graphics&) override;
    void resized() override;

    bool hitTest (int x, int y) override;

    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

    /** The strip to the right of the knob that holds the indicator bar. */
    static constexpr int barWidth = 6;
    static constexpr int barGap   = 3;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;

    juce::Rectangle<int> barArea() const;

    /** Which LFO owns the bar lane under this point, or -1. */
    int lfoLaneAt (juce::Point<int> position) const;

    juce::Slider&               knob;
    spacedust::ModMatrix&       matrix;
    spacedust::AssignModeState& mode;
    std::function<void()>       routingChanged;

    std::string destination;

    float phases[spacedust::numLfos] { 0.0f, 0.0f, 0.0f, 0.0f };

    /** Set while a drag is running, so the percentage reads out only then. */
    bool  dragging = false;
    int   dragLfo = -1;
    float dragStartAmount = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ModulatableKnob)
};

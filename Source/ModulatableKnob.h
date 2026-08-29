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
                        private juce::ChangeListener,
                        private juce::ComponentListener
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

    /** The slider's own rectangle, in the parent's coordinates. The wrapper sits
        exactly here -- the indicator bar is drawn inside this rectangle, not
        beside it, so the wrapper can never reach over a neighbouring knob. */
    juce::Rectangle<int> getWrappedBounds() const { return knob.getBounds(); }

    /** Add this wrapper to the slider's parent, then follow that slider for good.

        WHY IT PLACES ITSELF -- the same reason ComboStepper does. The layouts
        these knobs sit in are dense, hand-tuned, and spread over five page
        components that re-run their own resized() when a toggle moves; roughly
        150 of them would have to be positioned from the editor, AFTER whichever
        layout last moved the knob. Following the slider is the only version of
        that which cannot be out of date: the wrapper is told when the knob
        moves, resizes, is hidden or is disabled, and matches it every time.

        Safe to call with the slider parented or not, and called AGAIN from
        componentParentHierarchyChanged whenever the slider moves house -- the
        shaping and unison knobs have no parent until the Waveforms panel
        borrows them, which is long after the editor is built. */
    void attachToKnobParent();

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

    /** The strip inside the knob's right edge that holds the indicator bar,
        and how far it is held off that edge. */
    static constexpr int barWidth = 6;
    static constexpr int barGap   = 3;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;

    //== following the slider ==================================================
    void componentMovedOrResized (juce::Component&, bool, bool) override { followKnob(); }
    void componentVisibilityChanged (juce::Component&) override          { followVisibility(); }
    void componentEnablementChanged (juce::Component&) override          { followVisibility(); }

    /** The knob can be RE-PARENTED long after the wrapper was made: the shaping
        and unison knobs have no parent at all until the Waveforms panel borrows
        them, and get given back when it is pointed at another oscillator. */
    void componentParentHierarchyChanged (juce::Component&) override      { attachToKnobParent(); }

    /** Sit exactly over the knob. Never wider -- see the implementation. */
    void followKnob();

    /** A ring around a control that is not there is a ring around nothing.
        Half the effects knobs are hidden until their filter is switched on. */
    void followVisibility();

    juce::Rectangle<int> barArea() const;

    /** The one true partition of the bar into lanes. paint() and lfoLaneAt()
        both call this rather than each computing lane geometry on its own --
        two independent computations of the same pixels is how they end up
        disagreeing at lane counts that do not divide evenly. */
    juce::Rectangle<float> laneBoundsFor (int laneIndex, int laneCount) const;

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

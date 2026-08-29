#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "PitchCurve.h"
#include "SpaceDustLookAndFeel.h"

#include <functional>
#include <vector>

//==============================================================================
/** The curve's thumbnail box, where Pitch Env Amount, Time and Pitch used to
    sit. Draws the shape at thumbnail size -- flat by default, with a line at
    the centre meaning "no bend" -- and calls onClick when pressed.

    Read-only: all editing happens in the panel the click opens, see
    PitchCurveEditorPanel below. */
class PitchCurveBox : public juce::Component
{
public:
    explicit PitchCurveBox (const spacedust::PitchCurve& curveToShow);

    void paint (juce::Graphics&) override;
    void mouseUp (const juce::MouseEvent&) override;

    /** Called after a click that did not turn into a drag out of the box. */
    std::function<void()> onClick;

private:
    const spacedust::PitchCurve& curve;

    // Matches the editor panel's own vertical axis -- see PitchCurvePlot.
    static constexpr float maxSemitones = 24.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PitchCurveBox)
};

//==============================================================================
/** The plot inside the editor panel: draws the curve full-size against a
    -24..+24 semitone axis with a line at zero, and lets the player click to
    add a point, drag to move one, and right-click to remove one.

    Point count and edit granularity are capped here as well as inside
    PitchCurve itself (see PitchCurve::maxPoints): a new point is added only on
    a click, never once per pixel of a drag, so a full-width drag never adds
    more than the one point that click started. */
class PitchCurvePlot : public juce::Component
{
public:
    explicit PitchCurvePlot (spacedust::PitchCurve& curveToEdit);

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

    /** Reload the working points from the curve. Call when the panel opens,
        in case the curve changed under it (a patch load while it was open). */
    void refresh();

    /** Called after every edit that changes the curve -- add, move, remove --
        so the owner can repaint the thumbnail box and mark the host state as
        changed. Not called during a plain mouse-down that starts a drag on an
        existing point without moving it yet -- only once the shape itself has
        actually changed. */
    std::function<void()> onCurveChanged;

private:
    /** A point being edited, with a stable id so a drag can keep tracking the
        SAME point across the re-sort every commit() does -- an index alone
        would not survive that reorder if a drag crosses another point. */
    struct WorkingPoint
    {
        float t01 = 0.0f;
        float semitones = 0.0f;
        int   id = 0;
    };

    /** Pixel <-> curve-space conversions, against this component's own bounds. */
    float xToT01 (float x) const noexcept;
    float yToSemitones (float y) const noexcept;
    float t01ToX (float t01) const noexcept;
    float semitonesToY (float semitones) const noexcept;

    /** Index into `working` of the point nearest the given position, within
        hitRadius pixels, or -1 if none is that close. */
    int findNearest (juce::Point<float> position) const noexcept;

    /** Sort by time, push to the curve as one atomic setPoints() call -- see
        PitchCurve::setPoints for why this must never be clear()+addPoint() in
        a loop -- and tell the owner. */
    void commit();

    static constexpr float minSemitones = -24.0f;
    static constexpr float maxSemitones =  24.0f;
    static constexpr float hitRadius    =  10.0f;

    spacedust::PitchCurve& curve;

    std::vector<WorkingPoint> working;
    int nextId      = 0;
    int draggingId  = -1;   // -1 == no point is being dragged

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PitchCurvePlot)
};

//==============================================================================
/** The movable window the box opens: PitchCurvePlot plus the Time knob
    underneath it, in a frame that copies WaveformEditorPanel's -- the same
    title bar drag, the same clampInsideParent(), the same
    stays-where-you-put-it behaviour on reopening (see hasBeenMoved).

    A second, independent implementation rather than a shared base class:
    WaveformEditorPanel carries waveform-only state (the sample library, file
    drop, the resample host) that has no meaning here, and this panel's
    content is a plot, not a waveform list. Copying the four frame behaviours
    -- paint, drag, clamp, escape-to-close -- costs a few dozen lines and
    keeps this file free of any dependency on the Waveforms feature. */
class PitchCurveEditorPanel : public juce::Component
{
public:
    PitchCurveEditorPanel (spacedust::PitchCurve& curveToEdit,
                           juce::AudioProcessorValueTreeState& stateToUse,
                           juce::AudioProcessor& processorToNotify,
                           SpaceDustLookAndFeel& lookAndFeelToUse);
    ~PitchCurveEditorPanel() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    /** A press that reached the panel is a press INSIDE it -- see
        WaveformEditorPanel::mouseDown for why that matters to
        OutsideClickWatcher. A press on the title bar also begins a drag. */
    void mouseDown (const juce::MouseEvent& event) override;
    void mouseDrag (const juce::MouseEvent& event) override;
    void mouseUp (const juce::MouseEvent& event) override;
    void mouseMove (const juce::MouseEvent& event) override;

    bool keyPressed (const juce::KeyPress& key) override;

    /** Bring the panel up, placed against the box that was clicked -- unless
        the player has moved it before, in which case it stays put. */
    void showFor (juce::Component* anchorBox);

    void hidePanel();

    /** How far down the parent the panel may reach -- see
        WaveformEditorPanel::setKeepAboveBottom for why the standalone needs
        this told rather than worked out. */
    void setKeepAboveBottom (int y) { keepAboveBottom = y; }

    /** Called after every edit, so the owner can repaint the thumbnail box
        while this panel is open and the shape is changing under the mouse. */
    std::function<void()> onCurveChanged;

private:
    struct OutsideClickWatcher : public juce::MouseListener
    {
        explicit OutsideClickWatcher (PitchCurveEditorPanel& ownerToUse) : owner (ownerToUse) {}
        void mouseDown (const juce::MouseEvent& event) override;
        PitchCurveEditorPanel& owner;
    };

    juce::Rectangle<int> titleBarArea() const;
    void clampInsideParent();

    static constexpr int frameInset = 8;
    static constexpr int titleHeight = 24;

    juce::ComponentDragger dragger;
    bool draggingPanel = false;
    bool hasBeenMoved = false;

    juce::AudioProcessor& processorForHostNotify;
    SpaceDustLookAndFeel& lookAndFeel;

    PitchCurvePlot plot;
    juce::Slider timeSlider;
    juce::Label timeLabel;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> timeAttachment;
    juce::TextButton closeButton { "X" };

    OutsideClickWatcher outsideClicks { *this };
    bool watchingOutsideClicks = false;

    int keepAboveBottom = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PitchCurveEditorPanel)
};

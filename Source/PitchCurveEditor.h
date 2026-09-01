#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "PitchCurve.h"
#include "PitchCurveDivisions.h"
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
class PitchCurvePlot : public juce::Component,
                       public juce::TooltipClient
{
public:
    explicit PitchCurvePlot (spacedust::PitchCurve& curveToEdit);

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

    /** Removes the point under the mouse, the same as a right-click.

        Two ways to do one thing, because right-click is the discoverable one
        and double-click is the one hands already know. Double-clicking EMPTY
        space adds a point on the first click and removes it on the second, so
        it comes to nothing -- which is what double-clicking nothing should
        do. */
    void mouseDoubleClick (const juce::MouseEvent&) override;

    /** What this plot says under the mouse.

        Nothing about drawing a pitch shape is discoverable by looking at it --
        that a point snaps, that it snaps to TWO different grids, and that Shift
        is what lets go of both, are all things a player would otherwise have to
        be told out of band. So the plot says them itself, and says them
        differently over a point than over empty space, because the gestures
        available are different in the two places. */
    juce::String getTooltip() override;

    /** Reload the working points from the curve. Call when the panel opens,
        in case the curve changed under it (a patch load while it was open). */
    void refresh();

    /** What the vertical grid should count across the width.

        `beats` is how many beats the whole shape plays over when synced, taken
        straight from PitchCurveDivisions.h. When it is not synced there is no
        musical grid to draw, and the plot falls back to plain quarters of
        whatever the Time knob says.

        The panel pushes this in whenever Sync or Division moves, so the grid
        under the curve always counts the same thing the shape is played to. */
    void setTimeGrid (bool syncedToTempo, double beats);

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

    /** Whole semitones, unless Shift is held.

        Applied to every new point and to every step of a drag, so a shape is
        in tune by default and only leaves the grid when asked to. */
    static float snapSemitones (float semitones, const juce::ModifierKeys& mods) noexcept;

    /** A thirty-second of the whole shape, unless Shift is held.

        Shift is the ONE override for both axes: hold it and a node goes exactly
        where the mouse is, in time and in pitch. Releasing it snaps again.

        A thirty-second of the SPAN rather than a thirty-second NOTE, and the
        difference only shows away from the default. At one bar the two are the
        same thing. At eight bars a thirty-second note would be 256 places to
        put a node, which is not a grid, it is a wash; a thirty-second of the
        span is one place per beat. At a 1/32 division a thirty-second note
        would be the whole span -- one position, and nothing could be drawn at
        all. Dividing the span always leaves 32 usable places, whatever the
        division says. */
    static float snapTime (float t01, const juce::ModifierKeys& mods) noexcept;

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

    /** The little "+12" / "-24" numbers against the octave lines. */
    static constexpr int octaveLabelWidth  = 24;
    static constexpr int octaveLabelHeight = 12;

    /** How far inside the frame the ENDS of the pitch axis sit.

        Without it +24 maps to y=0 and -24 to y=height -- both exactly on the
        border. A point dragged to either end then straddled the frame, which
        reads as the editor spilling out of its own box rather than reaching its
        limit, and the "+24" label sat on the line instead of under it. The
        inset keeps the whole range, handles and labels included, inside the
        box, so the frame is what the axis stops AT rather than what it is drawn
        ON (Giuseppe, 2026-09-01). */
    static constexpr float verticalInset = 10.0f;

    /** Closer than this and a grid stops being lines and starts being a wash,
        so the finer ruling is dropped instead of drawn. */
    static constexpr float minGridSpacing = 5.0f;

    /** How many places along the width a node may land on. The snap resolution
        and the finest vertical ruling are the SAME number on purpose -- a grid
        that does not mark where a node will land is decoration. */
    static constexpr int timeSnapDivisions = 32;

    /** Draw the vertical ruling. See setTimeGrid for what it counts. */
    void paintTimeGrid (juce::Graphics& g, juce::Rectangle<float> area) const;

    bool   gridSynced = false;
    double gridBeats  = 4.0;

    /** Where a point sits along the shape, said in the unit the shape is
        actually played in: beats when synced to the host, per cent when running
        free on the Time knob. Saying "beat 3" while the shape is measured in
        seconds would be a readout that lies. */
    juce::String describeTime (float t01) const;

    /** A point's pitch, with decimals only when there are decimals to show --
        a snapped point reads "+7 st", not "+7.00 st". */
    static juce::String describeSemitones (float semitones);

    /** Index into `working` of the point under the mouse, or -1.

        Kept as state because JUCE asks a component WHAT to say without telling
        it WHERE the mouse is: getTooltip() takes no position. It also drives
        the hover ring, so the tooltip's subject is visible when two points sit
        close enough together to be ambiguous. */
    int hoveredIndex = -1;

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

    /** Show the Time knob or the division combo, never both.

        They answer the same question -- how long does the shape play over --
        and only one of them is being listened to at a time, so the other is
        hidden rather than left on screen doing nothing. The same swap the LFO
        panels already make between their free-rate slider and sync combo. */
    void updateSyncControls();

    static constexpr int frameInset = 8;
    static constexpr int titleHeight = 24;

    juce::ComponentDragger dragger;
    bool draggingPanel = false;
    bool hasBeenMoved = false;

    juce::AudioProcessor& processorForHostNotify;
    SpaceDustLookAndFeel& lookAndFeel;

    PitchCurvePlot plot;

    // Two knobs in ONE place, and never both at once -- see updateSyncControls().
    // Time when the curve runs in seconds, Division when it runs to the host's
    // tempo. Both are knobs, because they are the same control wearing two
    // scales, and a knob beside a dropdown would not have looked like that.
    juce::Slider timeSlider;
    juce::Label  timeLabel;
    juce::Slider divisionSlider;
    juce::Label  divisionLabel;

    juce::ToggleButton syncButton { "Sync" };
    juce::ToggleButton loopButton { "Loop" };

    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> timeAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> divisionAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> syncAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> loopAttachment;

    juce::TextButton closeButton { "X" };

    OutsideClickWatcher outsideClicks { *this };
    bool watchingOutsideClicks = false;

    int keepAboveBottom = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PitchCurveEditorPanel)
};

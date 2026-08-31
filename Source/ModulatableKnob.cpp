#include "ModulatableKnob.h"

#include <cmath>
#include <vector>

namespace
{
    /** A full-height drag sets the amount from one end to the other. 150 pixels
        is roughly the height of a knob plus its label, which makes a normal
        gesture cover the range without feeling twitchy. */
    constexpr float dragPixelsForFullRange = 150.0f;
}

ModulatableKnob::ModulatableKnob (juce::Slider& knobToWrap,
                                  spacedust::ModMatrix& matrixToEdit,
                                  spacedust::AssignModeState& modeState,
                                  std::function<void()> onRoutingChanged)
    : knob (knobToWrap),
      matrix (matrixToEdit),
      mode (modeState),
      routingChanged (std::move (onRoutingChanged))
{
    mode.addChangeListener (this);

    // From here on the wrapper tracks the slider itself, so no layout has to
    // know this exists. See attachToKnobParent in the header for why.
    knob.addComponentListener (this);
}

ModulatableKnob::~ModulatableKnob()
{
    knob.removeComponentListener (this);
    mode.removeChangeListener (this);
}

void ModulatableKnob::attachToKnobParent()
{
    // Follows the knob WHEREVER it goes, and is called again from
    // componentParentHierarchyChanged every time the knob is re-parented.
    //
    // That matters more than it looks. The fifteen shaping knobs and the
    // sixteen unison knobs are plain members of the editor with NO parent at
    // all until the Waveforms panel borrows them, which happens the first time
    // the panel is opened -- long after the editor is built. A version of this
    // that only ran once, at construction, left twenty-seven wrappers built but
    // never parented: never laid out, never painted, never hit-tested, and so
    // twenty-seven knobs that could not be assigned to at all, in silence.
    //
    // The panel also GIVES the knobs back (removeChildComponent) when it is
    // pointed at another oscillator, which leaves the knob with no parent
    // again -- so the wrapper has to be able to leave a parent as well as join
    // one, or it would be left behind in the panel beside nothing.
    auto* wanted = knob.getParentComponent();

    if (getParentComponent() != wanted)
    {
        if (auto* current = getParentComponent())
            current->removeChildComponent (this);

        // addChildComponent, not addAndMakeVisible: whether the wrapper shows
        // is decided by followVisibility, from the knob it belongs to.
        if (wanted != nullptr)
            wanted->addChildComponent (this);
    }

    followKnob();
    followVisibility();
}

void ModulatableKnob::followKnob()
{
    if (getParentComponent() == nullptr)
        return;

    // EXACTLY the knob's own rectangle, and not one pixel wider.
    //
    // This used to widen by barWidth + barGap so the indicator bar could sit
    // BESIDE the knob rather than on it. On a tightly packed row that put nine
    // pixels of a click-taking, always-in-front layer over the next knob along:
    // in assign mode it swallowed that knob's clicks, and outside assign mode a
    // routed knob kept a live six-pixel strip over its neighbour. A gesture
    // landing on a different control from the one under the pointer is the same
    // fault the bar's own lane geometry was fixed for.
    //
    // No measurement of the neighbours would have been safe either: a page lays
    // its knobs out one at a time, so a knob asked "how much room is to my
    // right?" while its neighbour has not been placed yet would get last
    // frame's answer. Staying inside the knob's own bounds is correct at every
    // window size, in every layout, in any order.
    //
    // So the bar is drawn just inside the knob's right edge instead -- see
    // barArea(). That edge is the corner of the square a round knob is drawn
    // in, which is empty space on every knob in the plugin.
    // Still exactly the knob's rectangle, for horizontal sliders too.
    //
    // A first attempt grew the wrapper downward so a pan bar could sit under the
    // control. It did clear the track -- and landed on the "Pan" label instead,
    // because a slider's label sits only a few pixels beneath it.
    //
    // Measuring showed the growth was never needed: Osc 1 Pan's BOUNDS run to
    // y=217 while the track JUCE draws inside them stops at y=209. There is
    // already dead space below the track and inside the control, which is where
    // the bar belongs -- clear of the track above it and of the label below
    // (Giuseppe, 2026-08-31).
    setBounds (getWrappedBounds());
    toFront (false);
}

juce::Rectangle<int> ModulatableKnob::knobArea() const
{
    // The wrapper IS the control, in both orientations -- see followKnob.
    return getLocalBounds();
}

void ModulatableKnob::followVisibility()
{
    setVisible (knob.isVisible() && knob.isEnabled());
}

void ModulatableKnob::changeListenerCallback (juce::ChangeBroadcaster*)
{
    // Nothing to toggle here: hitTest() below reads mode.activeLfo() on every
    // call, so JUCE re-derives whether this layer or the slider underneath
    // gets the mouse each time, with no flag to keep in sync. A mode change
    // only needs a repaint, for the ring and the bar's assign-mode styling.
    repaint();
}

juce::Rectangle<int> ModulatableKnob::barArea() const
{
    // Just INSIDE the knob's right edge, held off it by barGap so the ring
    // drawn round the whole cell in assign mode is not sat on. The wrapper is
    // exactly the knob's rectangle -- see followKnob for why it may not be a
    // pixel wider -- so this is the one place the bar's geometry is decided.
    // A HORIZONTAL slider gets a horizontal bar underneath it, not a vertical
    // one beside it. Pan is the case that makes this obvious: the control moves
    // left and right, so a vertical strip says nothing about how far the sound
    // is being pushed either way (Giuseppe, 2026-08-31).
    if (isHorizontalControl())
        return getLocalBounds().removeFromBottom (hBarWidth).reduced (hBarInset, 0);

    return getLocalBounds().withTrimmedRight (barGap).removeFromRight (barWidth);
}

bool ModulatableKnob::isHorizontalControl() const
{
    const auto style = knob.getSliderStyle();
    return style == juce::Slider::LinearHorizontal
        || style == juce::Slider::LinearBar
        || style == juce::Slider::TwoValueHorizontal
        || style == juce::Slider::ThreeValueHorizontal;
}

int ModulatableKnob::firstRoutedLfo() const
{
    for (int lfo = 0; lfo < spacedust::numLfos; ++lfo)
        if (matrix.amountFor (lfo, destination) != 0.0f)
            return lfo;

    return 0;
}

bool ModulatableKnob::removeOffered() const
{
    // Whenever this knob carries a routing at all -- assign mode or not, so a
    // routing can be taken off without entering the mode to do it
    // (Giuseppe, 2026-08-31).
    //
    // Still never on an empty knob: an x that deletes nothing would appear on
    // all ~120 of them the moment assign mode opened, and would lie about what
    // pressing it does.
    if (mode.activeLfo() >= 0)
        return matrix.amountFor (mode.activeLfo(), destination) != 0.0f;

    return matrix.hasAnyRouting (destination);
}

juce::Rectangle<int> ModulatableKnob::removeArea() const
{
    // Top-left of the CONTROL, not of the wrapper -- a horizontal slider's
    // wrapper runs on below it to hold the bar. The percentage readout takes the
    // top-RIGHT while a drag is running, and the two must never sit on each
    // other.
    return knobArea().removeFromTop (removeSize).removeFromLeft (removeSize);
}

bool ModulatableKnob::hitTest (int x, int y)
{
    // This is the actual mechanism that makes the component transparent to the
    // mouse while assign mode is off: returning false means JUCE never
    // delivers the click to this layer, so it falls through to the slider
    // underneath, which behaves exactly as it always did. There is no
    // setInterceptsMouseClicks flag involved -- overriding hitTest bypasses
    // that mechanism entirely.

    // The bar is clickable even outside assign mode, so an amount can be edited
    // without entering the mode at all. Everything else falls through.
    if (barArea().contains (x, y) && matrix.hasAnyRouting (destination))
        return true;

    // So is the remove-x, for the same reason: a routing can be taken off
    // without entering the mode to do it.
    if (removeOffered() && removeArea().contains (x, y))
        return true;

    return mode.activeLfo() >= 0;
}

juce::Rectangle<float> ModulatableKnob::laneBoundsFor (int laneIndex, int laneCount) const
{
    auto bar = barArea().toFloat();

    if (laneCount <= 0)
        return bar;

    // Float division throughout: at lane counts that do not divide barWidth
    // evenly (four lanes in a 6px bar, for one), an integer version of this
    // would give a different partition than this one, and paint() and
    // lfoLaneAt() would silently disagree about which pixel belongs to which
    // lane. Both call this same function so there is only one partition to
    // get right.
    // Lanes stack ACROSS the bar's short axis, so which axis that is depends on
    // which way the bar lies: side by side under a rotary, one above another
    // beneath a horizontal slider.
    if (isHorizontalControl())
    {
        const float laneHeight = bar.getHeight() / (float) laneCount;
        return bar.withHeight (laneHeight).withY (bar.getY() + laneHeight * (float) laneIndex);
    }

    const float laneWidth = bar.getWidth() / (float) laneCount;

    return bar.withWidth (laneWidth).withX (bar.getX() + laneWidth * (float) laneIndex);
}

int ModulatableKnob::lfoLaneAt (juce::Point<int> position) const
{
    auto bar = barArea();

    if (! bar.contains (position))
        return -1;

    // Where two LFOs reach one knob the bar is split into one lane each, in LFO
    // order, so which lane belongs to which LFO never moves as amounts change.
    std::vector<int> lanes;
    for (int lfo = 0; lfo < spacedust::numLfos; ++lfo)
        if (matrix.amountFor (lfo, destination) != 0.0f)
            lanes.push_back (lfo);

    if (lanes.empty())
        return -1;

    const auto point = position.toFloat();

    for (size_t i = 0; i < lanes.size(); ++i)
        if (laneBoundsFor ((int) i, (int) lanes.size()).contains (point))
            return lanes[i];

    // Floating-point rounding can leave the rightmost pixel column just
    // outside every lane's bounds; treat it as the last lane rather than miss
    // the click, since bar.contains() above already confirmed it is on the bar.
    return lanes.back();
}

void ModulatableKnob::mouseDown (const juce::MouseEvent& event)
{
    // In assign mode the active LFO always wins, even on the bar's own lanes:
    // the player's press means "assign this LFO", and letting a narrow strip
    // silently redirect the gesture to whichever LFO already owns that pixel
    // would be a trap. Editing an existing lane by pressing the bar directly
    // is only what happens when assign mode is off.
    // The remove-x first, because it sits inside the area a drag would otherwise
    // claim. A press here deletes the routing and starts no drag -- otherwise
    // the same gesture would remove it and immediately begin setting a new one.
    if (removeOffered() && removeArea().contains (event.getPosition()))
    {
        if (mode.activeLfo() >= 0)
        {
            // Assigning: take off the one you are working with, and leave any
            // others on this knob alone.
            matrix.clearRouting (mode.activeLfo(), destination);
        }
        else
        {
            // Not assigning: there is no "the one you are working with", so the
            // x means what it looks like -- take this knob's modulation off.
            // With a single routing, which is the ordinary case, the two
            // branches do the same thing.
            for (int lfo = 0; lfo < spacedust::numLfos; ++lfo)
                matrix.clearRouting (lfo, destination);
        }

        if (routingChanged)
            routingChanged();

        repaint();
        return;
    }

    if (mode.activeLfo() >= 0)
        dragLfo = mode.activeLfo();
    else
        dragLfo = lfoLaneAt (event.getPosition());

    if (dragLfo < 0)
        return;

    // This knob now wears the focus marks, and whichever one had them loses
    // them. The state broadcasts, so both repaint.
    mode.setFocusedDestination (destination);

    dragging = true;
    dragStartAmount = matrix.amountFor (dragLfo, destination);
}

void ModulatableKnob::mouseDrag (const juce::MouseEvent& event)
{
    if (! dragging || dragLfo < 0)
        return;

    // Up is positive. getDistanceFromDragStartY grows downwards, so it is negated.
    const float delta = -(float) event.getDistanceFromDragStartY() / dragPixelsForFullRange;
    const float amount = juce::jlimit (-1.0f, 1.0f, dragStartAmount + delta);

    // An amount of zero removes the routing, so dragging back through the middle
    // undoes an assignment rather than leaving a dead entry behind.
    matrix.setRouting (dragLfo, destination, amount);

    if (routingChanged)
        routingChanged();

    repaint();
}

void ModulatableKnob::mouseUp (const juce::MouseEvent&)
{
    dragging = false;
    dragLfo = -1;
    repaint();
}

void ModulatableKnob::setLfoState (const float* phases01, const float* outputs01)
{
    bool changed = false;

    for (int i = 0; i < spacedust::numLfos; ++i)
    {
        if (phases[i] != phases01[i])
        {
            phases[i] = phases01[i];
            changed = true;
        }

        if (outputs[i] != outputs01[i])
        {
            outputs[i] = outputs01[i];
            changed = true;
        }
    }

    // Only repaint when a bar is actually drawn here. The timer runs at 30 Hz
    // and there are about 150 of these.
    if (changed && matrix.hasAnyRouting (destination))
        repaint (barArea());
}

void ModulatableKnob::resized()
{
}

void ModulatableKnob::paint (juce::Graphics& g)
{
    const int assigning = mode.activeLfo();

    // -- the highlight, while assign mode is on --
    //
    // A soft wash over the knob rather than a rectangle drawn around it. The
    // outline read as a box bolted on beside the control; a tint reads as the
    // knob itself being lit, which is what "these knobs are assignable" should
    // look like (Giuseppe, 2026-08-31).
    if (assigning >= 0)
    {
        auto cell = knobArea().toFloat().reduced (1.0f);
        const auto colour = spacedust::AssignModeState::colourFor (assigning);

        // Three states, told apart by weight alone -- no outlines, since an
        // outline is the box that was removed.
        //
        //   assignable, empty       a faint wash: "you could put one here"
        //   already carries this    a much stronger wash: "this one is taken"
        //   focused                 the above, plus corner marks
        //
        // Before this, every assignable knob wore the identical wash, so a knob
        // already carrying a routing looked exactly like an empty one and the
        // only cues were the x and the bar (Giuseppe, 2026-08-31).
        const bool alreadyRouted = matrix.amountFor (assigning, destination) != 0.0f;

        g.setColour (colour.withAlpha (alreadyRouted ? 0.42f : 0.14f));
        g.fillRoundedRectangle (cell, 4.0f);

        // -- corner marks on the knob being edited --
        //
        // On the KNOB, not on the LFO panel. The panel version was invisible
        // exactly when it was wanted: assigning happens on Main and Effects,
        // and the panels are on Modulation.
        if (mode.focusedDestination() == destination)
        {
            const float armLen = juce::jmin (10.0f, juce::jmin (cell.getWidth(), cell.getHeight()) * 0.30f);
            const float cx = cell.getX(), cy = cell.getY(), cw = cell.getWidth(), ch = cell.getHeight();

            juce::Path corners;
            corners.startNewSubPath (cx, cy + armLen);            corners.lineTo (cx, cy);            corners.lineTo (cx + armLen, cy);
            corners.startNewSubPath (cx + cw - armLen, cy);       corners.lineTo (cx + cw, cy);       corners.lineTo (cx + cw, cy + armLen);
            corners.startNewSubPath (cx, cy + ch - armLen);       corners.lineTo (cx, cy + ch);       corners.lineTo (cx + armLen, cy + ch);
            corners.startNewSubPath (cx + cw - armLen, cy + ch);  corners.lineTo (cx + cw, cy + ch);  corners.lineTo (cx + cw, cy + ch - armLen);

            const auto stroke = juce::PathStrokeType (2.0f, juce::PathStrokeType::curved,
                                                      juce::PathStrokeType::rounded);

            g.setColour (colour.withAlpha (0.30f));
            g.strokePath (corners, juce::PathStrokeType (4.5f, juce::PathStrokeType::curved,
                                                         juce::PathStrokeType::rounded));

            g.setColour (colour.brighter (0.5f));
            g.strokePath (corners, stroke);
        }

    }

    // -- the bar, whether or not assign mode is on --
    std::vector<int> lanes;
    for (int lfo = 0; lfo < spacedust::numLfos; ++lfo)
        if (matrix.amountFor (lfo, destination) != 0.0f)
            lanes.push_back (lfo);

    if (! lanes.empty())
    {
        for (size_t i = 0; i < lanes.size(); ++i)
        {
            const int lfo = lanes[i];
            const float amount = matrix.amountFor (lfo, destination);
            const auto colour = spacedust::AssignModeState::colourFor (lfo);

            auto lane = laneBoundsFor ((int) i, (int) lanes.size());

            g.setColour (colour.withAlpha (0.20f));
            g.fillRect (lane);

            // The lit zone is SYMMETRIC about the knob's own value, because an
            // LFO swings a knob both ways: at +50% it pushes half a range up AND
            // half a range down. Filling only upward said the modulation went one
            // direction, which was simply wrong (Giuseppe, 2026-08-31).
            //
            // The sign of amount therefore does not change the zone -- it flips
            // which way the marker travels, which the marker itself shows.
            const bool horizontal = isHorizontalControl();

            const float mid = horizontal ? lane.getCentreX() : lane.getCentreY();
            const float span = horizontal ? lane.getWidth() : lane.getHeight();
            const float reach = span * 0.5f * std::abs (amount);

            g.setColour (colour.withAlpha (0.55f));

            if (horizontal)
                g.fillRect (lane.withX (mid - reach).withWidth (reach * 2.0f));
            else
                g.fillRect (lane.withY (mid - reach).withHeight (reach * 2.0f));

            // Where the knob is being pushed RIGHT NOW, inside that zone and
            // never outside it.
            //
            // Driven by the LFO's output, not its phase. For a sine, phase 0.25
            // is the top of the swing rather than a quarter of the way up it, so
            // a marker drawn from the phase would sit in the wrong place for
            // every waveform except a ramp.
            //
            // Depth reaches 2.0, so the product can leave the zone; clamping is
            // what keeps the promise that the marker stays inside the highlight.
            const float pushed = juce::jlimit (-1.0f, 1.0f, amount * outputs[lfo]);

            g.setColour (colour);

            if (horizontal)
            {
                // Positive pushes RIGHT, which is where a positive pan goes.
                const float x = mid + reach * pushed;
                g.fillRect (x - 1.0f, lane.getY(), 2.0f, lane.getHeight());
            }
            else
            {
                // Positive pushes UP, matching a knob turning clockwise.
                const float y = mid - reach * pushed;
                g.fillRect (lane.getX(), y - 1.0f, lane.getWidth(), 2.0f);
            }
        }
    }

    // -- the remove x, whether or not assign mode is on --
    //
    // Outside the mode it wears the colour of the LFO it would remove; with
    // several on one knob it wears the first, and removes them all.
    if (removeOffered())
    {
        const int forLfo = mode.activeLfo() >= 0 ? mode.activeLfo() : firstRoutedLfo();
        const auto xColour = spacedust::AssignModeState::colourFor (forLfo);

        auto box = removeArea().toFloat().reduced (1.0f);

        g.setColour (juce::Colours::black.withAlpha (0.55f));
        g.fillRoundedRectangle (box, 3.0f);

        g.setColour (xColour.withAlpha (0.75f));
        g.drawRoundedRectangle (box, 3.0f, 1.0f);

        // The stroke, drawn rather than typed: a glyph at this size lands on
        // whatever the font decides and will not centre reliably.
        auto arm = box.reduced (box.getWidth() * 0.30f);
        g.setColour (xColour.brighter (0.4f));
        g.drawLine (arm.getX(), arm.getY(), arm.getRight(), arm.getBottom(), 1.6f);
        g.drawLine (arm.getX(), arm.getBottom(), arm.getRight(), arm.getY(), 1.6f);
    }

    // -- the percentage, only while a drag is running --
    if (dragging && dragLfo >= 0)
    {
        const int percent = juce::roundToInt (matrix.amountFor (dragLfo, destination) * 100.0f);
        const auto text = juce::String (percent) + "%";

        auto label = knobArea().removeFromTop (14).removeFromRight (46).translated (-barWidth, 0);

        g.setColour (juce::Colours::black.withAlpha (0.70f));
        g.fillRoundedRectangle (label.toFloat(), 3.0f);
        g.setColour (spacedust::AssignModeState::colourFor (dragLfo));
        g.setFont (11.0f);
        g.drawText (text, label, juce::Justification::centred);
    }
}

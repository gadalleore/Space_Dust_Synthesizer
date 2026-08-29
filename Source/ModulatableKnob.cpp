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
    setBounds (getWrappedBounds());
    toFront (false);
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
    return getLocalBounds().withTrimmedRight (barGap).removeFromRight (barWidth);
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
    if (mode.activeLfo() >= 0)
        dragLfo = mode.activeLfo();
    else
        dragLfo = lfoLaneAt (event.getPosition());

    if (dragLfo < 0)
        return;

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

void ModulatableKnob::setLfoPhases (const float* phases01)
{
    bool changed = false;

    for (int i = 0; i < spacedust::numLfos; ++i)
    {
        if (phases[i] != phases01[i])
        {
            phases[i] = phases01[i];
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

    // -- the ring, while assign mode is on --
    if (assigning >= 0)
    {
        // The whole cell, because the bar now lives INSIDE this rectangle
        // rather than beside it -- trimming the ring back by the bar's width
        // would cut it across the round knob it is meant to enclose.
        auto ring = getLocalBounds().toFloat().reduced (1.0f);
        g.setColour (spacedust::AssignModeState::colourFor (assigning).withAlpha (0.85f));
        g.drawRoundedRectangle (ring, 4.0f, 2.0f);
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

            // The filled part is the amount, measured from the middle, so a
            // negative amount reads as clearly as a positive one.
            const float mid = lane.getCentreY();
            const float reach = lane.getHeight() * 0.5f * std::abs (amount);

            g.setColour (colour.withAlpha (0.55f));
            g.fillRect (lane.withY (amount >= 0.0f ? mid - reach : mid)
                            .withHeight (reach));

            // Where the LFO is in its cycle right now.
            const float y = lane.getBottom() - lane.getHeight() * phases[lfo];
            g.setColour (colour);
            g.fillRect (lane.getX(), y - 1.0f, lane.getWidth(), 2.0f);
        }
    }

    // -- the percentage, only while a drag is running --
    if (dragging && dragLfo >= 0)
    {
        const int percent = juce::roundToInt (matrix.amountFor (dragLfo, destination) * 100.0f);
        const auto text = juce::String (percent) + "%";

        auto label = getLocalBounds().removeFromTop (14).removeFromRight (46).translated (-barWidth, 0);

        g.setColour (juce::Colours::black.withAlpha (0.70f));
        g.fillRoundedRectangle (label.toFloat(), 3.0f);
        g.setColour (spacedust::AssignModeState::colourFor (dragLfo));
        g.setFont (11.0f);
        g.drawText (text, label, juce::Justification::centred);
    }
}

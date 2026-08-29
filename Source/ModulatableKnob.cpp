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
}

ModulatableKnob::~ModulatableKnob()
{
    mode.removeChangeListener (this);
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
    return getLocalBounds().removeFromRight (barWidth);
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
        auto ring = getLocalBounds().withTrimmedRight (barWidth + barGap).toFloat().reduced (1.0f);
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

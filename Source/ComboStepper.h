#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

/**
    A pair of little arrows that step a ComboBox one item at a time.

    WHY IT PLACES ITSELF

    Every dropdown in the plugin gets one, and the layouts they sit in are dense,
    hand-tuned and were written before this existed. Editing forty of them to
    make room would be forty chances to move something by a pixel -- and moving
    something by a pixel in this panel is exactly how a layout gets broken.

    So a stepper is never given bounds by a layout. It listens to its combo box
    and lays ITSELF over the combo's left edge whenever that combo moves or
    resizes. The LookAndFeel indents the combo's text by the same width, so the
    arrows sit in space the text has given up rather than space taken from a
    neighbour. Nothing around the combo moves, and no resized() has to know.

    A press on an arrow steps the selection and does NOT open the menu: the
    stepper is in front of the combo, so it takes the click first.

    Stepping skips nothing and wraps nowhere. At the last item the down arrow
    does nothing, which is what a list of twenty-one shapes wants -- wrapping
    round to Sine while hunting through them would lose the player's place.
*/
class ComboStepper : public juce::Component,
                     private juce::ComponentListener
{
public:
    /** How wide the strip of arrows is, and how far it sits from the box. */
    static constexpr int stripWidth = 12;
    static constexpr int gapToBox = 2;

    ComboStepper() { setInterceptsMouseClicks (true, false); }

    ~ComboStepper() override
    {
        if (box != nullptr)
            box->removeComponentListener (this);
    }

    /** Attach to a combo box, and follow it from then on.

        The stepper adds itself to the combo's parent, so a caller needs no
        knowledge of where the combo lives. Safe to call with the combo already
        laid out or not yet. */
    void attachTo (juce::ComboBox& comboToStep)
    {
        if (box != nullptr)
            box->removeComponentListener (this);

        box = &comboToStep;
        box->addComponentListener (this);

        if (auto* parent = box->getParentComponent())
            parent->addAndMakeVisible (this);

        followBox();
    }

    void paint (juce::Graphics& g) override
    {
        auto area = getLocalBounds().toFloat();
        const auto half = area.getHeight() * 0.5f;

        auto upper = area.removeFromTop (half).reduced (3.0f, 2.5f);
        auto lower = area.reduced (3.0f, 2.5f);

        drawArrow (g, upper, true, hoveredHalf == 1);
        drawArrow (g, lower, false, hoveredHalf == 2);
    }

    void mouseDown (const juce::MouseEvent& event) override
    {
        step (event.position.y < (float) getHeight() * 0.5f ? -1 : 1);
    }

    void mouseMove (const juce::MouseEvent& event) override
    {
        const int half = event.position.y < (float) getHeight() * 0.5f ? 1 : 2;

        if (half != hoveredHalf)
        {
            hoveredHalf = half;
            repaint();
        }
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        hoveredHalf = 0;
        repaint();
    }

private:
    /** Move the selection by one item, skipping anything the combo has disabled
        or is using as a separator or heading -- those have no id of their own and
        cannot be selected. */
    void step (int direction)
    {
        if (box == nullptr)
            return;

        const int count = box->getNumItems();
        int index = box->getSelectedItemIndex();

        for (int i = index + direction; i >= 0 && i < count; i += direction)
        {
            if (box->getItemId (i) != 0 && box->isItemEnabled (box->getItemId (i)))
            {
                // sendNotificationSync, so the parameter and everything watching
                // it move with the same gesture a menu pick would have made.
                box->setSelectedItemIndex (i, juce::sendNotificationSync);
                return;
            }
        }
    }

    void drawArrow (juce::Graphics& g, juce::Rectangle<float> area, bool pointsUp, bool lit)
    {
        juce::Path arrow;

        if (pointsUp)
        {
            arrow.startNewSubPath (area.getCentreX(), area.getY());
            arrow.lineTo (area.getRight(), area.getBottom());
            arrow.lineTo (area.getX(), area.getBottom());
        }
        else
        {
            arrow.startNewSubPath (area.getCentreX(), area.getBottom());
            arrow.lineTo (area.getRight(), area.getY());
            arrow.lineTo (area.getX(), area.getY());
        }

        arrow.closeSubPath();

        // The same light blue the panel's labels use, brightening under the
        // pointer so an arrow says it can be pressed.
        g.setColour (lit ? juce::Colour (0xff00d4ff) : juce::Colour (0xff5a7a94));
        g.fillPath (arrow);
    }

    /** Sit just OUTSIDE the combo's left edge, in the parent's coordinates.

        Outside rather than over it: arrows inside the box read as part of the
        menu and crowd the name, which on a list of twenty-one shapes is the one
        thing that has to stay readable.

        If there is no room to the left -- a combo hard against its parent's edge
        -- the strip sits at the parent's edge instead of being pushed off it. */
    void followBox()
    {
        if (box == nullptr)
            return;

        const auto b = box->getBounds();
        const int x = juce::jmax (0, b.getX() - stripWidth - gapToBox);

        setBounds (x, b.getY() + 2, stripWidth, juce::jmax (8, b.getHeight() - 4));
        toFront (false);
    }

    void componentMovedOrResized (juce::Component&, bool, bool) override { followBox(); }

    void componentBeingDeleted (juce::Component& c) override
    {
        if (&c == box)
        {
            box->removeComponentListener (this);
            box = nullptr;
        }
    }

    juce::ComboBox* box = nullptr;

    /** 0 for neither, 1 for the up arrow, 2 for the down arrow. */
    int hoveredHalf = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ComboStepper)
};

#pragma once

#include "ModMatrix.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace spacedust
{
    /** Which LFO is being assigned, and who is told when that changes.

        One of these lives in the editor. Every ModulatableKnob listens to it, so
        entering assign mode lights up every knob at once without the editor
        holding a list of them. */
    class AssignModeState : public juce::ChangeBroadcaster
    {
    public:
        /** -1 when assign mode is off. */
        int activeLfo() const noexcept { return active; }

        void setActiveLfo (int lfo)
        {
            const int clamped = (lfo >= 0 && lfo < numLfos) ? lfo : -1;

            if (clamped == active)
                return;

            active = clamped;
            sendChangeMessage();
        }

        /** Each LFO owns a colour, so which LFO holds a knob is readable at a
            glance once there are four of them. LFO 1 is the blue Giuseppe asked
            for; the other three are picked to stay apart on a dark background. */
        static juce::Colour colourFor (int lfo)
        {
            switch (lfo)
            {
                case 0:  return juce::Colour (0xff4aa3ff);  // blue
                case 1:  return juce::Colour (0xff4ad991);  // green
                case 2:  return juce::Colour (0xffffb347);  // amber
                case 3:  return juce::Colour (0xffe07aff);  // magenta
                default: return juce::Colours::grey;
            }
        }

        /** Which knob was last clicked or dragged while assigning, by parameter
            id, or empty for none.

            Lives here rather than in the knob because exactly one knob may wear
            the focus marks at a time, and a knob cannot know that another one
            has taken them. Every ModulatableKnob already listens to this object,
            so setting it repaints the one gaining focus and the one losing it. */
        const std::string& focusedDestination() const noexcept { return focused; }

        void setFocusedDestination (std::string destination)
        {
            if (destination == focused)
                return;

            focused = std::move (destination);
            sendChangeMessage();
        }

    private:
        int active = -1;
        std::string focused;
    };
}

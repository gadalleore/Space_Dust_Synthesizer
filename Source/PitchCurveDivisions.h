#pragma once

namespace spacedust
{
    /** The pitch curve's tempo-synced lengths: one list, three readers.

        The pitchCurveDivision PARAMETER is built from the names, the synced
        duration in processBlock() is worked out from the beats, and the pitch
        curve plot draws its vertical grid from the beats as well. Those three
        lived in two files before this header existed, with a comment in each
        asking the next reader to keep them in step -- which is a promise a
        comment cannot keep. Now there is nothing to keep in step.

        ORDER IS SLOW TO FAST. Turning the knob up shortens the shape, the same
        way turning a Rate knob up speeds an LFO up: the number goes up, the
        thing happens more often. The reverse order read backwards on the knob.

        "1/1" is one BAR, matching how the LFO sync list reads, and the beats
        assume 4/4 as every other sync path in the plugin does. */
    inline constexpr int numPitchCurveDivisions = 9;

    inline constexpr const char* pitchCurveDivisionNames[numPitchCurveDivisions]
    {
        "8/1", "4/1", "2/1", "1/1", "1/2", "1/4", "1/8", "1/16", "1/32"
    };

    inline constexpr double pitchCurveDivisionBeats[numPitchCurveDivisions]
    {
        32.0, 16.0, 8.0, 4.0, 2.0, 1.0, 0.5, 0.25, 0.125
    };

    /** One bar. The default, and the index the parameter is created with. */
    inline constexpr int defaultPitchCurveDivision = 3;

    static_assert (pitchCurveDivisionBeats[defaultPitchCurveDivision] == 4.0,
                   "the default division must still be one bar of 4/4");
}

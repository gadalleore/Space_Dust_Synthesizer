#pragma once

#include <vector>

namespace spacedust
{
    /** A pitch shape drawn by hand, in semitones over time.

        Replaces the three Pitch Env knobs, which could only make one straight
        fall. Free of JUCE for the same reason ModMatrix is.

        Time is 0..1 across whatever the Time knob is set to. Value is
        semitones, and the editor limits it to -24..+24.

        Outside the drawn range the nearest end value is HELD. That is what makes
        a curve drawn from some pitch back to zero behave exactly as the old ramp
        did, so a migrated patch sounds the same. */
    class PitchCurve
    {
    public:
        struct Point
        {
            float t01 = 0.0f;
            float semitones = 0.0f;
        };

        void clear();

        /** Points are kept sorted by time, so drawing right to left works. */
        void addPoint (float t01, float semitones);

        int pointCount() const noexcept { return (int) points.size(); }

        Point pointAt (int index) const { return points[(size_t) index]; }

        /** Straight between points, held past either end. */
        float valueAt (float t01) const noexcept;

        /** No points, or every point at zero. A flat curve is skipped entirely
            in the voice, so a patch that draws nothing costs nothing. */
        bool isFlat() const noexcept;

    private:
        std::vector<Point> points;
    };
}

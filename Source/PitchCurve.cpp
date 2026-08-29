#include "PitchCurve.h"

#include <algorithm>
#include <cmath>

namespace spacedust
{
    void PitchCurve::clear()
    {
        points.clear();
    }

    void PitchCurve::addPoint (float t01, float semitones)
    {
        const float t = t01 < 0.0f ? 0.0f : (t01 > 1.0f ? 1.0f : t01);

        points.push_back (Point { t, semitones });

        std::sort (points.begin(), points.end(),
                   [] (const Point& a, const Point& b) { return a.t01 < b.t01; });
    }

    bool PitchCurve::isFlat() const noexcept
    {
        for (const auto& p : points)
            if (p.semitones != 0.0f)
                return false;

        return true;
    }

    float PitchCurve::valueAt (float t01) const noexcept
    {
        if (points.empty())
            return 0.0f;

        if (t01 <= points.front().t01)
            return points.front().semitones;

        if (t01 >= points.back().t01)
            return points.back().semitones;

        for (size_t i = 1; i < points.size(); ++i)
        {
            const auto& a = points[i - 1];
            const auto& b = points[i];

            if (t01 > b.t01)
                continue;

            const float span = b.t01 - a.t01;

            if (span <= 0.0f)
                return b.semitones;

            const float f = (t01 - a.t01) / span;
            return a.semitones + (b.semitones - a.semitones) * f;
        }

        return points.back().semitones;
    }
}

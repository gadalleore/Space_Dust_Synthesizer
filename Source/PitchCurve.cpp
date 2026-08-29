#include "PitchCurve.h"

#include <algorithm>
#include <cmath>

namespace spacedust
{
    namespace
    {
        float clampT01 (float t) noexcept
        {
            return t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
        }

        float clampSemitones (float s) noexcept
        {
            return s < -24.0f ? -24.0f : (s > 24.0f ? 24.0f : s);
        }
    }

    void PitchCurve::clear()
    {
        points.clear();
        publish();
    }

    void PitchCurve::addPoint (float t01, float semitones)
    {
        // Defence in depth: see maxPoints. The editor never sends more than
        // this many, but a hand-edited or corrupt save file might.
        if ((int) points.size() >= maxPoints)
            return;

        // Not clamped to +/-24 here: this is also how the migration path
        // rebuilds the old three-knob ramp (up to +/-48 semitones), and that
        // must reproduce the old sound bit for bit. +/-24 is the DRAWING
        // limit the editor enforces on its own new points, via setPoints().
        points.push_back (Point { clampT01 (t01), semitones });

        std::sort (points.begin(), points.end(),
                   [] (const Point& a, const Point& b) { return a.t01 < b.t01; });

        publish();
    }

    void PitchCurve::setPoints (const std::vector<Point>& newPoints)
    {
        points.clear();
        points.reserve (std::min ((size_t) maxPoints, newPoints.size()));

        for (const auto& p : newPoints)
        {
            if ((int) points.size() >= maxPoints)
                break;

            points.push_back (Point { clampT01 (p.t01), clampSemitones (p.semitones) });
        }

        std::sort (points.begin(), points.end(),
                   [] (const Point& a, const Point& b) { return a.t01 < b.t01; });

        publish();
    }

    float PitchCurve::interpolate (const Point* pts, int count, float t01) noexcept
    {
        if (count == 0)
            return 0.0f;

        if (t01 <= pts[0].t01)
            return pts[0].semitones;

        if (t01 >= pts[count - 1].t01)
            return pts[count - 1].semitones;

        for (int i = 1; i < count; ++i)
        {
            const auto& a = pts[i - 1];
            const auto& b = pts[i];

            if (t01 > b.t01)
                continue;

            const float span = b.t01 - a.t01;

            if (span <= 0.0f)
                return b.semitones;

            const float f = (t01 - a.t01) / span;
            return a.semitones + (b.semitones - a.semitones) * f;
        }

        return pts[count - 1].semitones;
    }

    void PitchCurve::publish()
    {
        // Message thread. Straight rotation, never the buffer just published --
        // see the class comment for why three buffers cycled this way need no
        // reader announcement the way CompiledSet's do.
        const int live   = liveIndex.load (std::memory_order_relaxed);
        const int target = (live + 1) % numBuffers;

        auto& buf = buffers[target];
        buf.count = std::min ((int) points.size(), maxPoints);

        bool flat = true;
        for (int i = 0; i < buf.count; ++i)
        {
            buf.points[i] = points[(size_t) i];
            if (buf.points[i].semitones != 0.0f)
                flat = false;
        }
        buf.flat = flat;

        liveIndex.store (target, std::memory_order_release);
    }

    bool PitchCurve::isFlat() const noexcept
    {
        return buffers[liveIndex.load (std::memory_order_acquire)].flat;
    }

    float PitchCurve::valueAt (float t01) const noexcept
    {
        const int idx = liveIndex.load (std::memory_order_acquire);
        const auto& buf = buffers[idx];
        return interpolate (buf.points, buf.count, t01);
    }
}

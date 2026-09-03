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

        float clampBend (float b) noexcept
        {
            return b < -1.0f ? -1.0f : (b > 1.0f ? 1.0f : b);
        }
    }

    void PitchCurve::clear()
    {
        points.clear();
        publish();
    }

    void PitchCurve::addPoint (float t01, float semitones, float bend, float skew)
    {
        // Defence in depth: see maxPoints. The editor never sends more than
        // this many, but a hand-edited or corrupt save file might.
        if ((int) points.size() >= maxPoints)
            return;

        // Not clamped to +/-24 here: this is also how the migration path
        // rebuilds the old three-knob ramp (up to +/-48 semitones), and that
        // must reproduce the old sound bit for bit. +/-24 is the DRAWING
        // limit the editor enforces on its own new points, via setPoints().
        points.push_back (Point { clampT01 (t01), semitones,
                                  clampBend (bend), clampBend (skew) });

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

            points.push_back (Point { clampT01 (p.t01),
                                      clampSemitones (p.semitones),
                                      clampBend (p.bend),
                                      clampBend (p.skew) });
        }

        std::sort (points.begin(), points.end(),
                   [] (const Point& a, const Point& b) { return a.t01 < b.t01; });

        publish();
    }

    float PitchCurve::interpolate (const Point* pts, const float* bendK,
                                   const float* skewK, int count, float t01) noexcept
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

            // A SYMMETRIC bow: f + k.f.(1-f).
            //
            // The displacement from the straight line is k.f.(1-f), a parabola
            // that is zero at both ends and peaks exactly halfway between them.
            // So the line bulges about its own middle and both halves mirror
            // each other -- which is what bending a line looks like.
            //
            // An asymmetric ease (Schlick's bias, and every other "tension"
            // curve of that family) was tried first and is wrong for this: it
            // pushes the whole segment one way, so the line leaves one end
            // immediately and only meets the other at the last moment. That
            // reads as a shape that has been dragged, not bowed.
            //
            // At k = 0 this is exactly f, so a straight segment is bit-identical
            // to the plain lerp it replaced -- no branch needed to get there.
            // Two multiplies and an add, no divide and no pow: this runs once
            // per sample in EVERY voice, in every unison stack.
            //
            // The lean warps WHERE that bump peaks before the bump is applied.
            // w is the same shape of warp, so it too is zero at both ends and
            // monotonic -- it slides the middle of the segment towards one end
            // without moving the ends themselves. At s = 0, w is f and the two
            // lines below collapse back to the plain symmetric bow.
            //
            // Monotonic while |k|(1 + |s|) <= 1, which publish() guarantees by
            // scaling k down as the lean grows. Past that the line folds back
            // and the pitch would rise and fall inside one segment.
            const float s = skewK[i - 1];
            const float k = bendK[i - 1];

            const float w  = f + s * f * (1.0f - f);
            const float fb = f + k * w * (1.0f - w);

            return a.semitones + (b.semitones - a.semitones) * fb;
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

        // Turn each segment's bend into the coefficient interpolate() wants.
        //
        // Only the SIGN has to be decided, and it is decided here so it never
        // has to be decided per sample. Positive bend must always mean "the
        // middle of this segment sits ABOVE the straight line". The
        // displacement interpolate() applies is (end - start) * k * f(1-f), so
        // its direction depends on which way the segment runs: on a rising
        // segment a positive k lifts the middle, on a falling one it drops it.
        // Flipping the sign for falling segments makes the gesture mean the
        // same thing everywhere -- without it, dragging up would bow up on the
        // way up and bow down on the way down.
        for (int i = 0; i < maxPoints; ++i)
        {
            buf.bendK[i] = 0.0f;
            buf.skewK[i] = 0.0f;
        }

        for (int i = 0; i + 1 < buf.count; ++i)
        {
            const float bend = buf.points[i].bend;
            const float skew = buf.points[i].skew;

            if (bend == 0.0f)
                continue;   // no bow, so nothing for a lean to lean

            const bool rising = buf.points[i + 1].semitones >= buf.points[i].semitones;

            // The depth is divided by (1 + |skew|) for one reason: it makes
            // interpolate()'s monotonic condition, |k|(1 + |s|) <= 1, hold
            // exactly at the limits, given bend and skew are both already
            // clamped to [-1, 1] on the way in. So a fully leaned bow is half
            // as deep as a centred one -- the cost of a line that can never
            // fold back on itself, paid where it can be reasoned about rather
            // than discovered as an overshoot on a held note.
            buf.bendK[i] = (rising ? bend : -bend) / (1.0f + std::abs (skew));

            // NEGATED, and the sign is not obvious: interpolate() warps with
            // w = f + s.f(1-f), so a POSITIVE s makes w run ahead of f, which
            // means w reaches its own halfway point -- where the bow peaks --
            // at a SMALLER f. Positive s therefore pulls the peak EARLIER,
            // which is the opposite of what "lean towards the later point"
            // has to mean. Flipping it here keeps Point::skew reading the way
            // the gesture does. Caught by pitchcurve-test, which measures
            // where the peak actually lands rather than trusting this comment.
            buf.skewK[i] = -skew;
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
        return interpolate (buf.points, buf.bendK, buf.skewK, buf.count, t01);
    }
}

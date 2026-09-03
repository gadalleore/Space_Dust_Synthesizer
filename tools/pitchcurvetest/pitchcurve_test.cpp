// pitchcurve-test -- the drawn pitch shape's interpolation, checked by number.
//
// PitchCurve::valueAt() runs once per sample in EVERY voice, and since bending
// arrived it is no longer a plain straight line between points. The bend has
// three properties that are easy to state, easy to get subtly wrong, and
// impossible to confirm by looking at a plot:
//
//   1. a straight segment must still be EXACTLY linear -- not nearly,
//   2. a bow must be symmetric about the middle of its segment,
//   3. positive bend must lift the middle whether the segment rises or falls,
//   4. a lean must move the peak towards the end it leans to, and
//   5. nothing may fold back on itself: a rising segment must never dip.
//
// The last one is the one that matters most on a held note. A fold means the
// pitch rises and falls inside a single segment, which is not a bend, it is a
// warble -- and it is exactly what happens when a depth and a lean are both
// pushed to their limits without the scaling PitchCurve::publish() applies.
//
// No JUCE here, the same as notelock-test, so it builds in seconds.

#include "../../Source/PitchCurve.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace
{
    int failures = 0;

    void check (bool condition, const char* what)
    {
        if (! condition)
        {
            std::printf ("  FAILED: %s\n", what);
            ++failures;
        }
    }

    /** Fills a curve in place rather than returning one.

        PitchCurve holds the atomic that publishes its snapshots, so it cannot
        be copied or moved -- which is correct for the thing it is, and means a
        helper has to take it by reference. */
    void setCurve (spacedust::PitchCurve& c, float startSemis, float endSemis,
                   float bend, float skew)
    {
        c.setPoints ({ { 0.0f, startSemis, bend, skew },
                       { 1.0f, endSemis,   0.0f, 0.0f } });
    }

    /** How far the curve sits above the straight line joining the two ends. */
    float deviation (const spacedust::PitchCurve& c,
                     float startSemis, float endSemis, float t)
    {
        const float straight = startSemis + (endSemis - startSemis) * t;
        return c.valueAt (t) - straight;
    }

    /** Where the curve is furthest from the straight line, 0..1. */
    float peakPosition (const spacedust::PitchCurve& c,
                        float startSemis, float endSemis)
    {
        float bestT = 0.0f, bestAbs = -1.0f;

        for (int i = 1; i < 1000; ++i)
        {
            const float t = (float) i / 1000.0f;
            const float d = std::fabs (deviation (c, startSemis, endSemis, t));

            if (d > bestAbs)
            {
                bestAbs = d;
                bestT   = t;
            }
        }

        return bestT;
    }

    /** True when the curve never turns back on itself between the two ends. */
    bool isMonotonic (const spacedust::PitchCurve& c, bool rising)
    {
        float previous = c.valueAt (0.0f);

        for (int i = 1; i <= 2000; ++i)
        {
            const float v = c.valueAt ((float) i / 2000.0f);

            // A hair of slack for float noise; a real fold is orders larger.
            if (rising ? (v < previous - 1.0e-4f)
                       : (v > previous + 1.0e-4f))
                return false;

            previous = v;
        }

        return true;
    }
}

int main()
{
    std::printf ("Running pitch curve interpolation tests\n\n");

    //==========================================================================
    std::printf ("A straight segment is exactly linear:\n");
    {
        spacedust::PitchCurve c;
        setCurve (c, 0.0f, 12.0f, 0.0f, 0.0f);
        float worst = 0.0f;

        for (int i = 0; i <= 100; ++i)
        {
            const float t = (float) i / 100.0f;
            worst = std::fmax (worst, std::fabs (c.valueAt (t) - 12.0f * t));
        }

        std::printf ("  worst departure from the straight line: %.9f\n", worst);
        check (worst == 0.0f, "an unbent segment is not bit-exactly linear");
    }

    //==========================================================================
    std::printf ("\nA bow is symmetric about the middle of its segment:\n");
    {
        spacedust::PitchCurve c;
        setCurve (c, 0.0f, 12.0f, 1.0f, 0.0f);
        float worst = 0.0f;

        for (int i = 1; i < 50; ++i)
        {
            const float x    = (float) i / 100.0f;
            const float left  = deviation (c, 0.0f, 12.0f, 0.5f - x);
            const float right = deviation (c, 0.0f, 12.0f, 0.5f + x);

            worst = std::fmax (worst, std::fabs (left - right));
        }

        std::printf ("  worst mismatch between the two halves: %.9f\n", worst);
        check (worst < 1.0e-5f, "the two halves of a bow do not match");

        const float peak = peakPosition (c, 0.0f, 12.0f);
        std::printf ("  the bow peaks at t=%.3f (expect 0.500)\n", peak);
        check (std::fabs (peak - 0.5f) < 0.01f, "an unleaned bow does not peak in the middle");
    }

    //==========================================================================
    std::printf ("\nPositive bend lifts the middle, uphill AND downhill:\n");
    {
        spacedust::PitchCurve up, down;
        setCurve (up,   0.0f, 12.0f, 1.0f, 0.0f);
        setCurve (down, 12.0f, 0.0f, 1.0f, 0.0f);

        const float dUp   = deviation (up,   0.0f, 12.0f, 0.5f);
        const float dDown = deviation (down, 12.0f, 0.0f, 0.5f);

        std::printf ("  rising segment, middle sits %+.4f st off the line\n", dUp);
        std::printf ("  falling segment, middle sits %+.4f st off the line\n", dDown);

        check (dUp   > 0.1f, "positive bend does not lift a rising segment");
        check (dDown > 0.1f, "positive bend does not lift a falling segment");

        // Same shape either way round -- the direction only decides the sign
        // that has to be applied to get there.
        check (std::fabs (dUp - dDown) < 1.0e-4f,
               "a bow is not the same size uphill as downhill");
    }

    //==========================================================================
    std::printf ("\nNegative bend drops the middle:\n");
    {
        spacedust::PitchCurve c;
        setCurve (c, 0.0f, 12.0f, -1.0f, 0.0f);
        const float d = deviation (c, 0.0f, 12.0f, 0.5f);

        std::printf ("  middle sits %+.4f st off the line\n", d);
        check (d < -0.1f, "negative bend does not drop the middle");
    }

    //==========================================================================
    std::printf ("\nA lean moves the peak towards the end it leans to:\n");
    {
        spacedust::PitchCurve centred, late, early;
        setCurve (centred, 0.0f, 12.0f, 1.0f,  0.0f);
        setCurve (late,    0.0f, 12.0f, 1.0f,  1.0f);
        setCurve (early,   0.0f, 12.0f, 1.0f, -1.0f);

        const float pc = peakPosition (centred, 0.0f, 12.0f);
        const float pl = peakPosition (late,    0.0f, 12.0f);
        const float pe = peakPosition (early,   0.0f, 12.0f);

        std::printf ("  leaning late  : peak at t=%.3f\n", pl);
        std::printf ("  centred       : peak at t=%.3f\n", pc);
        std::printf ("  leaning early : peak at t=%.3f\n", pe);

        check (pl > pc + 0.02f, "leaning towards the later point did not move the peak later");
        check (pe < pc - 0.02f, "leaning towards the earlier point did not move the peak earlier");
    }

    //==========================================================================
    std::printf ("\nThe ends stay put however hard it is bent:\n");
    {
        float worst = 0.0f;

        for (int b = -10; b <= 10; ++b)
            for (int s = -10; s <= 10; ++s)
            {
                spacedust::PitchCurve c;
                setCurve (c, 0.0f, 12.0f, (float) b / 10.0f, (float) s / 10.0f);

                worst = std::fmax (worst, std::fabs (c.valueAt (0.0f) -  0.0f));
                worst = std::fmax (worst, std::fabs (c.valueAt (1.0f) - 12.0f));
            }

        std::printf ("  worst end-point movement over 441 bend/lean pairs: %.9f\n", worst);
        check (worst < 1.0e-4f, "bending moved the points at the ends of the segment");
    }

    //==========================================================================
    std::printf ("\nNothing folds back on itself, at any bend and any lean:\n");
    {
        int folds = 0;

        for (int b = -10; b <= 10; ++b)
            for (int s = -10; s <= 10; ++s)
            {
                const float bend = (float) b / 10.0f;
                const float skew = (float) s / 10.0f;

                spacedust::PitchCurve rising, falling;
                setCurve (rising,  0.0f, 12.0f, bend, skew);
                setCurve (falling, 12.0f, 0.0f, bend, skew);

                if (! isMonotonic (rising,  true))  ++folds;
                if (! isMonotonic (falling, false)) ++folds;
            }

        std::printf ("  folds found over 882 rising and falling cases: %d (expect 0)\n", folds);
        check (folds == 0, "some bend and lean pair folds the line back on itself");
    }

    //==========================================================================
    std::printf ("\nOutside the drawn range the nearest end is held:\n");
    {
        spacedust::PitchCurve c;
        setCurve (c, 3.0f, 9.0f, 0.8f, 0.4f);

        std::printf ("  before the start: %.4f (expect 3.0000)\n", c.valueAt (-0.5f));
        std::printf ("  after the end   : %.4f (expect 9.0000)\n", c.valueAt (1.5f));

        check (std::fabs (c.valueAt (-0.5f) - 3.0f) < 1.0e-4f, "the value before the start is not held");
        check (std::fabs (c.valueAt ( 1.5f) - 9.0f) < 1.0e-4f, "the value after the end is not held");
    }

    //==========================================================================
    std::printf ("\nA bend survives being saved into a point and read back:\n");
    {
        spacedust::PitchCurve c;
        c.addPoint (0.0f, 0.0f, 0.6f, -0.3f);
        c.addPoint (1.0f, 5.0f);

        const auto p = c.pointAt (0);
        std::printf ("  bend %.4f (expect 0.6000), lean %.4f (expect -0.3000)\n", p.bend, p.skew);

        check (std::fabs (p.bend - 0.6f)  < 1.0e-6f, "the bend did not survive addPoint");
        check (std::fabs (p.skew + 0.3f)  < 1.0e-6f, "the lean did not survive addPoint");
    }

    //==========================================================================
    std::printf ("\n");

    if (failures == 0)
    {
        std::printf ("All pitch curve tests passed.\n");
        return 0;
    }

    std::printf ("%d pitch curve check(s) did not pass.\n", failures);
    return 1;
}

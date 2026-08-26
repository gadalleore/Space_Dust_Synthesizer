// =====================================================================
//  Phase shaper test
//  ---------------------------------------------------------------------
//  Exercises the REAL PhaseShaper -- Bend +, Bend -, Bend +/-, Spectrum
//  and Sync, the five amounts that reshape an oscillator's phase before a
//  waveform is read from it.
//
//  All five are independent knobs and any of them may be turned up
//  together, so the properties that matter are about COMBINING them: all
//  zero must give back exactly the untouched phase, the two opposed bends
//  must cancel when both are full, nothing may run backwards however they
//  are stacked, and the phase must never leave the cycle.
//
//  Build & run:
//      cmake --build build --config Release --target shaper-test
//      ./build/shaper_test/Release/shaper_test.exe
// =====================================================================
#include "../../Source/PhaseShaper.h"

#include <cmath>
#include <cstdio>
#include <string>

namespace
{
    int failures = 0;

    void check(bool ok, const std::string& what)
    {
        if (!ok)
        {
            // Never the string "error :" -- MSBuild reads that as a build failure.
            std::printf("  FAIL  %s\n", what.c_str());
            ++failures;
        }
    }

    constexpr int steps = 2048;

    using A = PhaseShaper::Amounts;

    // Every knob at the same value, for the stacking checks.
    A all(double v)
    {
        A a; a.bendPlus = v; a.bendMinus = v; a.bendPlusMinus = v; a.spectrum = v; a.sync = v;
        return a;
    }
}

int main()
{
    std::printf("Phase shaper tests\n==================\n");

    std::printf("\nEvery knob has a name:\n");
    {
        const int n = (int)(sizeof(PhaseShaper::knobNames) / sizeof(PhaseShaper::knobNames[0]));
        check(n == PhaseShaper::numKnobs, "the knob list and the name list disagree");
        std::printf("  %d shaping knobs, %d names (Sync is separate)\n",
                    PhaseShaper::numKnobs, n);
    }

    std::printf("\nEverything at zero changes nothing:\n");
    {
        // The most important property here. If an untouched oscillator did not
        // give back exactly its old phase, every existing preset would change the
        // moment these knobs appeared.
        A a;
        double worst = 0.0;

        for (int i = 0; i < steps; ++i)
        {
            const double p = (double)i / steps;
            worst = std::fmax(worst, std::abs(PhaseShaper::shapedPhase(p, a) - p));
        }

        check(worst < 1e-12, "an idle shaper moved the phase");
        check(!PhaseShaper::isActive(a), "an idle shaper reported itself active");
        std::printf("  worst movement: %.2e\n", worst);
    }

    std::printf("\nThe two opposed bends cancel:\n");
    {
        // Bend + and Bend - are inverse curves. Turned up together they must undo
        // each other -- which is only true because they are folded into one
        // exponent. Applied one after the other they would not have cancelled
        // exactly, and two knobs pointing opposite ways would have felt broken.
        double worst = 0.0;

        for (int amount = 1; amount <= 10; ++amount)
        {
            A a;
            a.bendPlus = amount / 10.0;
            a.bendMinus = amount / 10.0;

            for (int i = 0; i < steps; ++i)
            {
                const double p = (double)i / steps;
                worst = std::fmax(worst, std::abs(PhaseShaper::shapedPhase(p, a) - p));
            }
        }

        check(worst < 1e-9, "Bend + and Bend - do not cancel when both are up");
        std::printf("  worst residue across every matched pair: %.2e\n", worst);
    }

    std::printf("\nBend + and Bend - pull opposite ways:\n");
    {
        A plus;  plus.bendPlus = 1.0;
        A minus; minus.bendMinus = 1.0;

        const double vPlus = PhaseShaper::shapedPhase(0.5, plus);
        const double vMinus = PhaseShaper::shapedPhase(0.5, minus);

        check(vPlus < 0.5 - 0.05, "Bend + does not compress the front of the cycle");
        check(vMinus > 0.5 + 0.05, "Bend - does not stretch the front of the cycle");
        std::printf("  at the half-way point: Bend + %.4f, untouched 0.5000, Bend - %.4f\n",
                    vPlus, vMinus);
    }

    std::printf("\nNothing runs backwards, however the knobs are stacked:\n");
    {
        // A phase curve that dipped would play the wave in reverse for part of
        // every turn -- a stutter, not a timbre. This is the check that the four
        // knobs are safe to combine at all.
        int nonMonotonic = 0;

        for (int bp = 0; bp <= 4; ++bp)
        for (int bm = 0; bm <= 4; ++bm)
        for (int bpm = 0; bpm <= 4; ++bpm)
        {
            A a;
            a.bendPlus = bp / 4.0;
            a.bendMinus = bm / 4.0;
            a.bendPlusMinus = bpm / 4.0;

            double previous = -1.0;

            for (int i = 0; i < 512; ++i)
            {
                const double v = PhaseShaper::shapedPhase((double)i / 512, a);
                if (v < previous - 1e-9) ++nonMonotonic;
                previous = v;
            }
        }

        check(nonMonotonic == 0, "a stacked bend runs backwards somewhere");
        std::printf("  backward steps across 125 knob combinations: %d\n", nonMonotonic);
    }

    std::printf("\nThe phase never leaves the cycle:\n");
    {
        double lo = 1e9, hi = -1e9;

        for (int v = 0; v <= 10; ++v)
        {
            const A a = all(v / 10.0);

            for (int i = 0; i < 512; ++i)
            {
                const double x = PhaseShaper::shapedPhase((double)i / 512, a);
                lo = std::fmin(lo, x);
                hi = std::fmax(hi, x);
            }
        }

        check(lo >= -1e-9 && hi <= 1.0 + 1e-9, "the shaped phase left the cycle");
        std::printf("  range with every knob swept together: %.6f .. %.6f\n", lo, hi);
    }

    std::printf("\nEach bend still spans the whole cycle:\n");
    {
        // A curve that did not reach both ends would clip part of the wave off
        // permanently instead of redistributing it.
        struct Named { const char* name; A a; };

        A p; p.bendPlus = 1.0;
        A m; m.bendMinus = 1.0;
        A s; s.bendPlusMinus = 1.0;

        for (const auto& n : { Named{"Bend +", p}, Named{"Bend -", m}, Named{"Bend +/-", s} })
        {
            check(std::abs(PhaseShaper::shapedPhase(0.0, n.a)) < 1e-9,
                  std::string(n.name) + " does not start the cycle at zero");
            check(PhaseShaper::shapedPhase(0.999999, n.a) > 0.99,
                  std::string(n.name) + " does not reach the end of the cycle");
        }

        std::printf("  all three bends span 0..1\n");
    }

    std::printf("\nBend +/- keeps the middle of the cycle where it is:\n");
    {
        // The two halves bend opposite ways and meet at the centre, so the centre
        // must not move -- and the curve must not tear there either.
        A a; a.bendPlusMinus = 0.8;

        check(std::abs(PhaseShaper::shapedPhase(0.5, a) - 0.5) < 1e-9,
              "Bend +/- moved the middle of the cycle");

        const double justBefore = PhaseShaper::shapedPhase(0.5 - 1e-6, a);
        const double justAfter = PhaseShaper::shapedPhase(0.5 + 1e-6, a);

        check(std::abs(justAfter - justBefore) < 1e-3,
              "Bend +/- tears where its two halves meet");

        std::printf("  middle stays at %.6f, join step %.2e\n",
                    PhaseShaper::shapedPhase(0.5, a), std::abs(justAfter - justBefore));
    }

    std::printf("\nBend +/- is its own curve, not a copy of Bend +:\n");
    {
        // It was a copy once. Both halves bent away from the middle, which near
        // phase zero is exactly what Bend + does -- and the start of a cycle is
        // where a waveform's character lives, so the two sounded alike and one of
        // the four knobs was wasted.
        //
        // The first half is the test: under Bend + it runs EARLY (phases pulled
        // towards zero) and under Bend +/- it must run LATE.
        A plus; plus.bendPlus = 0.8;
        A both; both.bendPlusMinus = 0.8;

        double worstGap = 0.0;

        for (int i = 1; i < steps / 2; ++i)
        {
            const double p = (double)i / steps;
            const double a1 = PhaseShaper::shapedPhase(p, plus);
            const double a2 = PhaseShaper::shapedPhase(p, both);

            check(a2 > a1, "Bend +/- bends the first half the same way Bend + does");
            worstGap = std::fmax(worstGap, a2 - a1);
        }

        check(worstGap > 0.1, "Bend +/- is too close to Bend + to tell apart");
        std::printf("  widest difference across the first half: %.4f\n", worstGap);
    }

    std::printf("\nSync repeats the cycle and always restarts at zero:\n");
    {
        A a; a.sync = 1.0;
        int wraps = 0;
        double previous = PhaseShaper::shapedPhase(0.0, a);

        for (int i = 1; i < steps; ++i)
        {
            const double v = PhaseShaper::shapedPhase((double)i / steps, a);
            if (v < previous) ++wraps;
            previous = v;
        }

        check(wraps == (int)PhaseShaper::maxSyncRatio - 1,
              "full sync did not fit the expected number of cycles into the note");
        check(std::abs(PhaseShaper::shapedPhase(0.0, a)) < 1e-9,
              "a synced cycle does not start at zero");
        std::printf("  wraps across one note at full sync: %d\n", wraps);
    }

    std::printf("\nSync and a bend work together:\n");
    {
        // The bend must shape EACH repeat, not the run of them -- so a synced and
        // bent phase must still return to zero the same number of times.
        A a; a.sync = 1.0; a.bendPlus = 0.8;
        int wraps = 0;
        double previous = PhaseShaper::shapedPhase(0.0, a);

        for (int i = 1; i < steps; ++i)
        {
            const double v = PhaseShaper::shapedPhase((double)i / steps, a);
            if (v < previous) ++wraps;
            previous = v;
        }

        check(wraps == (int)PhaseShaper::maxSyncRatio - 1,
              "bending a synced phase changed how many times it repeats");
        std::printf("  wraps with Bend + at 0.8 and full sync: %d\n", wraps);
    }

    std::printf("\nSpectrum fades a shape to a sine and leaves the phase alone:\n");
    {
        A a; a.spectrum = 1.0;
        double worstPhase = 0.0;

        for (int i = 0; i < steps; ++i)
        {
            const double p = (double)i / steps;
            worstPhase = std::fmax(worstPhase, std::abs(PhaseShaper::shapedPhase(p, a) - p));
        }

        check(worstPhase < 1e-12, "Spectrum moved the phase, which it must not");

        double worstValue = 0.0;

        for (int i = 0; i < steps; ++i)
        {
            const double p = (double)i / steps;
            const double got = PhaseShaper::shapedValue(OscShape::Saw, p, a);
            worstValue = std::fmax(worstValue, std::abs(got - std::sin(OscShape::twoPi * p)));
        }

        check(worstValue < 1e-6, "Spectrum at full does not leave a sine");
        std::printf("  phase movement %.2e, saw-to-sine difference %.2e\n",
                    worstPhase, worstValue);
    }

    std::printf("\nisActive only says yes when something would change:\n");
    {
        A idle;
        check(!PhaseShaper::isActive(idle), "idle knobs reported active");

        A one;
        one.bendPlusMinus = 0.01;
        check(PhaseShaper::isActive(one), "a raised knob reported idle");

        A justSync;
        justSync.sync = 0.5;
        check(PhaseShaper::isActive(justSync), "sync alone reported idle");

        std::printf("  the cheap path is taken exactly when nothing would change\n");
    }

    std::printf("\nShaped values stay inside the rails, every shape and every knob:\n");
    {
        double worstPeak = 0.0;

        for (int s = 0; s < OscShape::numShapes; ++s)
        {
            const A a = all(0.75);

            for (int i = 0; i < 128; ++i)
            {
                const float v = PhaseShaper::shapedValue(s, (double)i / 128, a);
                check(std::isfinite(v), "a shaped value is not a number");
                worstPeak = std::fmax(worstPeak, std::abs((double)v));
            }
        }

        check(worstPeak <= 1.1, "a shaped value overshoots the rails");
        std::printf("  loudest shaped peak with every knob at 0.75: %.4f\n", worstPeak);
    }

    std::printf("\n%s\n", failures == 0 ? "All phase shaper tests passed."
                                        : "Some phase shaper tests FAILED.");
    return failures == 0 ? 0 : 1;
}

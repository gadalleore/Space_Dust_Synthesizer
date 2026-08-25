// =====================================================================
//  Phase shaper test
//  ---------------------------------------------------------------------
//  Exercises the REAL PhaseShaper -- Bend, Spectrum and Sync, the three
//  things done to an oscillator's phase before a waveform is read.
//
//  The properties checked here are the ones that make these controls
//  usable rather than merely present: turning a knob to zero must give
//  back exactly what there was before, a bend must never run backwards
//  (a phase that goes back on itself plays the wave in reverse for part
//  of the cycle), and the phase must stay inside 0..1 so it never reads
//  outside the wave.
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
}

int main()
{
    std::printf("Phase shaper tests\n==================\n");

    std::printf("\nEvery mode has a name:\n");
    {
        const int n = (int)(sizeof(PhaseShaper::modeNames) / sizeof(PhaseShaper::modeNames[0]));
        check(n == PhaseShaper::numModes, "the mode list and the name list disagree");
        std::printf("  %d modes, %d names\n", (int)PhaseShaper::numModes, n);
    }

    std::printf("\nZero intensity and zero sync change nothing:\n");
    {
        // The most important property here. If turning Intensity down did not
        // give back exactly the old phase, every existing preset would change
        // the moment these controls appeared.
        double worst = 0.0;

        for (int m = 0; m < PhaseShaper::numModes; ++m)
        {
            for (int i = 0; i < steps; ++i)
            {
                const double p = (double)i / steps;
                worst = std::fmax(worst, std::abs(PhaseShaper::shapedPhase(p, m, 0.0, 0.0) - p));
            }
        }

        check(worst < 1e-12, "a mode moved the phase with intensity and sync at zero");
        std::printf("  worst movement: %.2e\n", worst);
    }

    std::printf("\nStandard mode never bends, however hard it is pushed:\n");
    {
        double worst = 0.0;

        for (int i = 0; i < steps; ++i)
        {
            const double p = (double)i / steps;
            worst = std::fmax(worst,
                              std::abs(PhaseShaper::shapedPhase(p, PhaseShaper::Standard, 1.0, 0.0) - p));
        }

        check(worst < 1e-12, "Standard bent the phase");
        std::printf("  worst movement at full intensity: %.2e\n", worst);
    }

    std::printf("\nThe phase never leaves 0..1:\n");
    {
        // A phase outside the cycle reads outside the wave. shapeValue wraps, so
        // it would not crash -- it would quietly play the wrong part of the cycle.
        double lo = 1e9, hi = -1e9;

        for (int m = 0; m < PhaseShaper::numModes; ++m)
            for (int inten = 0; inten <= 10; ++inten)
                for (int sy = 0; sy <= 10; ++sy)
                    for (int i = 0; i < 256; ++i)
                    {
                        const double v = PhaseShaper::shapedPhase((double)i / 256, m,
                                                                  inten / 10.0, sy / 10.0);
                        lo = std::fmin(lo, v);
                        hi = std::fmax(hi, v);
                    }

        check(lo >= -1e-9 && hi <= 1.0 + 1e-9, "the shaped phase left the cycle");
        std::printf("  range across every mode and setting: %.6f .. %.6f\n", lo, hi);
    }

    std::printf("\nA bend never runs backwards:\n");
    {
        // A bend must be monotonic across the cycle. If it dipped, the wave would
        // play backwards for part of every turn -- which is not a timbre, it is a
        // stutter, and it would sound like a fault rather than an effect.
        int nonMonotonic = 0;

        for (int m = 0; m < PhaseShaper::numModes; ++m)
        {
            for (int inten = 1; inten <= 10; ++inten)
            {
                double previous = -1.0;

                for (int i = 0; i < steps; ++i)
                {
                    const double v = PhaseShaper::shapedPhase((double)i / steps, m,
                                                              inten / 10.0, 0.0);
                    if (v < previous - 1e-9)
                        ++nonMonotonic;

                    previous = v;
                }
            }
        }

        check(nonMonotonic == 0, "a bend curve runs backwards somewhere");
        std::printf("  backward steps found: %d\n", nonMonotonic);
    }

    std::printf("\nThe bends still start at zero and end at one:\n");
    {
        // A curve that did not span the whole cycle would clip part of the wave
        // off permanently rather than redistributing it.
        for (int m = 1; m <= PhaseShaper::BendPlusMinus; ++m)
        {
            const double atZero = PhaseShaper::shapedPhase(0.0, m, 1.0, 0.0);
            const double atEnd = PhaseShaper::shapedPhase(0.999999, m, 1.0, 0.0);

            check(std::abs(atZero) < 1e-9,
                  std::string(PhaseShaper::modeNames[m]) + " does not start the cycle at zero");
            check(atEnd > 0.99,
                  std::string(PhaseShaper::modeNames[m]) + " does not reach the end of the cycle");
        }
        std::printf("  all three bends span the whole cycle\n");
    }

    std::printf("\nBend + and Bend - pull opposite ways:\n");
    {
        // Halfway through the cycle, one must be earlier than untouched and the
        // other later. If they did not differ, two menu entries would do one job.
        const double plus = PhaseShaper::shapedPhase(0.5, PhaseShaper::BendPlus, 1.0, 0.0);
        const double minus = PhaseShaper::shapedPhase(0.5, PhaseShaper::BendMinus, 1.0, 0.0);

        check(plus < 0.5 - 0.05, "Bend + does not compress the front of the cycle");
        check(minus > 0.5 + 0.05, "Bend - does not stretch the front of the cycle");
        std::printf("  at the half-way point: Bend + %.4f, untouched 0.5000, Bend - %.4f\n",
                    plus, minus);
    }

    std::printf("\nBend +/- is symmetric about the middle:\n");
    {
        double worst = 0.0;

        for (int i = 1; i < steps; ++i)
        {
            const double p = (double)i / steps;
            const double a = PhaseShaper::shapedPhase(p, PhaseShaper::BendPlusMinus, 0.7, 0.0);
            const double b = PhaseShaper::shapedPhase(1.0 - p, PhaseShaper::BendPlusMinus, 0.7, 0.0);
            worst = std::fmax(worst, std::abs(a - (1.0 - b)));
        }

        check(worst < 1e-9, "Bend +/- is lopsided");
        std::printf("  worst asymmetry: %.2e\n", worst);
    }

    std::printf("\nSync repeats the cycle and always restarts at zero:\n");
    {
        // Full sync must fit maxSyncRatio cycles into one note, and each must
        // begin at zero -- that snap back to zero IS the hard-sync tear.
        int wraps = 0;
        double previous = PhaseShaper::shapedPhase(0.0, PhaseShaper::Standard, 0.0, 1.0);

        for (int i = 1; i < steps; ++i)
        {
            const double v = PhaseShaper::shapedPhase((double)i / steps,
                                                      PhaseShaper::Standard, 0.0, 1.0);
            if (v < previous) ++wraps;
            previous = v;
        }

        check(wraps == (int)PhaseShaper::maxSyncRatio - 1,
              "full sync did not fit the expected number of cycles into the note");

        const double atStart = PhaseShaper::shapedPhase(0.0, PhaseShaper::Standard, 0.0, 1.0);
        check(std::abs(atStart) < 1e-9, "a synced cycle does not start at zero");

        std::printf("  wraps across one note at full sync: %d\n", wraps);
    }

    std::printf("\nSpectrum fades a shape to a sine and leaves the phase alone:\n");
    {
        double worstPhase = 0.0;

        for (int i = 0; i < steps; ++i)
        {
            const double p = (double)i / steps;
            worstPhase = std::fmax(worstPhase,
                                   std::abs(PhaseShaper::shapedPhase(p, PhaseShaper::Spectrum, 1.0, 0.0) - p));
        }

        check(worstPhase < 1e-12, "Spectrum moved the phase, which it must not");

        // At full intensity a saw must have become a sine.
        double worstValue = 0.0;

        for (int i = 0; i < steps; ++i)
        {
            const double p = (double)i / steps;
            const double got = PhaseShaper::shapedValue(OscShape::Saw, p,
                                                        PhaseShaper::Spectrum, 1.0, 0.0);
            const double sine = std::sin(OscShape::twoPi * p);
            worstValue = std::fmax(worstValue, std::abs(got - sine));
        }

        check(worstValue < 1e-6, "Spectrum at full does not leave a sine");
        std::printf("  phase movement %.2e, saw-to-sine difference %.2e\n",
                    worstPhase, worstValue);
    }

    std::printf("\nisActive only says yes when something would change:\n");
    {
        check(!PhaseShaper::isActive(PhaseShaper::Standard, 0.0, 0.0), "idle settings reported active");
        check(!PhaseShaper::isActive(PhaseShaper::BendPlus, 0.0, 0.0), "zero intensity reported active");
        check(!PhaseShaper::isActive(PhaseShaper::Standard, 1.0, 0.0), "Standard reported active");
        check(PhaseShaper::isActive(PhaseShaper::BendPlus, 0.5, 0.0), "a real bend reported idle");
        check(PhaseShaper::isActive(PhaseShaper::Standard, 0.0, 0.5), "sync reported idle");
        std::printf("  the cheap path is taken exactly when nothing would change\n");
    }

    std::printf("\nShaped values stay inside the rails:\n");
    {
        double worstPeak = 0.0;

        for (int s = 0; s < OscShape::numShapes; ++s)
            for (int m = 0; m < PhaseShaper::numModes; ++m)
                for (int i = 0; i < 128; ++i)
                {
                    const float v = PhaseShaper::shapedValue(s, (double)i / 128, m, 0.75, 0.4);
                    check(std::isfinite(v), "a shaped value is not a number");
                    worstPeak = std::fmax(worstPeak, std::abs((double)v));
                }

        check(worstPeak <= 1.1, "a shaped value overshoots the rails");
        std::printf("  loudest shaped peak: %.4f\n", worstPeak);
    }

    std::printf("\n%s\n", failures == 0 ? "All phase shaper tests passed."
                                        : "Some phase shaper tests FAILED.");
    return failures == 0 ? 0 : 1;
}

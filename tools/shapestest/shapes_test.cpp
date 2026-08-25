// =====================================================================
//  Oscillator shape test
//  ---------------------------------------------------------------------
//  Exercises the REAL OscShape::shapeValue -- the one function both the
//  audio path and the Waveforms picture call.
//
//  Every check here has an answer decided before the code ran: a shape
//  must stay inside the rails, must close its cycle, must carry no DC,
//  and must actually be a shape rather than a flat line. The four
//  original shapes must ALSO be bit-for-bit what they always were, or
//  every preset ever saved comes back sounding different.
//
//  Build & run:
//      cmake --build build --config Release --target shapes-test
//      ./build/shapes_test/Release/shapes_test.exe
// =====================================================================
#include "../../Source/OscillatorShapes.h"

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

    constexpr double twoPi = 6.283185307179586;
    constexpr int steps = 4096;
}

int main()
{
    std::printf("Oscillator shape tests\n======================\n");

    std::printf("\nEvery shape has a name:\n");
    {
        const int nameCount = (int)(sizeof(OscShape::names) / sizeof(OscShape::names[0]));
        check(nameCount == OscShape::numShapes,
              "the name list and the shape count disagree");
        std::printf("  %d shapes, %d names\n", (int)OscShape::numShapes, nameCount);
    }

    std::printf("\nThe original four are untouched:\n");
    {
        // Rewritten here from the maths SynthVoice used BEFORE this change, so
        // this compares against history rather than against itself.
        double worst = 0.0;

        for (int i = 0; i < steps; ++i)
        {
            const double p = (double)i / steps;
            const double angle = twoPi * p;

            const double oldSine = std::sin(angle);
            const double norm = angle / twoPi;
            const double wrapped = norm - std::floor(norm + 0.5);
            const double oldTri = 4.0 * std::abs(wrapped) - 1.0;
            const double oldSaw = 2.0 * wrapped;
            const double oldSqr = std::sin(angle) > 0.0 ? 1.0 : -1.0;

            worst = std::fmax(worst, std::abs(OscShape::shapeValue(OscShape::Sine, p) - oldSine));
            worst = std::fmax(worst, std::abs(OscShape::shapeValue(OscShape::Triangle, p) - oldTri));
            worst = std::fmax(worst, std::abs(OscShape::shapeValue(OscShape::Saw, p) - oldSaw));
            worst = std::fmax(worst, std::abs(OscShape::shapeValue(OscShape::Square, p) - oldSqr));
        }

        check(worst < 1e-6, "a original shape changed value");
        std::printf("  worst difference against the old maths: %.2e\n", worst);
    }

    std::printf("\nNothing leaves the rails:\n");
    {
        double worstPeak = 0.0;
        int offender = -1;

        for (int s = 0; s < OscShape::numShapes; ++s)
        {
            for (int i = 0; i < steps; ++i)
            {
                const float v = OscShape::shapeValue(s, (double)i / steps);
                check(std::isfinite(v), std::string("shape ") + OscShape::names[s]
                                        + " produced a value that is not a number");

                if (std::abs((double)v) > worstPeak)
                {
                    worstPeak = std::abs((double)v);
                    offender = s;
                }
            }
        }

        // A little over 1.0 is tolerable -- the additive shapes are normalised by
        // a computed peak, not a measured one -- but a shape that reaches 1.1 is
        // loud enough to change the balance when it is selected.
        check(worstPeak <= 1.1,
              std::string("a shape overshoots the rails: ")
              + (offender >= 0 ? OscShape::names[offender] : "?"));
        std::printf("  loudest peak: %.4f (%s)\n", worstPeak,
                    offender >= 0 ? OscShape::names[offender] : "?");
    }

    std::printf("\nEvery shape is actually a shape:\n");
    {
        // A shape that never moves is a bug that is silent in both senses: it
        // makes no sound and it draws a flat line.
        for (int s = 0; s < OscShape::numShapes; ++s)
        {
            double lo = 1e9, hi = -1e9;

            for (int i = 0; i < steps; ++i)
            {
                const double v = OscShape::shapeValue(s, (double)i / steps);
                lo = std::fmin(lo, v);
                hi = std::fmax(hi, v);
            }

            check(hi - lo > 0.2, std::string("shape ") + OscShape::names[s]
                                 + " barely moves, so it would be silent");
        }
        std::printf("  all %d shapes swing\n", (int)OscShape::numShapes);
    }

    std::printf("\nNo shape carries DC:\n");
    {
        // DC in a waveform is a click at every note-on and a slow drift in the
        // filter. Half Sine is the one that has to be corrected for it, so this
        // is the check that proves the correction works.
        double worstDc = 0.0;
        int offender = -1;

        for (int s = 0; s < OscShape::numShapes; ++s)
        {
            double sum = 0.0;

            for (int i = 0; i < steps; ++i)
                sum += OscShape::shapeValue(s, (double)i / steps);

            const double dc = std::abs(sum / steps);

            if (dc > worstDc) { worstDc = dc; offender = s; }
        }

        // Pulse 25% and Pulse 12% are DC by definition -- a narrow pulse spends
        // most of the cycle at -1 and that IS the shape. They are excluded from
        // the tight bound and given a loose one.
        check(worstDc <= 0.76, std::string("a shape carries too much DC: ")
                               + (offender >= 0 ? OscShape::names[offender] : "?"));

        for (int s = 0; s < OscShape::numShapes; ++s)
        {
            if (s == OscShape::Pulse25 || s == OscShape::Pulse12)
                continue;

            double sum = 0.0;
            for (int i = 0; i < steps; ++i)
                sum += OscShape::shapeValue(s, (double)i / steps);

            check(std::abs(sum / steps) < 0.05,
                  std::string("shape ") + OscShape::names[s] + " carries DC");
        }

        std::printf("  worst DC (pulses excluded from the tight bound): %.4f (%s)\n",
                    worstDc, offender >= 0 ? OscShape::names[offender] : "?");
    }

    std::printf("\nThe phase wraps without a seam:\n");
    {
        // shapeValue takes its phase modulo 1, so asking for 1.25 and 0.25 must
        // give the same answer. If it did not, an oscillator whose phase ran past
        // 1 before wrapping would glitch.
        double worst = 0.0;

        for (int s = 0; s < OscShape::numShapes; ++s)
        {
            for (int i = 0; i < steps; ++i)
            {
                const double p = (double)i / steps;
                const double a = OscShape::shapeValue(s, p);
                const double b = OscShape::shapeValue(s, p + 3.0);
                worst = std::fmax(worst, std::abs(a - b));
            }
        }

        check(worst < 1e-5, "a shape gives a different answer for the same phase");
        std::printf("  worst difference across a wrap: %.2e\n", worst);
    }

    std::printf("\nThe angle form agrees with the phase form:\n");
    {
        double worst = 0.0;

        for (int s = 0; s < OscShape::numShapes; ++s)
        {
            for (int i = 0; i < steps; ++i)
            {
                const double p = (double)i / steps;
                const double a = OscShape::shapeValue(s, p);
                const double b = OscShape::shapeValueFromAngle(s, twoPi * p);
                worst = std::fmax(worst, std::abs(a - b));
            }
        }

        check(worst < 1e-6, "the angle form and the phase form disagree");
        std::printf("  worst difference: %.2e\n", worst);
    }

    std::printf("\nNo two shapes are the same shape:\n");
    {
        // Twenty-one entries in a dropdown, two of which sound identical, is a
        // bug the player finds and nobody else does.
        int duplicates = 0;

        for (int a = 0; a < OscShape::numShapes; ++a)
        {
            for (int b = a + 1; b < OscShape::numShapes; ++b)
            {
                double worst = 0.0;

                for (int i = 0; i < 512; ++i)
                {
                    const double p = (double)i / 512;
                    worst = std::fmax(worst,
                                      std::abs(OscShape::shapeValue(a, p)
                                               - OscShape::shapeValue(b, p)));
                }

                if (worst < 1e-4)
                {
                    ++duplicates;
                    std::printf("  FAIL  %s and %s are the same shape\n",
                                OscShape::names[a], OscShape::names[b]);
                    ++failures;
                }
            }
        }

        if (duplicates == 0)
            std::printf("  all %d are distinct\n", (int)OscShape::numShapes);
    }

    std::printf("\n%s\n", failures == 0 ? "All oscillator shape tests passed."
                                        : "Some oscillator shape tests FAILED.");
    return failures == 0 ? 0 : 1;
}

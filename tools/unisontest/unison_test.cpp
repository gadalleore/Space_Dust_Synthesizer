// =====================================================================
//  Unison spread test
//  ---------------------------------------------------------------------
//  Exercises the REAL Unison::layout -- how many copies an oscillator
//  runs, how fast each one goes, and where each one sits.
//
//  The properties checked here are the ones that make Voices usable
//  rather than merely present: one voice must be the untouched
//  oscillator, the copies must be symmetric about the note, turning
//  Detune or Width to zero must undo them exactly, and turning Voices up
//  must not change how loud the patch is.
//
//  Build & run:
//      cmake --build build --config Release --target unison-test
//      ./build/unison_test/Release/unison_test.exe
// =====================================================================
#include "../../Source/UnisonSpread.h"

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

    /** How loud the sum of a set of copies is, treating them as incoherent --
        which detuned copies are. Power adds, so amplitudes add in quadrature. */
    double summedPower(const Unison::Copy* c, int n, float compensation)
    {
        double left = 0.0, right = 0.0;

        for (int i = 0; i < n; ++i)
        {
            const double l = c[i].gainLeft * compensation;
            const double r = c[i].gainRight * compensation;
            left += l * l;
            right += r * r;
        }

        return left + right;
    }
}

int main()
{
    std::printf("Unison spread tests\n===================\n");

    Unison::Copy copies[Unison::maxVoices];

    std::printf("\nOne voice is the oscillator untouched:\n");
    {
        // The most important property here. If a single voice were not exactly
        // the old oscillator, every existing preset would change the moment this
        // control appeared.
        const float comp = Unison::layout(1, 1.0, 1.0, copies);

        check(copies[0].ratio == 1.0, "one voice was detuned");
        check(std::abs(copies[0].gainLeft - copies[0].gainRight) < 1e-9,
              "one voice was pushed off centre");
        check(std::abs(comp - 1.0f) < 1e-9, "one voice was scaled");

        std::printf("  ratio %.6f, gains %.4f/%.4f, compensation %.4f\n",
                    copies[0].ratio, copies[0].gainLeft, copies[0].gainRight, comp);
    }

    std::printf("\nZero detune leaves every copy at the note's own pitch:\n");
    {
        for (int v = 1; v <= Unison::maxVoices; ++v)
        {
            Unison::layout(v, 0.0, 1.0, copies);

            for (int i = 0; i < v; ++i)
                check(copies[i].ratio == 1.0,
                      "a copy was detuned with Detune at zero");
        }
        std::printf("  checked 1..%d voices\n", (int) Unison::maxVoices);
    }

    std::printf("\nZero width leaves every copy dead centre:\n");
    {
        for (int v = 1; v <= Unison::maxVoices; ++v)
        {
            Unison::layout(v, 1.0, 0.0, copies);

            for (int i = 0; i < v; ++i)
                check(std::abs(copies[i].gainLeft - copies[i].gainRight) < 1e-6,
                      "a copy was pushed off centre with Width at zero");
        }
        std::printf("  checked 1..%d voices\n", (int) Unison::maxVoices);
    }

    std::printf("\nThe copies are symmetric about the note:\n");
    {
        // Detune must spread evenly either side. If it did not, turning it up
        // would drag the perceived pitch of the patch off the note being played.
        for (int v = 2; v <= Unison::maxVoices; ++v)
        {
            Unison::layout(v, 1.0, 1.0, copies);

            for (int i = 0; i < v; ++i)
            {
                const int mirror = v - 1 - i;

                // A ratio and its mirror multiply to one: equal cents up and down.
                check(std::abs(copies[i].ratio * copies[mirror].ratio - 1.0) < 1e-9,
                      "the detune is lopsided");

                // And the pan mirrors too.
                check(std::abs(copies[i].gainLeft - copies[mirror].gainRight) < 1e-6,
                      "the width is lopsided");
            }
        }
        std::printf("  checked 2..%d voices\n", (int) Unison::maxVoices);
    }

    std::printf("\nAn odd count keeps a copy on the note, dead centre:\n");
    {
        // This is why the maximum is odd. Without a middle copy the played note
        // is only implied by the copies either side of it, and the pitch reads as
        // vague rather than thick.
        for (int v = 1; v <= Unison::maxVoices; v += 2)
        {
            Unison::layout(v, 1.0, 1.0, copies);
            const int middle = v / 2;

            check(std::abs(copies[middle].ratio - 1.0) < 1e-12,
                  "the middle copy is not at the note's pitch");
            check(std::abs(copies[middle].gainLeft - copies[middle].gainRight) < 1e-6,
                  "the middle copy is not centred");
        }
        std::printf("  1, 3, 5 and 7 all keep the note itself\n");
    }

    std::printf("\nThe outermost copies reach the full detune and no further:\n");
    {
        Unison::layout(Unison::maxVoices, 1.0, 0.0, copies);

        const double topCents = 1200.0 * std::log2(copies[Unison::maxVoices - 1].ratio);
        const double bottomCents = 1200.0 * std::log2(copies[0].ratio);

        check(std::abs(topCents - Unison::maxDetuneCents) < 1e-6,
              "the highest copy is not at the full detune");
        check(std::abs(bottomCents + Unison::maxDetuneCents) < 1e-6,
              "the lowest copy is not at the full detune");

        std::printf("  %.1f cents to %.1f cents\n", bottomCents, topCents);
    }

    std::printf("\nTurning Voices up does not change how loud it is:\n");
    {
        // Detuned copies drift in and out of phase, so they sum incoherently and
        // the total sits near the square root of the count. Without the
        // compensation every turn of Voices would have to be answered with the
        // level knob, which is what makes such a control unusable.
        const float comp1 = Unison::layout(1, 0.5, 0.5, copies);
        const double power1 = summedPower(copies, 1, comp1);

        double worstRatio = 1.0;

        for (int v = 2; v <= Unison::maxVoices; ++v)
        {
            const float comp = Unison::layout(v, 0.5, 0.5, copies);
            const double power = summedPower(copies, v, comp);
            const double ratio = power / power1;

            worstRatio = std::fmax(worstRatio, std::fmax(ratio, 1.0 / ratio));
        }

        check(worstRatio < 1.15,
              "the level moves as Voices is turned up");
        std::printf("  worst power difference across 1..%d voices: %.1f%%\n",
                    (int) Unison::maxVoices, (worstRatio - 1.0) * 100.0);
    }

    std::printf("\nEvery pan is equal power, so no copy is louder for its position:\n");
    {
        double worst = 0.0;

        for (int v = 1; v <= Unison::maxVoices; ++v)
        {
            for (int w = 0; w <= 10; ++w)
            {
                Unison::layout(v, 0.5, w / 10.0, copies);

                for (int i = 0; i < v; ++i)
                {
                    const double power = copies[i].gainLeft * copies[i].gainLeft
                                       + copies[i].gainRight * copies[i].gainRight;
                    worst = std::fmax(worst, std::abs(power - 1.0));
                }
            }
        }

        check(worst < 1e-6, "a copy's pan is not equal power");
        std::printf("  worst deviation from unity power: %.2e\n", worst);
    }

    std::printf("\nAsking for nonsense gives something sane:\n");
    {
        // A voice count out of range must clamp rather than read off the end of
        // the caller's array, which on the audio thread is not a wrong sound but
        // a crash.
        Unison::layout(0, 0.5, 0.5, copies);
        check(copies[0].ratio == 1.0, "zero voices did not fall back to one");

        Unison::layout(999, 0.5, 0.5, copies);
        check(copies[Unison::maxVoices - 1].gainLeft > 0.0f,
              "an over-large count did not clamp to the maximum");

        Unison::layout(3, -5.0, 12.0, copies);
        for (int i = 0; i < 3; ++i)
        {
            check(std::isfinite(copies[i].ratio), "an out-of-range detune gave a bad ratio");
            check(copies[i].gainLeft >= 0.0f && copies[i].gainLeft <= 1.0f,
                  "an out-of-range width gave a bad gain");
        }

        std::printf("  handled\n");
    }

    std::printf("\n%s\n", failures == 0 ? "All unison spread tests passed."
                                        : "Some unison spread tests FAILED.");
    return failures == 0 ? 0 : 1;
}

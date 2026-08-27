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

    /** How loud a set of copies actually is, by RUNNING them.

        This used to add the gains in quadrature instead -- a model that assumes
        the copies never line up. Detuned ones do not, so the model looked right
        and the test passed; but at the quiet end of Detune the copies stay locked
        together, and the model cannot see that. It could not see the arrangement
        that made Voices up with Detune down silent, either, because it never
        looked at a waveform. So this generates one. */
    struct Measured { double rmsLeft, rmsRight; };

    Measured measure(int voices, double detune, double width, int seconds = 2)
    {
        constexpr double twoPi = 6.283185307179586476925286766559;

        Unison::Copy copies[Unison::maxVoices];
        const float comp = Unison::layout(voices, detune, width, copies);

        const double sampleRate = 48000.0;
        const double delta = twoPi * 220.0 / sampleRate;
        const int total = (int) (sampleRate * seconds);

        // Every copy starts where the plain oscillator would -- the same as the
        // voice does. See SynthVoice::seedUnisonPhases.
        double angle[Unison::maxVoices] = {};

        double sumL = 0.0, sumR = 0.0;

        for (int n = 0; n < total; ++n)
        {
            double left = 0.0, right = 0.0;

            for (int i = 0; i < voices; ++i)
            {
                const double v = std::sin(angle[i]);
                left  += v * copies[i].gainLeft;
                right += v * copies[i].gainRight;

                angle[i] += delta * copies[i].ratio;
                if (angle[i] >= twoPi) angle[i] -= twoPi;
            }

            left *= comp;
            right *= comp;
            sumL += left * left;
            sumR += right * right;
        }

        return { std::sqrt(sumL / total), std::sqrt(sumR / total) };
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
        check(std::abs(copies[0].gainLeft - 1.0f) < 1e-6,
              "one voice does not come out at unity -- the pan law is eating level");

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


    std::printf("\nTurning Voices up does not change how loud it is, at ANY Detune:\n");
    {
        // The level is measured from a running oscillator, across the whole of
        // Detune -- not from the incoherent-power model, which is only right at
        // the top of that range and hid this for a whole release.
        double worstDb = 0.0;
        double worstAt = 0.0;
        int worstVoices = 0;

        for (int d = 0; d <= 10; ++d)
        {
            const double detune = d / 10.0;
            const double one = measure(1, detune, 0.5).rmsLeft;

            for (int v = 2; v <= Unison::maxVoices; ++v)
            {
                const double got = measure(v, detune, 0.5).rmsLeft;
                const double diff = 20.0 * std::log10((got + 1e-12) / (one + 1e-12));

                if (std::abs(diff) > std::abs(worstDb))
                {
                    worstDb = diff;
                    worstAt = detune;
                    worstVoices = v;
                }
            }
        }

        check(std::abs(worstDb) < 3.0,
              "the level moves as Voices is turned up");
        std::printf("  worst level change across 1..%d voices and all of Detune: "
                    "%+.2f dB (%d voices, detune %.1f)\n",
                    (int) Unison::maxVoices, worstDb, worstVoices, worstAt);
    }

    std::printf("\nVoices up with Detune at zero is not silent:\n");
    {
        // The arrangement this replaces spread the copies evenly around the turn,
        // which is exactly the arrangement that sums to zero. Seven copies at
        // Detune 0 measured 157 dB down -- inaudible. One voice is the right
        // answer here: identical copies ARE one oscillator.
        const double one = measure(1, 0.0, 0.0).rmsLeft;
        double worstDb = 0.0;

        for (int v = 2; v <= Unison::maxVoices; ++v)
        {
            const double got = measure(v, 0.0, 0.0).rmsLeft;
            const double diff = 20.0 * std::log10((got + 1e-12) / (one + 1e-12));

            if (std::abs(diff) > std::abs(worstDb))
                worstDb = diff;
        }

        check(std::abs(worstDb) < 1.0,
              "Voices at zero Detune does not leave the level alone");
        std::printf("  worst level change at Detune 0: %+.2f dB\n", worstDb);
    }

    std::printf("\nThe two sides stay level with each other:\n");
    {
        // The copies are panned symmetrically, so nothing should favour a side.
        double worstDb = 0.0;
        double worstAt = 0.0;

        for (int d = 0; d <= 10; ++d)
        {
            const double detune = d / 10.0;

            for (int v = 2; v <= Unison::maxVoices; ++v)
            {
                const Measured m = measure(v, detune, 1.0);
                const double diff = 20.0 * std::log10((m.rmsLeft + 1e-12) / (m.rmsRight + 1e-12));

                if (std::abs(diff) > std::abs(worstDb)) { worstDb = diff; worstAt = detune; }
            }
        }

        check(std::abs(worstDb) < 1.0, "one side is louder than the other");
        std::printf("  worst imbalance at full Width: %+.2f dB (detune %.1f)\n", worstDb, worstAt);
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
                    // Two, not one: the gains are normalised so a CENTRED copy is
                    // unity a side rather than 0.707, which is what makes turning
                    // unison on cost nothing. Equal power is the property being
                    // checked, and it holds at whatever the constant is.
                    const double power = copies[i].gainLeft * copies[i].gainLeft
                                       + copies[i].gainRight * copies[i].gainRight;
                    worst = std::fmax(worst, std::abs(power - 2.0));
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
            // The ceiling is sqrt(2), not 1: a hard-panned copy carries the level
            // its silent side gives up, which is what keeps the pan equal power.
            check(copies[i].gainLeft >= 0.0f && copies[i].gainLeft <= 1.4142136f,
                  "an out-of-range width gave a bad gain");
        }

        std::printf("  handled\n");
    }

    std::printf("\n%s\n", failures == 0 ? "All unison spread tests passed."
                                        : "Some unison spread tests FAILED.");
    return failures == 0 ? 0 : 1;
}

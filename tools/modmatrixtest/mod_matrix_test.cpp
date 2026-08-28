// =====================================================================
//  Modulation matrix test
//  ---------------------------------------------------------------------
//  The arithmetic that decides where a knob actually sits once an LFO
//  reaches it. Kept free of JUCE so it builds and runs in seconds.
//
//  Build & run:
//      cmake --build build --config Release --target modmatrix-test
//      ./build/mod_matrix_test/Release/mod_matrix_test.exe
// =====================================================================
#include "../../Source/ModMatrix.h"

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

    void checkNear(double got, double want, double tol, const std::string& what)
    {
        if (std::fabs(got - want) > tol)
        {
            std::printf("  FAIL  %s (got %.6f, want %.6f)\n", what.c_str(), got, want);
            ++failures;
        }
    }
}

int main()
{
    using namespace spacedust;

    const DestRange unit { 0.0f, 1.0f };

    // -- an unassigned knob is left exactly where it was --
    {
        ModMatrix m;
        const float lfos[numLfos] = { 1.0f, 1.0f, 1.0f, 1.0f };
        checkNear(m.applyByName("filterCutoff", 0.25f, unit, lfos), 0.25,
                  1e-9, "no routing leaves the base value untouched");
    }

    // -- the LFO rotates around the knob, not around zero --
    {
        ModMatrix m;
        m.setRouting(0, "filterCutoff", 1.0f);
        const float peak[numLfos]   = {  1.0f, 0.0f, 0.0f, 0.0f };
        const float trough[numLfos] = { -1.0f, 0.0f, 0.0f, 0.0f };
        const float centre[numLfos] = {  0.0f, 0.0f, 0.0f, 0.0f };

        // halfRange of 0..1 is 0.5, so +100% at the peak is base + 0.5.
        checkNear(m.applyByName("filterCutoff", 0.5f, unit, peak),   1.0, 1e-6, "peak sits half a range above the knob");
        checkNear(m.applyByName("filterCutoff", 0.5f, unit, trough), 0.0, 1e-6, "trough sits half a range below the knob");
        checkNear(m.applyByName("filterCutoff", 0.5f, unit, centre), 0.5, 1e-6, "the centre of the cycle is the knob itself");

        // Move the knob and the whole movement moves with it.
        checkNear(m.applyByName("filterCutoff", 0.3f, unit, centre), 0.3, 1e-6, "the movement follows the knob");
    }

    // -- a negative amount turns the movement upside down --
    {
        ModMatrix m;
        m.setRouting(0, "filterCutoff", -1.0f);
        const float peak[numLfos] = { 1.0f, 0.0f, 0.0f, 0.0f };
        checkNear(m.applyByName("filterCutoff", 0.5f, unit, peak), 0.0, 1e-6,
                  "a negative amount inverts the movement");
    }

    // -- the result never leaves the knob's legal range --
    {
        ModMatrix m;
        m.setRouting(0, "filterCutoff", 1.0f);
        const float peak[numLfos]   = {  1.0f, 0.0f, 0.0f, 0.0f };
        const float trough[numLfos] = { -1.0f, 0.0f, 0.0f, 0.0f };
        checkNear(m.applyByName("filterCutoff", 0.9f, unit, peak),   1.0, 1e-6, "clamped at the top");
        checkNear(m.applyByName("filterCutoff", 0.1f, unit, trough), 0.0, 1e-6, "clamped at the bottom");
    }

    // -- two LFOs on one knob add before the clamp --
    {
        ModMatrix m;
        m.setRouting(0, "reverbWetMix", 0.5f);
        m.setRouting(1, "reverbWetMix", 0.5f);
        const float both[numLfos] = { 1.0f, 1.0f, 0.0f, 0.0f };
        // 0.5*0.5 + 0.5*0.5 = 0.5 above the knob.
        checkNear(m.applyByName("reverbWetMix", 0.2f, unit, both), 0.7, 1e-6,
                  "two LFOs on one knob add together");
    }

    // -- assigning the same pair twice replaces, it does not stack --
    {
        ModMatrix m;
        m.setRouting(0, "reverbWetMix", 0.25f);
        m.setRouting(0, "reverbWetMix", 0.75f);
        check(m.routings().size() == 1, "assigning the same pair twice keeps one entry");
        checkNear(m.amountFor(0, "reverbWetMix"), 0.75, 1e-6, "the second assignment wins");
    }

    // -- an amount of zero removes the routing rather than leaving a dead entry --
    {
        ModMatrix m;
        m.setRouting(0, "reverbWetMix", 0.5f);
        m.setRouting(0, "reverbWetMix", 0.0f);
        check(m.routings().empty(), "an amount of zero removes the routing");
        check(!m.hasAnyRouting("reverbWetMix"), "and the knob reports no routing");
    }

    // -- clearRouting removes only the pair it names --
    {
        ModMatrix m;
        m.setRouting(0, "a", 0.5f);
        m.setRouting(1, "a", 0.5f);
        m.clearRouting(0, "a");
        check(m.routings().size() == 1, "clearRouting removes one entry");
        check(m.amountFor(1, "a") == 0.5f, "and leaves the other LFO alone");
    }

    // -- an amount outside -1..+1 is clamped when stored --
    {
        ModMatrix m;
        m.setRouting(0, "a",  4.0f);
        m.setRouting(1, "a", -4.0f);
        checkNear(m.amountFor(0, "a"),  1.0, 1e-6, "an amount above +1 is clamped");
        checkNear(m.amountFor(1, "a"), -1.0, 1e-6, "an amount below -1 is clamped");
    }

    // -- an out-of-range LFO index is refused, not stored --
    {
        ModMatrix m;
        m.setRouting(-1, "a", 0.5f);
        m.setRouting(numLfos, "a", 0.5f);
        check(m.routings().empty(), "an illegal LFO index stores nothing");
    }

    // -- a real parameter range, not just 0..1 --
    {
        ModMatrix m;
        m.setRouting(0, "filterCutoff", 1.0f);
        const DestRange hz { 20.0f, 20000.0f };   // halfRange 9990
        const float peak[numLfos] = { 1.0f, 0.0f, 0.0f, 0.0f };
        checkNear(m.applyByName("filterCutoff", 1000.0f, hz, peak), 10990.0, 1e-3,
                  "the amount scales with the knob's own range");
    }

    if (failures == 0)
        std::printf("\nAll modulation matrix tests passed.\n");
    else
        std::printf("\n%d modulation matrix test(s) FAILED.\n", failures);

    return failures == 0 ? 0 : 1;
}

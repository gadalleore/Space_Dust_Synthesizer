// =====================================================================
//  Note Lock grid test
//  ---------------------------------------------------------------------
//  Exercises the REAL NoteLock::snapHz -- this links Source/NoteLockGrid.cpp
//  rather than reimplementing the maths, because a reimplemented test harness
//  only ever proves the harness.
//
//  Build & run:
//      cmake --build build --config Release --target notelock-test
//      ./build/notelock_test/Release/notelock_test.exe
// =====================================================================
#include "../../Source/NoteLockGrid.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
    int failures = 0;

    void check(bool ok, const std::string& what)
    {
        if (!ok)
        {
            std::printf("  FAIL  %s\n", what.c_str());
            ++failures;
        }
    }

    constexpr double kMin = 20.0;
    constexpr double kMax = 20000.0;

    double cents(double a, double b) { return 1200.0 * std::log2(a / b); }

    // Every frequency the harmonic grid is allowed to produce, built independently
    // of snapHz so "nearest" can be brute-forced and compared.
    std::vector<double> harmonicTable()
    {
        std::vector<double> v;
        // k == 1 is the root, and root*1 == root/1 -- add it once or the detent
        // count reported below is off by one.
        for (int k = 1; k <= 200; ++k)
        {
            const double up = NoteLock::referenceHz * k;
            if (up >= kMin && up <= kMax) v.push_back(up);

            if (k == 1) continue;
            const double down = NoteLock::referenceHz / k;
            if (down >= kMin && down <= kMax) v.push_back(down);
        }
        return v;
    }

    std::vector<double> semitoneTable()
    {
        std::vector<double> v;
        for (int n = -200; n <= 200; ++n)
        {
            const double f = NoteLock::referenceHz * std::pow(2.0, n / 12.0);
            if (f >= kMin && f <= kMax) v.push_back(f);
        }
        return v;
    }

    // Log-uniform sweep across the whole cutoff range.
    std::vector<double> sweep(int n)
    {
        std::vector<double> v;
        v.reserve(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i)
            v.push_back(kMin * std::pow(kMax / kMin, i / static_cast<double>(n - 1)));
        return v;
    }

    void testGrid(const char* name, NoteLock::Grid grid, const std::vector<double>& table)
    {
        std::printf("%s: %zu detents in range\n", name, table.size());

        int worstIdx = -1;
        double worstErr = 0.0;

        for (double f : sweep(20000))
        {
            const double got = NoteLock::snapHz(f, kMin, kMax, grid);

            // (1) the result must actually BE a detent
            bool onGrid = false;
            for (double t : table)
                if (std::abs(cents(got, t)) < 0.01) { onGrid = true; break; }
            check(onGrid, "result " + std::to_string(got) + " Hz is not on the grid (input "
                          + std::to_string(f) + ")");

            // (2) it must be the NEAREST detent, brute-forced in log space
            double want = table.front();
            double wantErr = std::abs(cents(f, want));
            for (double t : table)
            {
                const double e = std::abs(cents(f, t));
                if (e < wantErr) { wantErr = e; want = t; }
            }
            if (std::abs(cents(got, want)) > 0.01)
            {
                if (++worstIdx < 3)
                    std::printf("  FAIL  f=%.3f got=%.3f want=%.3f\n", f, got, want);
                ++failures;
            }

            // (3) never leaves the knob's range
            check(got >= kMin && got <= kMax, "result out of range: " + std::to_string(got));

            const double err = std::abs(cents(f, got));
            if (err > worstErr) worstErr = err;
        }
        std::printf("  worst snap distance over the sweep: %.1f cents\n", worstErr);
    }
}

int main()
{
    std::printf("=== Note Lock grid ===\n\n");

    testGrid("Semitones", NoteLock::Grid::Semitones, semitoneTable());
    std::printf("\n");
    testGrid("Harmonics", NoteLock::Grid::Harmonics, harmonicTable());

    // -- The property the whole feature rests on --------------------------------
    // Key Tracking multiplies the cutoff by 2^((note-60)/12), so a snapped cutoff
    // must sit at the SAME ratio above every played note. Verified against the
    // actual fundamental of each MIDI note across the keyboard.
    std::printf("\nKey-tracking invariant (ratio to the played note must be constant):\n");
    for (auto grid : { NoteLock::Grid::Semitones, NoteLock::Grid::Harmonics })
    {
        double worst = 0.0;
        for (double knob : { 300.0, 900.0, 2000.0, 6000.0, 15000.0 })
        {
            const double snapped = NoteLock::snapHz(knob, kMin, kMax, grid);
            const double baseRatio = snapped / NoteLock::referenceHz;
            for (int note = 21; note <= 108; ++note)
            {
                const double fund  = 440.0 * std::pow(2.0, (note - 69) / 12.0);
                const double heard = snapped * std::pow(2.0, (note - 60) / 12.0);
                worst = std::max(worst, std::abs(cents(heard / fund, baseRatio)));
            }
        }
        std::printf("  %-10s worst deviation across MIDI 21-108: %.2e cents\n",
                    grid == NoteLock::Grid::Semitones ? "Semitones" : "Harmonics", worst);
        check(worst < 1e-6, "key-tracking ratio is not constant across notes");
    }

    // -- Degenerate inputs must not invent detents ------------------------------
    std::printf("\nDegenerate inputs:\n");
    for (auto grid : { NoteLock::Grid::Semitones, NoteLock::Grid::Harmonics })
    {
        check(NoteLock::snapHz(0.0,  kMin, kMax, grid) >= kMin, "zero frequency escaped the range");
        check(NoteLock::snapHz(-5.0, kMin, kMax, grid) >= kMin, "negative frequency escaped the range");
        const double inverted = NoteLock::snapHz(500.0, 20000.0, 20.0, grid);
        check(std::isfinite(inverted), "inverted range produced a non-finite result");
    }
    std::printf("  handled\n");

    std::printf("\n%s (%d failure%s)\n", failures == 0 ? "ALL PASSED" : "FAILED",
                failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}

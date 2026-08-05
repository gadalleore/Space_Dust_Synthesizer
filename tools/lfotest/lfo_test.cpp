// =====================================================================
//  LFO fold-back test
//  ---------------------------------------------------------------------
//  Renders the REAL LfoWaveform::generate at a range of rates and measures how
//  much energy lands somewhere other than the intended fundamental and its
//  harmonics. Anything that shows up between the harmonics is aliasing that has
//  folded back down, and that is what makes the audible pitch disagree with the
//  rate the knob (and Note Lock) asked for.
//
//  Passing "legacy" as an argument evaluates the OLD fraction-only edges by
//  forcing dt = 0, so the improvement can be compared rather than asserted.
//
//  Build & run:
//      cmake --build build --config Release --target lfo-test
//      ./build/lfo_test/Release/lfo_test.exe [legacy]
// =====================================================================
#include "../../Source/LfoWaveform.h"

#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    constexpr double kPi = 3.14159265358979323846;
    constexpr double kSampleRate = 48000.0;
    constexpr int    kN = 16384;                 // FFT size

    int failures = 0;

    std::vector<std::complex<double>> dft(const std::vector<double>& x)
    {
        // Radix-2 iterative FFT; kN is a power of two.
        const int n = static_cast<int>(x.size());
        std::vector<std::complex<double>> a(n);
        for (int i = 0; i < n; ++i) a[i] = { x[i], 0.0 };

        for (int i = 1, j = 0; i < n; ++i)
        {
            int bit = n >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j) std::swap(a[i], a[j]);
        }
        for (int len = 2; len <= n; len <<= 1)
        {
            const double ang = -2.0 * kPi / len;
            const std::complex<double> wl(std::cos(ang), std::sin(ang));
            for (int i = 0; i < n; i += len)
            {
                std::complex<double> w(1.0, 0.0);
                for (int k = 0; k < len / 2; ++k)
                {
                    const auto u = a[i + k];
                    const auto v = a[i + k + len / 2] * w;
                    a[i + k] = u + v;
                    a[i + k + len / 2] = u - v;
                    w *= wl;
                }
            }
        }
        return a;
    }

    /** Energy BELOW the fundamental, relative to total, in dB.

        This is the metric that matters and the one that is hard to fool. A correctly
        band-limited oscillator puts essentially nothing below its own fundamental --
        every partial is above it. Harmonics that exceed Nyquist, though, fold back
        DOWN, and a large share of them land under the fundamental. That is exactly
        what makes the perceived pitch disagree with the rate asked for.

        Counting "off-harmonic" energy instead would be misleading: at a rate that
        divides the sample rate evenly (2000 Hz into 48 kHz) every alias lands on
        another harmonic and hides completely. The rates below are deliberately
        non-commensurate for the same reason. */
    double foldbackDb(int waveform, double freqHz, bool legacy, double edgeFloor = LfoWaveform::minEdgeSamples)
    {
        const double dt = freqHz / kSampleRate;

        std::vector<double> buf(kN);
        double phase = 0.0;
        for (int i = 0; i < kN; ++i)
        {
            buf[i] = legacy ? LfoWaveform::generate(phase, waveform, 0.0, 0.0)
                            : LfoWaveform::generate(phase, waveform, dt, edgeFloor);
            phase = std::fmod(phase + dt, 1.0);
        }

        // Remove DC first. The eased saws are slightly asymmetric, and DC leaking
        // through the window into the low bins otherwise swamps the very thing being
        // measured -- it showed Saw Up pinned at -21 dB regardless of rate, which is
        // offset, not fold-back.
        double mean = 0.0;
        for (double v : buf) mean += v;
        mean /= kN;
        for (double& v : buf) v -= mean;

        // Hann window, so a non-integer number of cycles does not smear energy
        // everywhere and get mistaken for aliasing.
        for (int i = 0; i < kN; ++i)
            buf[i] *= 0.5 * (1.0 - std::cos(2.0 * kPi * i / (kN - 1)));

        const auto spec = dft(buf);

        double total = 0.0, below = 0.0;
        const double binHz = kSampleRate / kN;

        // Skip a few bins either side of the fundamental so the Hann window's own
        // skirt is not counted as fold-back.
        const double guardHz = 6.0 * binHz;

        for (int k = 1; k < kN / 2; ++k)
        {
            const double hz = k * binHz;
            const double e = std::norm(spec[k]);
            total += e;
            if (hz < freqHz - guardHz)
                below += e;
        }

        if (total <= 0.0) return -200.0;
        return 10.0 * std::log10(below / total + 1e-20);
    }

    const char* nameOf(int w)
    {
        switch (w)
        {
            case LfoWaveform::Sine:     return "Sine";
            case LfoWaveform::Triangle: return "Triangle";
            case LfoWaveform::SawUp:    return "Saw Up";
            case LfoWaveform::SawDown:  return "Saw Down";
            case LfoWaveform::Square:   return "Square";
            default:                    return "?";
        }
    }
}

int main(int argc, char** argv)
{
    const bool legacy = (argc > 1 && std::strcmp(argv[1], "legacy") == 0);

    std::printf("=== LFO fold-back (%s edges) ===\n", legacy ? "LEGACY fraction-only" : "band-limited");
    std::printf("energy BELOW the fundamental, dB rel. total (lower = cleaner)\n");
    std::printf("rates are non-commensurate with 48 kHz so aliases cannot hide on harmonics\n\n");
    std::printf("%-10s", "rate");
    for (int w : { LfoWaveform::Sine, LfoWaveform::Triangle, LfoWaveform::SawUp,
                   LfoWaveform::SawDown, LfoWaveform::Square })
        std::printf("%12s", nameOf(w));
    std::printf("\n");

    for (double hz : { 2.13, 51.7, 203.9, 517.3, 1033.7, 1190.0, 1777.1 })
    {
        std::printf("%8.0f Hz", hz);
        for (int w : { LfoWaveform::Sine, LfoWaveform::Triangle, LfoWaveform::SawUp,
                       LfoWaveform::SawDown, LfoWaveform::Square })
            std::printf("%12.1f", foldbackDb(w, hz, legacy));
        std::printf("\n");
    }

    // Fine sweep across the top of the range: a single bad rate hides in a coarse
    // table, and "it folds back at 1190" is exactly that kind of report.
    std::printf("\nFine sweep 900-2000 Hz (worst shape at each rate):\n");
    for (double hz = 900.0; hz <= 2000.0; hz += 55.0)
    {
        double worst = -300.0; const char* who = "";
        for (int w : { LfoWaveform::Sine, LfoWaveform::Triangle, LfoWaveform::SawUp,
                       LfoWaveform::SawDown, LfoWaveform::Square })
        {
            const double db = foldbackDb(w, hz, legacy);
            if (db > worst) { worst = db; who = nameOf(w); }
        }
        std::printf("  %6.0f Hz  %7.1f dB   %s\n", hz, worst, who);
    }

    if (!legacy)
    {
        // Sweep the edge floor so the chosen value is evidence-based. Reported at the
        // rates where fold-back actually bites.
        std::printf("\nEdge-floor sweep (dB below fundamental; lower = cleaner):\n");
        std::printf("%-14s", "floor(samples)");
        for (double hz : { 517.3, 1033.7, 1777.1 }) std::printf("%9.0fHz", hz);
        std::printf("   | shape cost at 1777 Hz\n");
        for (double floorSamples : { 0.0, 2.0, 4.0, 8.0, 16.0 })
        {
            std::printf("%-14.0f", floorSamples);
            for (double hz : { 517.3, 1033.7, 1777.1 })
                std::printf("%11.1f", foldbackDb(LfoWaveform::Square, hz, false, floorSamples));
            // How far the square has softened: peak-to-peak of the realised shape.
            double lo = 1e9, hi = -1e9;
            const double dt = 1777.1 / kSampleRate;
            for (int i = 0; i < 2048; ++i)
            {
                const double v = LfoWaveform::generate(i / 2048.0, LfoWaveform::Square, dt, floorSamples);
                lo = std::min(lo, v); hi = std::max(hi, v);
            }
            std::printf("   | peak-to-peak %.2f\n", hi - lo);
        }

        // The shapes must be UNCHANGED at LFO rates: the whole point of flooring the
        // edge in samples rather than replacing it is that slow rates keep their sound.
        std::printf("\nShapes unchanged at LFO rates (max sample difference vs legacy):\n");
        for (int w : { LfoWaveform::Triangle, LfoWaveform::SawUp,
                       LfoWaveform::SawDown, LfoWaveform::Square })
        {
            double worst = 0.0;
            for (double hz : { 0.1, 1.0, 5.0, 20.0 })
            {
                const double dt = hz / kSampleRate;
                for (int i = 0; i < 4096; ++i)
                {
                    const double p = i / 4096.0;
                    worst = std::max(worst, std::abs((double) LfoWaveform::generate(p, w, dt)
                                                   - (double) LfoWaveform::generate(p, w, 0.0, 0.0)));
                }
            }
            std::printf("  %-10s %.2e\n", nameOf(w), worst);
            if (worst > 1e-9)
            {
                std::printf("  FAIL  %s changed at LFO rates\n", nameOf(w));
                ++failures;
            }
        }

        // Sine must stay clean at every rate -- it has no harmonics to fold, so this
        // doubles as a sanity check that the metric is measuring aliasing and not
        // some artefact of the analysis.
        std::printf("\nSine sanity (must stay clean; validates the metric itself):\n");
        for (double hz : { 517.3, 1033.7, 1777.1 })
        {
            const double db = foldbackDb(LfoWaveform::Sine, hz, false);
            std::printf("  %7.1f Hz  %.1f dB\n", hz, db);
            // The metric floors around -55 dB on window leakage alone, so this checks
            // the sine sits at that floor rather than above it.
            if (db > -50.0)
            {
                std::printf("  FAIL  Sine shows fold-back at %.1f Hz (%.1f dB)\n", hz, db);
                ++failures;
            }
        }
    }

    std::printf("\n%s (%d failure%s)\n", failures == 0 ? "ALL PASSED" : "FAILED",
                failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}

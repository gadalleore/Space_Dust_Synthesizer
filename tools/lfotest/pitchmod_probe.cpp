// =====================================================================
//  Pitch-modulation fold-back probe
//  ---------------------------------------------------------------------
//  Diagnostic, not a regression test. Reproduces the pitch-modulation path from
//  SynthVoice::renderNextBlock -- per sample, oscFreq *= 2^(lfoValue * 1200 / 1200)
//  -- and measures how much energy ends up BELOW the carrier fundamental, which is
//  where fold-back lands.
//
//  The carrier shapes below mirror SynthVoice::generateWaveform. They are copied
//  rather than linked because that class needs the whole of JUCE; they are four
//  lines each and the point here is to locate the problem, not to guard it.
//
//  It separates the two possible culprits:
//    * the carrier's OWN aliasing (a naive saw/square aliases even unmodulated)
//    * FM sidebands pushed past Nyquist by the modulation itself
//  by reporting each carrier unmodulated and then modulated.
// =====================================================================
#include "../../Source/LfoWaveform.h"
#include "../../Source/OversampledStage.h"

#include <cmath>
#include <complex>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
    constexpr double kPi = 3.14159265358979323846;
    constexpr double kSR = 48000.0;
    constexpr int    kN  = 16384;

    inline double polyBlep(double t, double dt)
    {
        if (dt <= 0.0) return 0.0;
        if (t < dt)       { t /= dt;           return t + t - t * t - 1.0; }
        if (t > 1.0 - dt) { t = (t - 1.0) / dt; return t * t + t + t + 1.0; }
        return 0.0;
    }

    // Mirrors SynthVoice::generateWaveform -- the naive shapes, as shipped.
    float carrier(double angle, int shape, double)
    {
        const double norm = angle / (2.0 * kPi);
        const double ph   = norm - std::floor(norm + 0.5);
        switch (shape)
        {
            case 0: return static_cast<float>(std::sin(angle));                  // Sine
            case 1: return static_cast<float>(4.0 * std::abs(ph) - 1.0);         // Triangle
            case 2: return static_cast<float>(2.0 * ph);                         // Saw
            default: return std::sin(angle) > 0.0 ? 1.0f : -1.0f;                // Square
        }
    }
    const char* carrierName(int s)
    {
        return s == 0 ? "Sine" : s == 1 ? "Triangle" : s == 2 ? "Saw" : "Square";
    }

    std::vector<std::complex<double>> fft(std::vector<double> x)
    {
        const int n = (int) x.size();
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
                    const auto u = a[i + k], v = a[i + k + len / 2] * w;
                    a[i + k] = u + v; a[i + k + len / 2] = u - v; w *= wl;
                }
            }
        }
        return a;
    }

    /** Energy below the carrier fundamental, dB rel. total. */
    double belowFundamentalDb(int shape, double carrierHz, double lfoHz, double depth, bool blep, bool subStepMod)
    {
        std::vector<double> buf(kN);
        double ang = 0.0, lfoPhase = 0.0;
        const double lfoDt = lfoHz / kSR;

        OversampledStage os;
        os.prepare();
        os.setFactor(blep ? 4 : 1);          // "blep" flag now means "oversampled"

        for (int i = 0; i < kN; ++i)
        {
            double f = carrierHz;
            if (depth > 0.0)
            {
                const double lfoVal = LfoWaveform::generate(lfoPhase, LfoWaveform::Sine, lfoDt) * depth;
                f *= std::pow(2.0, lfoVal);          // matches the voice: 1200 cents at full swing
                if (!(subStepMod && os.getFactor() > 1)) lfoPhase = std::fmod(lfoPhase + lfoDt, 1.0);
            }

            const int osf = os.getFactor();
            buf[i] = os.process(0, 0.0f, [&](float) -> float
            {
                const float v = carrier(ang, shape, 0.0);

                // Modulation evaluated PER SUB-STEP when subStepMod is set, rather than
                // held across the base-rate sample. Holding it means the modulator is
                // itself stepped at 48 kHz, and those steps alias no matter how finely
                // the carrier is generated -- which is the thing being tested here.
                double fSub = f;
                if (depth > 0.0 && subStepMod && osf > 1)
                {
                    const double lv = LfoWaveform::generate(lfoPhase, LfoWaveform::Sine, lfoDt / osf) * depth;
                    fSub = carrierHz * std::pow(2.0, lv);
                    lfoPhase = std::fmod(lfoPhase + lfoDt / osf, 1.0);
                }
                ang = std::fmod(ang + 2.0 * kPi * fSub / kSR / osf, 2.0 * kPi);
                return v;
            });
        }

        double mean = 0.0; for (double v : buf) mean += v; mean /= kN;
        for (double& v : buf) v -= mean;
        for (int i = 0; i < kN; ++i) buf[i] *= 0.5 * (1.0 - std::cos(2.0 * kPi * i / (kN - 1)));

        const auto spec = fft(buf);
        const double binHz = kSR / kN;
        double total = 0.0, below = 0.0;
        for (int k = 1; k < kN / 2; ++k)
        {
            const double e = std::norm(spec[k]);
            total += e;
            if (k * binHz < carrierHz - 6.0 * binHz) below += e;
        }
        return total > 0.0 ? 10.0 * std::log10(below / total + 1e-20) : -200.0;
    }
}

int main(int argc, char** argv)
{
    const bool blep    = !(argc > 1 && std::string(argv[1]) == "naive");
    const bool subStep = (argc > 1 && std::string(argv[1]) == "substep");
    const double carrierHz = 261.6256;   // middle C
    const double lfoHz     = 1190.0;

    std::printf("=== Pitch-modulation fold-back (%s oscillators) ===\n", subStep ? "4x + per-sub-step modulation" : blep ? "4x, modulation held" : "no oversampling");
    std::printf("carrier %.1f Hz, LFO %.0f Hz sine, 48 kHz\n", carrierHz, lfoHz);
    std::printf("energy BELOW the carrier fundamental, dB (lower = cleaner)\n\n");

    std::printf("%-10s %14s %14s %14s %14s\n", "carrier",
                "unmodulated", "depth 0.10", "depth 0.25", "depth 1.00");
    for (int s = 0; s < 4; ++s)
    {
        std::printf("%-10s", carrierName(s));
        for (double d : { 0.0, 0.10, 0.25, 1.0 })
            std::printf("%14.1f", belowFundamentalDb(s, carrierHz, lfoHz, d, blep, subStep));
        std::printf("\n");
    }

    std::printf("\nNOTE: only the 'unmodulated' column above is a valid aliasing figure.\n");
    std::printf("Under FM a rich carrier puts real sidebands below its fundamental, which\n");
    std::printf("that metric cannot tell from fold-back. Measuring the modulated case\n");
    std::printf("properly needs a high-rate reference render; not yet built.\n");

    return 0;
}

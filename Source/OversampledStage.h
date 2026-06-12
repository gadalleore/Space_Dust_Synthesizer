#pragma once

#include <juce_core/juce_core.h>
#include <algorithm>
#include <cmath>

//==============================================================================
/**
    Per-sample integer-factor oversampling wrapper for a NONLINEAR stage.

    Built for the synth voice's master filter stage (self-oscillating SVF + tanh
    warm-saturation + per-stage hard clip), whose nonlinearities generate content
    above Nyquist that folds back into the audible band as aliasing — audible as
    the LFO "fold-back" when the filter is hammered by audio-rate modulation.

    For each incoming base-rate sample it:
      1. up-samples to `factor` sub-samples (zero-stuff + anti-imaging FIR),
      2. runs a caller-supplied nonlinear function at the oversampled rate,
      3. decimates back to one sample (anti-aliasing FIR + pick).

    Linear-phase Hann-windowed-sinc half/quarter-band kernels (factor 2 or 4).
    factor == 1 is a transparent pass-through (the function is called once), so
    the toggle-off path is bit-identical to the un-oversampled stage.

    Stereo only (2 channels). RT-safe: no allocation after setFactor().
*/
class OversampledStage
{
public:
    void prepare() noexcept { reset(); }

    /** factor must be 1, 2 or 4; anything else clamps to 1 (off). */
    void setFactor (int f) noexcept
    {
        f = (f == 2 || f == 4) ? f : 1;
        if (f == factor)
            return;
        factor = f;
        if (factor > 1)
            designKernel();
        reset();
    }

    int getFactor() const noexcept { return factor; }

    void reset() noexcept
    {
        for (int c = 0; c < 2; ++c)
        {
            std::fill (std::begin (upZ[c]),   std::end (upZ[c]),   0.0f);
            std::fill (std::begin (downZ[c]), std::end (downZ[c]), 0.0f);
            upPos[c] = downPos[c] = 0;
        }
    }

    /** Process one base-rate sample for `channel` through `fn` at the oversampled
        rate. `fn` is invoked `factor` times per call (or once when factor == 1). */
    template <typename Fn>
    float process (int channel, float x, Fn&& fn) noexcept
    {
        if (factor <= 1)
            return fn (x);

        const int ch = (channel == 1) ? 1 : 0;
        float out = 0.0f;
        for (int k = 0; k < factor; ++k)
        {
            // Zero-stuff with the unity-DC-gain anti-imaging FIR; pre-scale by
            // `factor` so the held energy of the inserted zeros is restored.
            const float in = (k == 0) ? x * static_cast<float> (factor) : 0.0f;
            const float up = firStep (upZ[ch],   upPos[ch],   in);
            const float y  = fn (up);
            const float dn = firStep (downZ[ch], downPos[ch], y);
            if (k == factor - 1)   // decimate: keep the last filtered sub-sample
                out = dn;
        }
        return out;
    }

private:
    static constexpr int kMaxTaps = 33;

    void designKernel() noexcept
    {
        numTaps = (factor == 4) ? 33 : 21;
        const int   M  = numTaps - 1;
        const double fc = 0.5 / static_cast<double> (factor);   // cycles/sample at OS rate
        const double pi = juce::MathConstants<double>::pi;

        double sum = 0.0;
        for (int n = 0; n < numTaps; ++n)
        {
            const double m = n - M / 2.0;
            const double sinc = (std::abs (m) < 1.0e-9)
                                  ? 2.0 * fc
                                  : std::sin (2.0 * pi * fc * m) / (pi * m);
            const double w = 0.5 - 0.5 * std::cos (2.0 * pi * n / static_cast<double> (M)); // Hann
            const double v = sinc * w;
            h[n] = static_cast<float> (v);
            sum += v;
        }
        const float norm = (std::abs (sum) > 1.0e-12) ? static_cast<float> (1.0 / sum) : 1.0f;
        for (int n = 0; n < numTaps; ++n)
            h[n] *= norm;   // unity DC gain
    }

    float firStep (float* z, int& pos, float in) noexcept
    {
        z[pos] = in;
        float acc = 0.0f;
        int idx = pos;
        for (int i = 0; i < numTaps; ++i)
        {
            acc += h[i] * z[idx];
            idx = (idx == 0) ? numTaps - 1 : idx - 1;
        }
        pos = (pos + 1) % numTaps;
        return acc;
    }

    int   factor  = 1;
    int   numTaps = 1;
    float h[kMaxTaps] {};
    float upZ[2][kMaxTaps]   {};
    float downZ[2][kMaxTaps] {};
    int   upPos[2]   { 0, 0 };
    int   downPos[2] { 0, 0 };
};

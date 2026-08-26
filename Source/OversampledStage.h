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

    PERFORMANCE (polyphase, bit-identical to the naive form):
      * Upsampling: a zero-stuffed convolution multiplies ¾ (or ½) of its taps by
        an inserted zero, contributing nothing. We instead keep a base-rate input
        history and, for each output phase k, sum ONLY the taps h[k], h[k+factor],
        … against the real input samples. Same non-zero terms, same ascending-tap
        order → identical sum, at 1/factor the multiply count.
      * Downsampling: the decimator discards every sub-sample except the last, so
        the full anti-aliasing convolution is only needed once per base sample.
        The intermediate sub-samples are still pushed into the FIR state (the
        filter must see them) but their dot-product is skipped. Identical output,
        ~1/factor the multiply count.

    Up to kMaxChannels independent signals. RT-safe: no allocation after setFactor().
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
        for (int c = 0; c < kMaxChannels; ++c)
        {
            std::fill (std::begin (xHist[c]), std::end (xHist[c]), 0.0f);
            std::fill (std::begin (downZ[c]), std::end (downZ[c]), 0.0f);
            xPos[c]   = 0;
            downPos[c] = 0;
        }
    }

    /** Process one base-rate sample for `channel` through `fn` at the oversampled
        rate. `fn` is invoked `factor` times per call (or once when factor == 1). */
    template <typename Fn>
    float process (int channel, float x, Fn&& fn) noexcept
    {
        if (factor <= 1)
            return fn (x);

        // Clamped, not folded. This read `(channel == 1) ? 1 : 0` while there were
        // only two channels, which would quietly have put channels 2 and 3 onto
        // channel 0 -- three signals sharing one filter's history, which is not a
        // wrong number so much as three wrong numbers.
        const int ch = juce::jlimit (0, kMaxChannels - 1, channel);

        // Push the pre-scaled base-rate input once. Pre-scaling by `factor` here (to
        // restore the held energy of the zero-stuffed samples) keeps each product
        // h[t] * (x*factor) bit-identical to the naive zero-stuffed convolution.
        int& xp = xPos[ch];
        xp = (xp == 0) ? kBaseHist - 1 : xp - 1;
        xHist[ch][xp] = x * static_cast<float> (factor);

        float out = 0.0f;
        for (int k = 0; k < factor; ++k)
        {
            const float up = upsamplePhase (ch, k);     // polyphase anti-imaging FIR
            const float y  = fn (up);                    // nonlinear at the OS rate
            const bool  keep = (k == factor - 1);        // decimate: keep last sub-sample
            const float dn = decimateStep (ch, y, keep); // anti-aliasing FIR (dot only when kept)
            if (keep)
                out = dn;
        }
        return out;
    }

    /** Like process(), but for a generator that makes TWO signals at once.

        `fn` is handed the upsampled input and a float& for its second output, and
        returns the first. Both are filtered, each on its own channel.

        One loop, not two calls to process(). The generator here advances a phase
        accumulator, so running it a second time for the right channel would step
        the oscillator twice per sample and play it an octave up. This is the whole
        reason the stage grew from two channels to four. */
    template <typename Fn>
    void processStereo (int chLeft, int chRight, float x, Fn&& fn,
                        float& outLeft, float& outRight) noexcept
    {
        if (factor <= 1)
        {
            outRight = 0.0f;
            outLeft = fn (x, outRight);
            return;
        }

        const int cl = juce::jlimit (0, kMaxChannels - 1, chLeft);
        const int cr = juce::jlimit (0, kMaxChannels - 1, chRight);

        // The input history is pushed for the LEFT channel only. There is no input
        // signal here -- x is zero and the generator ignores it -- so the right
        // channel needs no upsampling of its own, only its own decimator state.
        int& xp = xPos[cl];
        xp = (xp == 0) ? kBaseHist - 1 : xp - 1;
        xHist[cl][xp] = x * static_cast<float> (factor);

        outLeft = 0.0f;
        outRight = 0.0f;

        for (int k = 0; k < factor; ++k)
        {
            const float up = upsamplePhase (cl, k);

            float right = 0.0f;
            const float left = fn (up, right);

            const bool keep = (k == factor - 1);
            const float dnL = decimateStep (cl, left, keep);
            const float dnR = decimateStep (cr, right, keep);

            if (keep)
            {
                outLeft = dnL;
                outRight = dnR;
            }
        }
    }

private:
    static constexpr int kMaxTaps  = 33;
    // Base-rate history length: for output phase k we touch taps k, k+factor, … so we
    // need ceil(numTaps / factor) past inputs. ceil(33/2)=17 is the worst case (factor 2).
    static constexpr int kBaseHist = 17;

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

    /** Anti-imaging FIR for output sub-sample phase `k`, evaluated polyphase against
        the base-rate input history. Sums h[k], h[k+factor], … times x[m], x[m-1], …
        — the exact non-zero terms of the zero-stuffed convolution, in tap order. */
    float upsamplePhase (int ch, int k) const noexcept
    {
        const float* xz = xHist[ch];
        float acc = 0.0f;
        int idx = xPos[ch];               // newest base sample = x[m]
        for (int tap = k; tap < numTaps; tap += factor)
        {
            acc += h[tap] * xz[idx];
            idx = (idx == kBaseHist - 1) ? 0 : idx + 1;   // walk back toward x[m-1], x[m-2], …
        }
        return acc;
    }

    /** Push `y` into the decimation FIR's delay line. The decimator only keeps one
        sub-sample per base sample, so the dot-product is computed only when
        `computeOut` is true; the discarded sub-samples just advance the state. */
    float decimateStep (int ch, float y, bool computeOut) noexcept
    {
        float* z = downZ[ch];
        int&   pos = downPos[ch];
        z[pos] = y;
        float acc = 0.0f;
        if (computeOut)
        {
            int idx = pos;
            for (int i = 0; i < numTaps; ++i)
            {
                acc += h[i] * z[idx];
                idx = (idx == 0) ? numTaps - 1 : idx - 1;
            }
        }
        pos = (pos + 1) % numTaps;
        return acc;
    }

public:
    /** How many independent signals one stage can carry.

        Four, not two: the two oscillators used one channel each, and each of them
        now produces a LEFT and a RIGHT when it is playing a stereo slot. Every
        channel costs two short history buffers and nothing else, so the room is
        cheaper than a second stage and keeps one filter's state in one object. */
    static constexpr int kMaxChannels = 4;

private:
    int   factor  = 1;
    int   numTaps = 1;
    float h[kMaxTaps] {};
    float xHist[kMaxChannels][kBaseHist] {};   // base-rate input history (pre-scaled by factor)
    float downZ[kMaxChannels][kMaxTaps]  {};   // decimation FIR delay line (oversampled rate)
    int   xPos[kMaxChannels]   {};
    int   downPos[kMaxChannels] {};
};

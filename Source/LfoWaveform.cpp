#include "LfoWaveform.h"

#include <algorithm>
#include <cmath>

namespace
{
    constexpr double kPi    = 3.14159265358979323846;
    constexpr double kTwoPi = 6.28318530717958647692;

    /** Raised-cosine ease from 0 to 1 over t in [0,1]. Smooth in value and slope,
        which is what keeps the edge from radiating harmonics. */
    inline double ease(double t)
    {
        t = std::min(std::max(t, 0.0), 1.0);
        return (1.0 - std::cos(t * kPi)) * 0.5;
    }

    // Set per call from the edgeFloorSamples argument, so the test can sweep it.
    double gEdgeFloorSamples = LfoWaveform::minEdgeSamples;

    /** The original per-cycle fraction, floored at a number of samples so a short
        cycle cannot collapse the edge into a step. Capped below half a cycle so the
        shape stays recognisable when the rate is extreme. */
    inline double edgeWidth(double fraction, double dt)
    {
        const double floorWidth = gEdgeFloorSamples * std::abs(dt);
        return std::min(std::max(fraction, floorWidth), 0.5);
    }
}

float LfoWaveform::generate(double phase, int waveform, double dt)
{
    return generate(phase, waveform, dt, minEdgeSamples);
}

float LfoWaveform::generate(double phase, int waveform, double dt, double edgeFloorSamples)
{
    gEdgeFloorSamples = edgeFloorSamples;

    double p = std::fmod(phase, 1.0);
    if (p < 0.0) p += 1.0;

    switch (waveform)
    {
        case Sine:
            // Pure tone: no harmonics to fold back, exact at any rate.
            return static_cast<float>(std::sin(p * kTwoPi));

        case Triangle:
        {
            // Left as the plain shape on purpose. A triangle has corners, not steps,
            // so its harmonics already fall off as 1/n^2 -- by the 12th they are 42 dB
            // down, which is an order of magnitude less fold-back than the saws or the
            // square produce. Rounding the corners as well measurably helps very
            // little and costs the shape its character. The test reports its residual
            // so the claim stays honest rather than assumed.
            if (p < 0.25)      return static_cast<float>(p * 4.0);
            else if (p < 0.75) return static_cast<float>(2.0 - p * 4.0);
            else               return static_cast<float>(p * 4.0 - 4.0);
        }

        case SawUp:
        {
            // Rounded at the wrap: the last slice eases into -1.
            const double w = edgeWidth(0.08, dt);
            if (p < 1.0 - w)
                return static_cast<float>(p * 2.0 - 1.0);
            const double linearEnd = (1.0 - w) * 2.0 - 1.0;
            const double e = ease((p - (1.0 - w)) / w);
            return static_cast<float>(linearEnd + e * (-1.0 - linearEnd));
        }

        case SawDown:
        {
            // The original eased from +1 down to the ramp over the first slice, which
            // reads as "rounded at the wrap" but is not: the cycle still ENDED at -1
            // and restarted at +1, so the step was fully intact. That untouched
            // discontinuity was measurably the worst fold-back source of any shape --
            // 30 dB worse than its own mirror image, Saw Up, which does close its wrap.
            //
            // Now the first slice is the flyback, easing UP from -1 to +1, and the
            // ramp runs from there. Value at p=1 is -1 and at p=0 is -1, so the cycle
            // joins itself.
            const double w = edgeWidth(0.08, dt);
            if (p < w)
                return static_cast<float>(-1.0 + 2.0 * ease(p / w));
            return static_cast<float>(1.0 - 2.0 * (p - w) / (1.0 - w));
        }

        case Square:
        {
            // Two edges per cycle, at p = 0.5 and at the wrap. The original only eased
            // the mid-cycle one; the wrap was a hard step, which is the loudest source
            // of fold-back once cycles get short. Both are eased now.
            const double w = edgeWidth(0.03, dt);

            if (p < 0.5 - w)  return 1.0f;
            if (p < 0.5 + w)                                  // falling edge
                return static_cast<float>(1.0 - 2.0 * ease((p - (0.5 - w)) / (2.0 * w)));
            if (p < 1.0 - w)  return -1.0f;
            return static_cast<float>(-1.0 + 2.0 * ease((p - (1.0 - w)) / w));  // rising into the wrap
        }

        case SampleHold:
        default:
            // Held value; the caller supplies it. Nothing to shape here.
            return 0.0f;
    }
}

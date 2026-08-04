#include "NoteLockGrid.h"

#include <algorithm>
#include <cmath>
#include <limits>

double NoteLock::snapHz(double freqHz, double minHz, double maxHz, Grid grid)
{
    // log of a non-positive frequency is meaningless, and an inverted range means
    // there is nothing to snap to. Both are caller bugs rather than musical cases,
    // so fall back to the plain clamp instead of inventing a detent.
    if (freqHz <= 0.0 || minHz <= 0.0 || maxHz < minHz)
        return std::min(std::max(freqHz, minHz), maxHz);

    if (grid == Grid::Semitones)
    {
        // Detents outside the knob's range are unreachable, so clamp in SEMITONE
        // space rather than in Hz -- clamping the frequency afterwards would park the
        // knob on 20 Hz or 20 kHz, which are not themselves grid points.
        const double lowestSemis  = std::ceil (12.0 * std::log2(minHz / referenceHz));
        const double highestSemis = std::floor(12.0 * std::log2(maxHz / referenceHz));
        if (highestSemis < lowestSemis)
            return std::min(std::max(freqHz, minHz), maxHz);

        const double semis = std::min(std::max(std::round(12.0 * std::log2(freqHz / referenceHz)),
                                               lowestSemis),
                                      highestSemis);
        return referenceHz * std::pow(2.0, semis / 12.0);
    }

    //==========================================================================
    // Harmonics: k * root above the root, root / k below it.
    //
    // Unlike the semitone grid these are not evenly spaced, so there is no closed
    // form for "nearest" -- but the only partials that can possibly be nearest are
    // the two bracketing freqHz, so only those need testing.
    double best    = std::numeric_limits<double>::quiet_NaN();
    double bestErr = std::numeric_limits<double>::infinity();

    auto consider = [&] (double candidateHz)
    {
        if (candidateHz < minHz || candidateHz > maxHz)
            return;                                   // unreachable on this knob

        const double err = std::abs(std::log(freqHz / candidateHz));
        if (err < bestErr)
        {
            bestErr = err;
            best    = candidateHz;
        }
    };

    const double ratio = freqHz / referenceHz;

    if (ratio >= 1.0)
    {
        const double k = std::floor(ratio);           // >= 1 here
        consider(referenceHz * k);
        consider(referenceHz * (k + 1.0));
    }
    else
    {
        // Below the root, walk the subharmonics. 1/ratio > 1, so the divisor is >= 1
        // and the two brackets are root/m and root/(m+1).
        const double m = std::floor(1.0 / ratio);
        consider(referenceHz / m);
        consider(referenceHz / (m + 1.0));
    }

    // The root itself is always a partial; fall back to it when both brackets were
    // out of range, and to a plain clamp if even the root is unreachable.
    if (std::isnan(best))
        return (referenceHz >= minHz && referenceHz <= maxHz)
                   ? referenceHz
                   : std::min(std::max(freqHz, minHz), maxHz);

    return best;
}

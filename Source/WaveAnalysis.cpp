#include "WaveAnalysis.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>

namespace WaveAnalysis
{
namespace
{
    constexpr double twoPi = 6.283185307179586476925286766559;

    /** Nothing below this is a musical fundamental; below it we are reading room
        rumble and DC wander. */
    constexpr double lowestPitchHz = 20.0;

    /** Nothing above this either. A fundamental this high leaves no harmonics to
        analyse, and a table built from one is a sine with extra steps. */
    constexpr double highestPitchHz = 5000.0;

    /** How many periods analyseHarmonics() looks at. More periods separate the
        harmonics further apart in the spectrum, so a slightly wrong fundamental
        leaks less energy between them -- but they also have to be periods of the
        SAME steady tone, and a real instrument drifts. Eight is the point where
        those two stop trading well. */
    constexpr int analysisPeriods = 8;

    /** Where to start looking at a signal.

        Not at sample zero. The attack of a note is a burst of inharmonic noise
        that is the least periodic part of the file, and a pitch detector aimed at
        it reports whatever the pick or the hammer did. So we find the loudest
        block and then step PAST its leading edge into the body of the note. */
    int chooseAnalysisStart (const float* samples, int numSamples, int windowLen)
    {
        if (windowLen >= numSamples)
            return 0;

        const int block = std::max (256, windowLen / 8);
        const int numBlocks = numSamples / block;
        if (numBlocks < 2)
            return 0;

        int loudest = 0;
        double loudestEnergy = -1.0;

        for (int b = 0; b < numBlocks; ++b)
        {
            double energy = 0.0;
            const float* p = samples + (std::size_t) b * (std::size_t) block;
            for (int i = 0; i < block; ++i)
                energy += (double) p[i] * (double) p[i];

            if (energy > loudestEnergy)
            {
                loudestEnergy = energy;
                loudest = b;
            }
        }

        // A quarter of a window past the loudest point clears the transient
        // without wandering into the decay, where a note has lost its harmonics.
        int start = loudest * block + windowLen / 4;
        return std::max (0, std::min (start, numSamples - windowLen));
    }

    /** Peak absolute value, used to reject silence before any analysis runs. */
    float peakMagnitude (const float* samples, int numSamples)
    {
        float peak = 0.0f;
        for (int i = 0; i < numSamples; ++i)
            peak = std::max (peak, std::abs (samples[i]));
        return peak;
    }

    //==========================================================================
    /** Time-domain pitch, the YIN way.

        Slide the signal against itself and find the lag at which it best matches.
        The cumulative mean normalisation is what stops lag zero (a perfect match
        with itself) from winning, and what makes the resulting dip depth a usable
        confidence figure rather than an energy figure.

        Returns the period in samples, fractional, or 0 if nothing matched.
        dipDepth is set to the normalised difference at that lag: small is good. */
    double yinPeriod (const float* samples, int numSamples, double sampleRate, double& dipDepth)
    {
        dipDepth = 1.0;

        const int window = std::min (numSamples / 2, 4096);
        if (window < 256)
            return 0.0;

        const int tauMin = std::max (2, (int) (sampleRate / highestPitchHz));
        const int tauMax = std::min (window, (int) (sampleRate / lowestPitchHz));
        if (tauMax <= tauMin + 2)
            return 0.0;

        const int start = chooseAnalysisStart (samples, numSamples, window + tauMax);
        const float* x = samples + start;

        std::vector<double> diff ((std::size_t) tauMax + 1, 0.0);

        for (int tau = tauMin; tau <= tauMax; ++tau)
        {
            double sum = 0.0;
            for (int j = 0; j < window; ++j)
            {
                const double d = (double) x[j] - (double) x[j + tau];
                sum += d * d;
            }
            diff[(std::size_t) tau] = sum;
        }

        // Cumulative mean normalisation: divide each lag by the running average of
        // every lag before it. A lag that is merely quiet no longer looks good;
        // only a lag that is better than its neighbours does.
        std::vector<double> normalised ((std::size_t) tauMax + 1, 1.0);
        double running = 0.0;

        for (int tau = tauMin; tau <= tauMax; ++tau)
        {
            running += diff[(std::size_t) tau];
            normalised[(std::size_t) tau] = running > 0.0
                ? diff[(std::size_t) tau] * (double) (tau - tauMin + 1) / running
                : 1.0;
        }

        // Take the FIRST lag good enough to qualify, not the best one. The best is
        // routinely a multiple of the true period, because a wave that repeats
        // every period also repeats every two.
        constexpr double acceptThreshold = 0.15;
        int chosen = -1;

        for (int tau = tauMin + 1; tau < tauMax; ++tau)
        {
            if (normalised[(std::size_t) tau] < acceptThreshold
                && normalised[(std::size_t) tau] <= normalised[(std::size_t) tau + 1])
            {
                chosen = tau;
                break;
            }
        }

        if (chosen < 0)
        {
            // Nothing cleared the bar. Fall back to the deepest dip and let the
            // confidence figure say how weak it was.
            double best = 2.0;
            for (int tau = tauMin + 1; tau < tauMax; ++tau)
            {
                if (normalised[(std::size_t) tau] < best)
                {
                    best = normalised[(std::size_t) tau];
                    chosen = tau;
                }
            }

            if (chosen < 0)
                return 0.0;
        }

        dipDepth = normalised[(std::size_t) chosen];

        // Fit a parabola through the dip and its two neighbours. Whole-sample lags
        // quantise the pitch coarsely -- at 48 kHz a one-sample error on a 440 Hz
        // note is 16 cents, which is audibly out of tune.
        double period = (double) chosen;

        if (chosen > tauMin && chosen < tauMax)
        {
            const double a = diff[(std::size_t) chosen - 1];
            const double b = diff[(std::size_t) chosen];
            const double c = diff[(std::size_t) chosen + 1];
            const double denom = 2.0 * (2.0 * b - a - c);

            if (std::abs (denom) > 1.0e-12)
            {
                const double shift = (c - a) / denom;
                if (std::abs (shift) < 1.0)
                    period += shift;
            }
        }

        return period > 0.0 ? period : 0.0;
    }

    //==========================================================================
    /** How much of the signal's energy sits on whole multiples of freq.

        Read straight off the signal at the frequencies in question rather than
        from an FFT grid. A grid puts a 33 Hz fundamental between two bins six Hz
        apart and forces a guess about where in between it fell, while this asks
        the exact question: how much energy is there at f, 2f, 3f and so on.

        Measured over the first few harmonics only. Beyond that a real instrument
        has too little left for the answer to mean anything. */
    double harmonicSupport (const float* x, const double* window, int windowLen,
                            double sampleRate, double freq)
    {
        constexpr int harmonicsToWeigh = 16;

        if (freq <= 0.0 || windowLen < 16)
            return 0.0;

        const int count = std::min (harmonicsToWeigh, (int) ((sampleRate * 0.5) / freq) - 1);
        if (count < 1)
            return 0.0;

        double total = 0.0;

        for (int h = 1; h <= count; ++h)
        {
            const double step = twoPi * (double) h * freq / sampleRate;
            const std::complex<double> increment (std::cos (step), -std::sin (step));
            std::complex<double> phasor (1.0, 0.0);

            double re = 0.0, im = 0.0;

            for (int n = 0; n < windowLen; ++n)
            {
                const double weighted = (double) x[n] * window[(std::size_t) n];
                re += weighted * phasor.real();
                im += weighted * phasor.imag();
                phasor *= increment;
            }

            total += re * re + im * im;
        }

        return total;
    }

    /** Settle the octave of a candidate pitch.

        A difference function is accurate about WHERE the wave repeats but not
        about how often: a wave that repeats every period also repeats every two,
        and a wave with a strong second harmonic looks like it repeats twice as
        fast. Both mistakes are whole-number ratios of the truth, so testing that
        handful of ratios against the spectrum settles it.

        The rule is: take the HIGHEST candidate that still accounts for nearly all
        the energy the best candidate does. Half of a true fundamental always
        accounts for at least as much as the truth, since its harmonics include
        every one of the truth's -- so simply taking the best would walk down
        forever. Twice a true fundamental, on the other hand, throws away the
        fundamental itself and every odd harmonic with it, and fails the test at
        once. */
    double settleOctave (const float* samples, int numSamples, double sampleRate,
                         double candidateHz, bool& corrected)
    {
        corrected = false;

        const int windowLen = std::min (numSamples, 8192);
        if (windowLen < 512 || candidateHz <= 0.0)
            return candidateHz;

        const int start = chooseAnalysisStart (samples, numSamples, windowLen);
        const float* x = samples + start;

        std::vector<double> window ((std::size_t) windowLen);
        for (int n = 0; n < windowLen; ++n)
            window[(std::size_t) n] = 0.5 - 0.5 * std::cos (twoPi * (double) n / (double) windowLen);

        // Ordered low to high, because the choice below wants the highest one
        // that qualifies.
        static const double ratios[] = { 0.25, 1.0 / 3.0, 0.5, 1.0, 2.0 };
        constexpr int numRatios = 5;

        double support[numRatios] = {};
        double best = 0.0;

        for (int i = 0; i < numRatios; ++i)
        {
            const double freq = candidateHz * ratios[i];
            if (freq < lowestPitchHz || freq > highestPitchHz)
                continue;

            support[i] = harmonicSupport (x, window.data(), windowLen, sampleRate, freq);
            best = std::max (best, support[i]);
        }

        if (best <= 0.0)
            return candidateHz;

        // "Nearly all" is 85%. Tight enough that dropping the fundamental and all
        // the odd harmonics never passes, loose enough that a real recording --
        // where the harmonics are never exactly whole multiples -- still does.
        constexpr double keepThreshold = 0.85;

        for (int i = numRatios - 1; i >= 0; --i)
        {
            if (support[i] >= best * keepThreshold)
            {
                corrected = (ratios[i] != 1.0);
                return candidateHz * ratios[i];
            }
        }

        return candidateHz;
    }
}

//==============================================================================
void fft (float* real, float* imag, int numSamples, bool inverse)
{
    if (real == nullptr || imag == nullptr || numSamples < 2)
        return;

    // Power of two only. Every caller here sizes its own block, so a bad size is
    // a programming error rather than something to paper over at run time.
    if ((numSamples & (numSamples - 1)) != 0)
        return;

    // Bit-reversal permutation, done in place by counting in reverse.
    for (int i = 1, j = 0; i < numSamples; ++i)
    {
        int bit = numSamples >> 1;
        for (; (j & bit) != 0; bit >>= 1)
            j ^= bit;
        j ^= bit;

        if (i < j)
        {
            std::swap (real[i], real[j]);
            std::swap (imag[i], imag[j]);
        }
    }

    const double sign = inverse ? 1.0 : -1.0;

    for (int len = 2; len <= numSamples; len <<= 1)
    {
        const double angle = sign * twoPi / (double) len;
        const double wReal = std::cos (angle);
        const double wImag = std::sin (angle);

        for (int i = 0; i < numSamples; i += len)
        {
            // Recomputed per block rather than carried across the whole transform,
            // so the rotation error cannot accumulate over numSamples steps.
            double curReal = 1.0, curImag = 0.0;

            for (int k = 0; k < len / 2; ++k)
            {
                const double uReal = real[i + k];
                const double uImag = imag[i + k];
                const double vReal = real[i + k + len / 2] * curReal - imag[i + k + len / 2] * curImag;
                const double vImag = real[i + k + len / 2] * curImag + imag[i + k + len / 2] * curReal;

                real[i + k] = (float) (uReal + vReal);
                imag[i + k] = (float) (uImag + vImag);
                real[i + k + len / 2] = (float) (uReal - vReal);
                imag[i + k + len / 2] = (float) (uImag - vImag);

                const double nextReal = curReal * wReal - curImag * wImag;
                curImag = curReal * wImag + curImag * wReal;
                curReal = nextReal;
            }
        }
    }

    if (inverse)
    {
        const float scale = 1.0f / (float) numSamples;
        for (int i = 0; i < numSamples; ++i)
        {
            real[i] *= scale;
            imag[i] *= scale;
        }
    }
}

//==============================================================================
PitchEstimate detectFundamental (const float* samples, int numSamples, double sampleRate)
{
    PitchEstimate result;

    if (samples == nullptr || numSamples < 512 || sampleRate <= 0.0)
        return result;

    // Silence has no pitch, and the detectors would happily invent one from
    // denormals and dither.
    if (peakMagnitude (samples, numSamples) < 1.0e-5f)
        return result;

    double dipDepth = 1.0;
    const double period = yinPeriod (samples, numSamples, sampleRate, dipDepth);

    if (period <= 0.0)
        return result;

    const double frequency = settleOctave (samples, numSamples, sampleRate,
                                           sampleRate / period, result.octaveCorrected);

    result.frequencyHz = std::min (std::max (frequency, lowestPitchHz), highestPitchHz);

    // The dip depth is a difference measure, so subtract it from one to get
    // something that reads as "how sure".
    result.confidence = std::min (1.0, std::max (0.0, 1.0 - dipDepth));

    return result;
}

//==============================================================================
HarmonicSet analyseHarmonics (const float* samples, int numSamples, double sampleRate,
                              double fundamentalHz)
{
    HarmonicSet set;

    if (samples == nullptr || numSamples < 64 || sampleRate <= 0.0
        || fundamentalHz < lowestPitchHz || fundamentalHz > highestPitchHz)
        return set;

    const double period = sampleRate / fundamentalHz;

    // Use as many whole periods as the file can supply, down to one. A window cut
    // to a whole number of periods puts every harmonic exactly on an analysis
    // frequency, so none of them leak into their neighbours.
    int periods = analysisPeriods;
    while (periods > 1 && (int) (period * (double) periods) > numSamples)
        --periods;

    const int windowLen = (int) (period * (double) periods);
    if (windowLen < 16 || windowLen > numSamples)
        return set;

    const int start = chooseAnalysisStart (samples, numSamples, windowLen);
    const float* x = samples + start;

    const int nyquistHarmonic = (int) ((sampleRate * 0.5) / fundamentalHz);
    const int maxHarmonics = std::min (tableSize / 2 - 1, std::max (0, nyquistHarmonic - 1));
    if (maxHarmonics < 1)
        return set;

    // Hann over the window. Even with whole periods the ends of the window do not
    // match exactly, because windowLen was rounded to a sample.
    std::vector<double> window ((std::size_t) windowLen);
    double windowSum = 0.0;
    for (int n = 0; n < windowLen; ++n)
    {
        window[(std::size_t) n] = 0.5 - 0.5 * std::cos (twoPi * (double) n / (double) windowLen);
        windowSum += window[(std::size_t) n];
    }

    if (windowSum <= 0.0)
        return set;

    set.amplitude.resize ((std::size_t) maxHarmonics, 0.0f);
    set.phase.resize ((std::size_t) maxHarmonics, 0.0f);

    for (int h = 1; h <= maxHarmonics; ++h)
    {
        // One rotating phasor per harmonic, stepped by multiplication instead of
        // calling sin and cos per sample. Over a few thousand steps in double the
        // magnitude drifts by about a part in 10^13.
        const double step = twoPi * (double) h * fundamentalHz / sampleRate;
        const std::complex<double> increment (std::cos (step), -std::sin (step));
        std::complex<double> phasor (1.0, 0.0);

        double re = 0.0, im = 0.0;

        for (int n = 0; n < windowLen; ++n)
        {
            const double weighted = (double) x[n] * window[(std::size_t) n];
            re += weighted * phasor.real();
            im += weighted * phasor.imag();
            phasor *= increment;
        }

        // Two over the window sum converts the correlation into the peak
        // amplitude of a cosine at this frequency.
        const double scale = 2.0 / windowSum;
        set.amplitude[(std::size_t) (h - 1)] = (float) (scale * std::sqrt (re * re + im * im));
        set.phase[(std::size_t) (h - 1)] = (float) std::atan2 (im, re);
    }

    return set;
}

//==============================================================================
void renderTable (const HarmonicSet& harmonics, int maxHarmonics, float* out)
{
    if (out == nullptr)
        return;

    std::fill (out, out + tableSize, 0.0f);

    if (harmonics.isEmpty() || maxHarmonics < 1)
        return;

    const int count = std::min (std::min (maxHarmonics, harmonics.size()), tableSize / 2 - 1);
    if (count < 1)
        return;

    // Build the spectrum and transform it back, rather than summing cosines per
    // sample. Same answer, but N log N instead of N times the harmonic count --
    // and the whole ladder of tables gets rendered on every import.
    std::vector<float> re ((std::size_t) tableSize, 0.0f);
    std::vector<float> im ((std::size_t) tableSize, 0.0f);

    for (int h = 1; h <= count; ++h)
    {
        const double a = 0.5 * (double) harmonics.amplitude[(std::size_t) (h - 1)];
        const double p = (double) harmonics.phase[(std::size_t) (h - 1)];

        // Scaled by tableSize because the inverse transform divides by it, and
        // mirrored into the negative frequency so the result comes out real.
        const double reVal = a * std::cos (p) * (double) tableSize;
        const double imVal = a * std::sin (p) * (double) tableSize;

        re[(std::size_t) h] = (float) reVal;
        im[(std::size_t) h] = (float) imVal;
        re[(std::size_t) (tableSize - h)] = (float) reVal;
        im[(std::size_t) (tableSize - h)] = (float) -imVal;
    }

    fft (re.data(), im.data(), tableSize, true);
    std::copy (re.begin(), re.end(), out);
}

//==============================================================================
void renderMipmaps (const HarmonicSet& harmonics, float* out)
{
    if (out == nullptr)
        return;

    for (int level = 0; level < mipLevels; ++level)
        renderTable (harmonics, harmonicsAtLevel (level), out + (std::size_t) level * tableSize);

    // One gain for the whole ladder, measured on the richest level. Normalising
    // each level separately would raise the volume of a note as it crossed into
    // the next level, because dropping harmonics lowers the peak of the shape.
    float peak = 0.0f;
    for (int i = 0; i < tableSize; ++i)
        peak = std::max (peak, std::abs (out[i]));

    if (peak <= 1.0e-9f)
        return;

    const float gain = 1.0f / peak;
    for (int i = 0; i < mipLevels * tableSize; ++i)
        out[i] *= gain;
}

//==============================================================================
int levelForFrequency (double freqHz, double sampleRate)
{
    if (freqHz <= 0.0 || sampleRate <= 0.0)
        return 0;

    // Level L holds (tableSize/2) >> L harmonics, so the condition
    //     harmonicsAtLevel(L) * freq < nyquist
    // rearranges to 2^(10 - L) < nyquist / freq, and the smallest L that
    // satisfies it falls straight out of the exponent of that ratio. ilogb is a
    // single instruction, where searching the ladder was a loop -- and this runs
    // once per sample, per oscillator, per voice.
    const int exponent = std::ilogb ((sampleRate * 0.5) / freqHz);
    const int level = (mipLevels - 1) - exponent;

    return level < 0 ? 0 : (level > mipLevels - 1 ? mipLevels - 1 : level);
}

//==============================================================================
HarmonicSet harmonicsFromTable (const float* table)
{
    HarmonicSet set;

    if (table == nullptr)
        return set;

    std::vector<float> re (table, table + tableSize);
    std::vector<float> im ((std::size_t) tableSize, 0.0f);

    fft (re.data(), im.data(), tableSize, false);

    const int count = tableSize / 2 - 1;
    set.amplitude.resize ((std::size_t) count);
    set.phase.resize ((std::size_t) count);

    for (int h = 1; h <= count; ++h)
    {
        // Two over the length undoes the transform's scaling and folds in the
        // mirrored negative-frequency half, which carries the other half of the
        // energy of a real signal.
        const double a = (double) re[(std::size_t) h];
        const double b = (double) im[(std::size_t) h];

        set.amplitude[(std::size_t) (h - 1)] = (float) (2.0 * std::sqrt (a * a + b * b) / (double) tableSize);
        set.phase[(std::size_t) (h - 1)] = (float) std::atan2 (b, a);
    }

    return set;
}

//==============================================================================
double playbackPhaseScale (double fileSampleRate, double rootHz, int loopLength)
{
    if (fileSampleRate <= 0.0 || rootHz <= 0.0 || loopLength <= 0)
        return 1.0;

    return fileSampleRate / (rootHz * (double) loopLength);
}

//==============================================================================
double semitonesFromMiddleC (double freqHz)
{
    if (freqHz <= 0.0)
        return 0.0;

    return 12.0 * std::log2 (freqHz / middleCHz);
}

void describePitch (double freqHz, char* name, std::size_t capacity)
{
    if (name == nullptr || capacity == 0)
        return;

    name[0] = '\0';

    if (freqHz <= 0.0)
        return;

    static const char* const noteNames[] =
        { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

    const double midi = 69.0 + 12.0 * std::log2 (freqHz / 440.0);
    const int nearest = (int) std::lround (midi);
    const int cents = (int) std::lround ((midi - (double) nearest) * 100.0);

    // Middle C is MIDI 60 and is called C4 here, which is the naming that puts
    // A440 in octave 4 alongside it.
    const int octave = nearest / 12 - 1;
    const int pitchClass = ((nearest % 12) + 12) % 12;

    std::snprintf (name, capacity, "%s%d %+dc", noteNames[pitchClass], octave, cents);
}

}

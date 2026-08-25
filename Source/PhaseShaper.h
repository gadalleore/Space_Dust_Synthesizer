#pragma once

#include "OscillatorShapes.h"

#include <cmath>

/**
    Bend, Spectrum and Sync -- the things done to an oscillator's PHASE before a
    waveform is read from it.

    WHY THE PHASE AND NOT THE WAVE

    Bending the phase costs one curve and works on everything. The oscillator
    hands over a position in its cycle; this moves that position; whatever is
    read at the new position comes out bent. It does not care whether the thing
    being read is one of the twenty-one built-in shapes or a single cycle the
    player imported, because it never looks at it.

    Bending the WAVE instead would mean rebuilding a table on every turn of the
    Intensity knob, which cannot be modulated and cannot be automated smoothly.
    That is the whole reason anyone reaches for these.

    WHAT EACH MODE DOES

      Bend +      compresses the front of the cycle and stretches the back, so
                  the waveform leans forward. A sine grows a hard edge and starts
                  to sound like a saw.

      Bend -      the same the other way. The waveform leans back and softens.

      Bend +/-    both at once: an S, pulling the cycle away from its middle and
                  towards its ends. Hollow, and the strongest of the three.

      Spectrum    not a bend at all. It fades the shape towards a plain sine,
                  which is what taking the upper harmonics out sounds like.
                  Handled by shapedValue() rather than by warping the phase,
                  because a sine cannot be reached by moving where you read.

      Sync        restarts the cycle several times per note. The phase is
                  multiplied and wrapped, so the wave is cut off mid-cycle and
                  begins again -- the hard-sync tear.

    NO JUCE, so every curve here is checked against known phases without a plugin
    host. See tools/shapertest.
*/
namespace PhaseShaper
{
    /** What Intensity does to the phase. Order is a parameter contract: these
        indices are stored in presets and automation lanes. Append only. */
    enum Mode : int
    {
        Standard = 0,
        BendPlus = 1,
        BendMinus = 2,
        BendPlusMinus = 3,
        Spectrum = 4,

        numModes = 5
    };

    inline const char* const modeNames[] =
    {
        "Standard", "Bend +", "Bend -", "Bend +/-", "Spectrum"
    };

    /** How many times over a cycle may repeat at full Sync.

        Eight is the point where the tear is dramatic and the result is still a
        pitch rather than a noise. Past that the sync sweep stops reading as a
        sweep. */
    inline constexpr double maxSyncRatio = 8.0;

    /** The strongest bend Intensity can ask for.

        The curve is p^e. At 4.0 the front of the cycle is squeezed into a very
        small corner, which is as far as it can go before the waveform is mostly
        one edge and the pitch starts to smear. */
    inline constexpr double maxBendExponent = 4.0;

    /**
        Where to actually read, given where the oscillator has got to.

        phase01   how far through the note's own cycle, 0..1. Wrapped here, so any
                  positive number is fine.
        mode      one of the Mode values above.
        intensity 0..1. At 0 every mode returns the phase untouched, so Standard
                  and a turned-down Intensity are the same thing and the player
                  can leave the mode set to whatever they like.
        sync      0..1. At 0 the cycle runs once per note, as it always did.

        Sync is applied FIRST and the bend inside the result, so the bend shapes
        each repeat rather than the run of them. That ordering is what makes the
        two controls usable together: the other way round, sync would chop up an
        already-bent cycle and the bend would stop being audible at all.
    */
    inline double shapedPhase (double phase01, int mode, double intensity, double sync) noexcept
    {
        double p = phase01 - std::floor (phase01);

        // -- Sync ---------------------------------------------------------------
        // Multiplying the phase and wrapping IS hard sync: the master runs 0..1
        // once per note, so frac(p * ratio) runs 0..1 `ratio` times and snaps back
        // to zero every time the master comes round. No second oscillator and no
        // reset flag -- the wrap does it.
        if (sync > 0.0)
        {
            const double ratio = 1.0 + sync * (maxSyncRatio - 1.0);
            p *= ratio;
            p -= std::floor (p);
        }

        // -- Bend ---------------------------------------------------------------
        if (intensity <= 0.0)
            return p;

        const double amount = intensity > 1.0 ? 1.0 : intensity;

        switch (mode)
        {
            case BendPlus:
            {
                // p^e with e above 1: early phases are pulled towards zero, so
                // the first part of the wave is squeezed and the rest spreads out.
                const double e = 1.0 + amount * (maxBendExponent - 1.0);
                return std::pow (p, e);
            }

            case BendMinus:
            {
                // The inverse curve, so the wave leans the other way.
                const double e = 1.0 + amount * (maxBendExponent - 1.0);
                return std::pow (p, 1.0 / e);
            }

            case BendPlusMinus:
            {
                // Each half bent towards the middle, mirrored -- an S through
                // (0.5, 0.5). Both ends of the cycle are stretched and the middle
                // is squeezed, which is why this one sounds hollow.
                const double e = 1.0 + amount * (maxBendExponent - 1.0);

                if (p < 0.5)
                    return 0.5 * std::pow (p * 2.0, e);

                return 1.0 - 0.5 * std::pow ((1.0 - p) * 2.0, e);
            }

            case Spectrum:
            case Standard:
            default:
                // Neither of these moves the phase. Spectrum acts on the VALUE and
                // is applied in shapedValue below.
                return p;
        }
    }

    /**
        A built-in shape read through the whole chain: sync, bend, then Spectrum.

        Kept next to shapedPhase rather than left to each caller, so the
        oscillator and the Waveforms picture apply the same three things in the
        same order. They disagreed about the shapes once already.
    */
    inline float shapedValue (int shape, double phase01, int mode,
                              double intensity, double sync) noexcept
    {
        const double p = shapedPhase (phase01, mode, intensity, sync);
        const float v = OscShape::shapeValue (shape, p);

        if (mode != Spectrum || intensity <= 0.0)
            return v;

        // Fade towards a plain sine at the SAME phase. Taking the upper harmonics
        // out of a shape leaves its fundamental, and its fundamental is a sine, so
        // at full intensity that is what is left.
        //
        // A crossfade, not a filter: a filter would need state, and this has to
        // give the same answer for a phase whether it is asked once per sample by
        // the oscillator or four thousand times in a row by the picture.
        const double amount = intensity > 1.0 ? 1.0 : intensity;
        const float sine = (float) std::sin (OscShape::twoPi * p);

        return (float) (v * (1.0 - amount) + sine * amount);
    }

    /** Whether these settings change anything at all.

        The oscillator asks this once per block and takes a plainer path when the
        answer is no, so a patch that uses none of this pays nothing for it. */
    inline bool isActive (int mode, double intensity, double sync) noexcept
    {
        if (sync > 0.0)
            return true;

        return intensity > 0.0 && mode != Standard;
    }
}

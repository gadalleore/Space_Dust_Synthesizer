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

    Bending the WAVE instead would mean rebuilding a table on every turn of a
    knob, which cannot be modulated and cannot be automated smoothly. That is the
    whole reason anyone reaches for these.

    WHY FOUR KNOBS AND NOT A MODE

    Massive makes these a menu: one mode at a time, with a single Intensity knob
    behind it. Here they are four separate amounts and any of them may be turned
    up together, because they genuinely compose:

      Bend +    compresses the front of the cycle and stretches the back, so the
                waveform leans forward. A sine grows a hard edge.

      Bend -    the same the other way. The waveform leans back and softens.

                These two are inverse curves, so they are folded into ONE net
                exponent before either is applied -- turning both up halfway
                really does cancel out, which is what a player would expect and
                what applying them in series would NOT have given.

      Bend +/-  an S through the middle of the cycle, pulling it away from its
                centre and towards both ends. A different curve, not a mixture of
                the two above, so it is applied after them rather than folded in.

      Spectrum  not a bend at all. It fades the shape towards a plain sine, which
                is what taking the upper harmonics out sounds like. It acts on the
                VALUE, so it comes last of all.

      Sync      restarts the cycle several times per note. Applied FIRST, so
                everything else shapes each repeat rather than the run of them.

    NO JUCE, so every curve here is checked against known phases without a plugin
    host. See tools/shapertest.
*/
namespace PhaseShaper
{
    /** Every shaping amount for one oscillator, each 0..1.

        All zero means "do nothing", and the oscillator checks for exactly that
        once per block and takes its old plain path -- so a patch that uses none
        of this pays nothing for it and sounds as it always did. */
    struct Amounts
    {
        double bendPlus = 0.0;
        double bendMinus = 0.0;
        double bendPlusMinus = 0.0;
        double spectrum = 0.0;
        double sync = 0.0;
    };

    /** The four shaping knobs, in the order they are drawn. Sync is deliberately
        not in here: it is a different kind of thing and gets its own knob. */
    inline const char* const knobNames[] =
    {
        "Bend +", "Bend -", "Bend +/-", "Spectrum"
    };

    inline constexpr int numKnobs = 4;

    /** How many times over a cycle may repeat at full Sync.

        Eight is the point where the tear is dramatic and the result is still a
        pitch rather than a noise. Past that the sync sweep stops reading as one. */
    inline constexpr double maxSyncRatio = 8.0;

    /** The strongest bend one knob can ask for, as an exponent on the phase.

        At 4.0 the front of the cycle is squeezed into a very small corner, which
        is as far as it goes before the waveform is mostly one edge and the pitch
        starts to smear. */
    inline constexpr double maxBendExponent = 4.0;

    namespace detail
    {
        inline double clamp01 (double v) noexcept
        {
            return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
        }
    }

    /**
        Where to actually read, given where the oscillator has got to.

        The phase is wrapped here, so any positive number is fine.

        Order is Sync, then the two opposed bends as one exponent, then the S.
        Each step maps 0..1 onto 0..1 and never runs backwards, so the result is
        always a real position in the cycle and the wave never plays in reverse.
    */
    inline double shapedPhase (double phase01, const Amounts& a) noexcept
    {
        double p = phase01 - std::floor (phase01);

        // -- Sync ---------------------------------------------------------------
        // Multiplying the phase and wrapping IS hard sync: the master runs 0..1
        // once per note, so frac(p * ratio) runs 0..1 `ratio` times and snaps back
        // to zero every time the master comes round. No second oscillator and no
        // reset flag -- the wrap does it.
        const double sync = detail::clamp01 (a.sync);

        if (sync > 0.0)
        {
            const double ratio = 1.0 + sync * (maxSyncRatio - 1.0);
            p *= ratio;
            p -= std::floor (p);
        }

        // -- The two opposed bends, as one exponent -----------------------------
        // p^up and p^(1/down) are inverses, so they are divided rather than
        // applied one after the other. Turning both fully up leaves the phase
        // exactly as it was, which is the only behaviour that makes two knobs
        // pointing opposite ways feel honest.
        const double up = 1.0 + detail::clamp01 (a.bendPlus) * (maxBendExponent - 1.0);
        const double down = 1.0 + detail::clamp01 (a.bendMinus) * (maxBendExponent - 1.0);
        const double exponent = up / down;

        if (exponent != 1.0)
            p = std::pow (p, exponent);

        // -- The S --------------------------------------------------------------
        // Each half bent towards the middle and mirrored, so both ends of the
        // cycle stretch and the middle squeezes. A different curve from the two
        // above, which is why it goes on top of them instead of into them.
        const double s = detail::clamp01 (a.bendPlusMinus);

        if (s > 0.0)
        {
            const double e = 1.0 + s * (maxBendExponent - 1.0);

            p = p < 0.5 ? 0.5 * std::pow (p * 2.0, e)
                        : 1.0 - 0.5 * std::pow ((1.0 - p) * 2.0, e);
        }

        return p;
    }

    /**
        A built-in shape read through the whole chain, Spectrum included.

        Kept next to shapedPhase rather than left to each caller, so the
        oscillator and the Waveforms picture apply the same things in the same
        order. They disagreed about the shapes once already.
    */
    inline float shapedValue (int shape, double phase01, const Amounts& a) noexcept
    {
        const double p = shapedPhase (phase01, a);
        const float v = OscShape::shapeValue (shape, p);

        const double spectrum = detail::clamp01 (a.spectrum);

        if (spectrum <= 0.0)
            return v;

        // Fade towards a plain sine at the SAME phase. Taking the upper harmonics
        // out of a shape leaves its fundamental, and its fundamental is a sine, so
        // at full this is what is left.
        //
        // A crossfade, not a filter: a filter would need state, and this has to
        // give the same answer for a phase whether it is asked once per sample by
        // the oscillator or four thousand times in a row by the picture.
        const float sine = (float) std::sin (OscShape::twoPi * p);

        return (float) (v * (1.0 - spectrum) + sine * spectrum);
    }

    /** Whether any of it would change anything.

        The oscillator asks this once per block and takes a plainer path when the
        answer is no, so a patch that uses none of this pays nothing for it. */
    inline bool isActive (const Amounts& a) noexcept
    {
        return a.bendPlus > 0.0 || a.bendMinus > 0.0
            || a.bendPlusMinus > 0.0 || a.spectrum > 0.0 || a.sync > 0.0;
    }
}

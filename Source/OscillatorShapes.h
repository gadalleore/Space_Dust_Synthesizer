#pragma once

#include <cmath>

/**
    The built-in oscillator shapes, and the one place their maths is written down.

    WHY THIS IS A FILE OF ITS OWN

    A shape used to be written twice: once in SynthVoice::generateWaveform, which
    is what you HEAR, and once in WaveformEditorComponent::builtInShapeValue,
    which is what you SEE in the Waveforms list. With four shapes that was a
    small risk. With twenty-one it is a certainty -- the picture and the sound
    would drift apart, and the picture is what the player picks a shape by.

    So both now call shapeValue(). One switch, one answer, one place to fix.

    NO JUCE, so it can be checked against generated phases without a plugin host
    -- see tools/shapestest. That is the same arrangement WaveAnalysis,
    NoteLockGrid and ResampleCapture already use.

    NO BAND LIMITING. Every shape here is computed naively from the phase, which
    is what Saw and Square already did before there were any others. The
    oscillators run at kOscOSFactor times the host rate (see SynthVoice), and
    that oversampling is what keeps the fold-back down. A shape that needs more
    than that belongs in a wavetable, not here.

    ORDER IS A CONTRACT. These indices are what a preset and a host automation
    lane store. Adding a shape anywhere but the END moves every shape after it,
    and moves every User slot with them. See UserWave::oscUserBase and
    SpaceDustAudioProcessor::migrateWaveformChoicesIfOld.
*/
namespace OscShape
{
    /** The four shapes that existed before the families below were added.

        Their maths is copied exactly, not rewritten: index 2 must still be the
        same saw it always was, down to the sample, or every preset ever saved
        would come back sounding slightly different. */
    enum : int
    {
        Sine = 0,
        Triangle = 1,
        Saw = 2,
        Square = 3,

        // -- Classic analogue --
        Pulse25 = 4,
        Pulse12 = 5,
        HalfSine = 6,
        Trapezoid = 7,
        RampDown = 8,

        // -- Harmonic / organ --
        Sine5th = 9,
        Organ = 10,
        OddHarmonics = 11,
        Bell = 12,

        // -- Digital / aggressive --
        StackSaw = 13,
        FoldSine = 14,
        ExpSaw = 15,
        Stairs = 16,

        // -- Soft / smooth --
        RoundSquare = 17,
        Parabola = 18,
        SineCubed = 19,
        HyperTriangle = 20,

        /** How many built-in shapes there are. UserWave::oscUserBase must equal
            this: the import slots begin where the built-ins run out. */
        numShapes = 21
    };

    inline constexpr double twoPi = 6.283185307179586;

    /** The names, in index order. Index i is names[i]. */
    inline const char* const names[] =
    {
        "Sine", "Triangle", "Saw", "Square",
        "Pulse 25%", "Pulse 12%", "Half Sine", "Trapezoid", "Ramp Down",
        "Sine 5th", "Organ", "Odd Harmonics", "Bell",
        "Stack Saw", "Fold Sine", "Exp Saw", "Stairs",
        "Round Square", "Parabola", "Sine Cubed", "Hyper Triangle"
    };

    namespace detail
    {
        /** The existing saw, exactly as SynthVoice wrote it: zero at phase 0,
            climbing to +1 at the half turn, jumping to -1 and climbing back. */
        inline double rawSaw (double p) noexcept
        {
            return 2.0 * (p - std::floor (p + 0.5));
        }

        /** The existing triangle, exactly as SynthVoice wrote it. */
        inline double rawTriangle (double p) noexcept
        {
            return 4.0 * std::abs (p - std::floor (p + 0.5)) - 1.0;
        }

        /** Keep a sign while changing a magnitude. Used by the shapes that bend
            a curve without folding it over. */
        inline double signedPow (double v, double exponent) noexcept
        {
            return v < 0.0 ? -std::pow (-v, exponent) : std::pow (v, exponent);
        }

        /** Fold anything outside -1..1 back inside, as a wavefolder does: the
            curve turns around at the rail instead of flattening against it. */
        inline double fold (double v) noexcept
        {
            for (int guard = 0; guard < 8 && (v > 1.0 || v < -1.0); ++guard)
                v = v > 1.0 ? 2.0 - v : -2.0 - v;

            return v;
        }
    }

    /**
        One cycle of a shape, from a phase of 0..1.

        The phase is taken modulo 1 here, so a caller may hand in any positive
        number and need not wrap it first.

        Every shape returns roughly -1..1 and is centred on zero. "Roughly"
        because the additive ones are normalised by their peak rather than
        measured: a few of them reach a little under 1, none reaches over.
    */
    inline float shapeValue (int shape, double phase01) noexcept
    {
        double p = phase01 - std::floor (phase01);

        switch (shape)
        {
            //-- The original four. Unchanged maths, deliberately. ------------
            case Sine:      return (float) std::sin (twoPi * p);
            case Triangle:  return (float) detail::rawTriangle (p);
            case Saw:       return (float) detail::rawSaw (p);
            case Square:    return std::sin (twoPi * p) > 0.0 ? 1.0f : -1.0f;

            //-- Classic analogue ---------------------------------------------
            // A narrow pulse is the same square with the corner moved. The
            // narrower it is the more it whistles: 12% is the thin reedy one.
            case Pulse25:   return p < 0.25 ? 1.0f : -1.0f;
            case Pulse12:   return p < 0.125 ? 1.0f : -1.0f;

            case HalfSine:
            {
                // Half-wave rectified: the top half of a sine, then nothing.
                // Rectifying leaves a DC offset of 1/pi, which is taken back out
                // -- a waveform with DC in it makes a click at every note.
                constexpr double mean = 0.3183098861837907;   // 1/pi
                const double s = p < 0.5 ? std::sin (twoPi * p) : 0.0;
                return (float) ((s - mean) / (1.0 - mean));
            }

            case Trapezoid:
            {
                // A triangle driven into its rails: the slopes stay, the peaks
                // flatten. Between a triangle and a square, and the classic
                // shape for a brass-like tone.
                const double t = detail::rawTriangle (p) * 2.0;
                return (float) (t > 1.0 ? 1.0 : (t < -1.0 ? -1.0 : t));
            }

            // The saw the other way up. Same harmonics, opposite slope -- which
            // matters the moment it is the modulator rather than the sound.
            case RampDown:  return (float) (-detail::rawSaw (p));

            //-- Harmonic / organ ---------------------------------------------
            // Built by adding partials rather than by bending a curve, so these
            // stay smooth however hard they are pushed.
            case Sine5th:
            {
                // The fundamental with its third harmonic -- an octave and a
                // fifth above. Hollow, and it sits under a lead well.
                const double x = twoPi * p;
                return (float) ((std::sin (x) + 0.6 * std::sin (3.0 * x)) / 1.6);
            }

            case Organ:
            {
                // Drawbar-style: fundamental, octave, twelfth.
                const double x = twoPi * p;
                return (float) ((std::sin (x)
                                 + 0.5 * std::sin (2.0 * x)
                                 + 0.33 * std::sin (3.0 * x)) / 1.83);
            }

            case OddHarmonics:
            {
                // Odd harmonics at 1/h -- a square wave built the honest way,
                // stopping at the seventh. Square-like, without the corners that
                // make a square alias.
                const double x = twoPi * p;
                const double v = std::sin (x)
                               + std::sin (3.0 * x) / 3.0
                               + std::sin (5.0 * x) / 5.0
                               + std::sin (7.0 * x) / 7.0;
                return (float) (v / 1.2);
            }

            case Bell:
            {
                // Widely spaced partials, so it rings rather than buzzes. Kept
                // harmonic -- a truly inharmonic partial would not close the
                // cycle, and a shape that does not close clicks once per turn.
                const double x = twoPi * p;
                const double v = std::sin (x)
                               + 0.4 * std::sin (4.0 * x)
                               + 0.2 * std::sin (9.0 * x);
                return (float) (v / 1.6);
            }

            //-- Digital / aggressive -----------------------------------------
            case StackSaw:
            {
                // Four saws at spread phases, added.
                //
                // NOT a supersaw, and not named one. A supersaw DETUNES its
                // saws, which needs several oscillators running at different
                // speeds; one cycle of one wave cannot hold that. Spreading the
                // phase instead gives the thickness without the drift.
                const double v = detail::rawSaw (p)
                               + detail::rawSaw (p + 0.11)
                               + detail::rawSaw (p + 0.27)
                               + detail::rawSaw (p + 0.41);
                return (float) (v * 0.4);
            }

            case FoldSine:
            {
                // A sine pushed past the rails and folded back. Adds a burst of
                // high partials that a sine has none of, and stays continuous.
                return (float) detail::fold (2.6 * std::sin (twoPi * p));
            }

            case ExpSaw:
            {
                // A saw whose ramp is a curve, so the energy piles into the end
                // of the cycle. Brighter and harder than a straight saw.
                //
                // A curved ramp is NOT centred on zero the way a straight one is,
                // and the offset is big -- about -0.44 before correction. The two
                // constants below take it out and put the peak back at 1. Both
                // are worked out from k, not guessed:
                //
                //   e(p)   = (exp(k p) - 1) / (exp(k) - 1)
                //   mean e = ((exp(k) - 1)/k - 1) / (exp(k) - 1)   [integrate e]
                //   mean v = 2 (mean e) - 1                        [v = 2e - 1]
                //   peak   = 1 - mean v                            [v tops out at 1]
                //
                // The shape test measures the DC that is actually left, so a
                // wrong constant here fails there rather than in the player's ear.
                constexpr double k = 3.0;
                constexpr double meanV = -0.4381225891;
                constexpr double peak = 1.4381225891;

                const double e = (std::exp (k * p) - 1.0) / (std::exp (k) - 1.0);
                return (float) ((2.0 * e - 1.0 - meanV) / peak);
            }

            case Stairs:
            {
                // A saw cut into eight steps -- what a saw sounds like through a
                // three-bit converter. Harsh on purpose.
                constexpr double steps = 8.0;
                return (float) (2.0 * (std::floor (p * steps) / (steps - 1.0)) - 1.0);
            }

            //-- Soft / smooth ------------------------------------------------
            case RoundSquare:
            {
                // A square with its corners taken off. Nearly as full as a
                // square, with far less of what makes one alias.
                constexpr double drive = 4.0;
                return (float) (std::tanh (drive * std::sin (twoPi * p))
                                / std::tanh (drive));
            }

            case Parabola:
            {
                // One arch per cycle. Very close to a sine, a little fuller --
                // a good sub, and gentle under a pad.
                //
                // An arch sits above its own centre line: the mean of 1 - 2t^2
                // over a whole cycle is 1/3, not 0. Taking that out leaves the
                // range -4/3..2/3, so the 3/4 puts the peak back at 1. The result
                // is lopsided, and that is what a parabolic wave is -- the arch
                // is not a sine and does not pretend to be.
                const double t = 2.0 * p - 1.0;
                return (float) ((1.0 - 2.0 * t * t - (1.0 / 3.0)) * 0.75);
            }

            case SineCubed:
            {
                // A sine pinched towards zero: quieter in the middle of each
                // half, so it is softer than a sine but not duller.
                const double s = std::sin (twoPi * p);
                return (float) (s * s * s);
            }

            case HyperTriangle:
            {
                // A triangle pushed towards a square without ever getting a
                // corner. Sits between the two, and stays smooth.
                return (float) detail::signedPow (detail::rawTriangle (p), 0.6);
            }

            default:
                return (float) std::sin (twoPi * p);
        }
    }

    /** The same shape from an angle in radians, which is how the oscillators
        carry their phase. */
    inline float shapeValueFromAngle (int shape, double angle) noexcept
    {
        return shapeValue (shape, angle / twoPi);
    }
}

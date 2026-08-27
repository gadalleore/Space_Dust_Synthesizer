#pragma once

#include <cmath>

/**
    Unison -- several detuned copies of one oscillator, spread across the stereo
    field.

    WHAT IS HERE AND WHAT IS NOT

    This works out, for a given number of copies and a given Detune and Width,
    what SPEED each copy runs at and where each one sits. It generates nothing and
    knows nothing about phase, waveforms or samples: the voice takes these numbers
    and runs its existing oscillator once per copy.

    That split is the point. The arithmetic below -- the spread, the cent-to-ratio
    conversion, the pan law, the level compensation -- is all checkable against
    numbers decided in advance, without a synth, a note or a sound card. See
    tools/unisontest.

    WHY THE COPIES ARE SUMMED BEFORE THE OVERSAMPLER

    The voice runs its oscillators through an oversampling stage to keep their
    fold-back down. Seven copies each through their own stage would be seven times
    the filter work, which is the expensive part. Seven copies summed INSIDE one
    stage is seven times the cheap part -- reading a waveform -- and one filter.
    The voice is written that way; this header simply makes it possible by handing
    over every copy's numbers at once.

    LEVEL

    How loud N copies are depends on how far apart they are, and it moves between
    two extremes. Copies at the same pitch are the same waveform at the same phase,
    so they add: N copies are N times one. Copies far enough apart drift in and out
    of phase, sum incoherently, and land near the square root of N. Dividing by the
    right one of those keeps turning Voices up from changing how loud the patch is,
    which is what makes the control usable -- otherwise every turn of it has to be
    answered with the level knob.

    Assuming the incoherent case everywhere, as this first did, is wrong at the
    quiet end of Detune, where the copies have not separated at all.
*/
namespace Unison
{
    /** The most copies one oscillator may run.

        Seven, and an odd number on purpose: an odd count has a copy exactly in
        the middle, at the note's own pitch and dead centre, so the played note is
        still audibly the note. An even count leaves the centre empty and the
        pitch reads as slightly vague. */
    inline constexpr int maxVoices = 7;

    /** How far the outermost copies are detuned at Detune 1.0, in cents.

        Fifty cents is a quarter tone each way. Past that the copies stop reading
        as one thick note and start reading as a chord, which is a different
        effect and not the one this control is for. */
    inline constexpr double maxDetuneCents = 50.0;

    /** One copy: how fast it runs relative to the note, and where it sits. */
    struct Copy
    {
        /** Multiplier on the oscillator's phase increment. Exactly 1.0 means the
            note as played. */
        double ratio = 1.0;

        float gainLeft = 1.0f;
        float gainRight = 1.0f;
    };

    /** Where copy `index` of `voices` sits, from -1 (first) to +1 (last).

        A single copy sits at 0 -- dead centre, no detune -- which is what makes
        Voices 1 the untouched oscillator rather than a special case anyone has to
        remember. */
    inline double spreadOf (int index, int voices) noexcept
    {
        if (voices <= 1)
            return 0.0;

        return 2.0 * ((double) index / (double) (voices - 1)) - 1.0;
    }

    /**
        Work out every copy, and return the gain the sum should be scaled by.

        `out` must have room for `voices` entries. `detune01` and `width01` are
        both 0..1 and both do nothing at 0, so a patch that turns Voices up and
        nothing else gets copies that are exactly on top of each other -- louder,
        and nothing more, until Detune is turned.
    */
    inline float layout (int voices, double detune01, double width01, Copy* out) noexcept
    {
        if (voices < 1) voices = 1;
        if (voices > maxVoices) voices = maxVoices;

        const double detune = detune01 < 0.0 ? 0.0 : (detune01 > 1.0 ? 1.0 : detune01);
        const double width = width01 < 0.0 ? 0.0 : (width01 > 1.0 ? 1.0 : width01);

        for (int i = 0; i < voices; ++i)
        {
            const double spread = spreadOf (i, voices);

            // Cents to a speed multiplier. Twelve hundred cents is an octave, and
            // an octave is twice the speed.
            const double cents = spread * maxDetuneCents * detune;
            out[i].ratio = std::pow (2.0, cents / 1200.0);

            // Equal power, so a copy pushed to one side is no louder than one in
            // the middle. The pan runs from the copy's own position scaled by
            // Width, so Width 0 puts every copy dead centre and the unison is
            // mono -- thick, but not wide.
            const double pan = spread * width;                 // -1 .. +1
            const double angle = (pan + 1.0) * 0.25 * 3.14159265358979323846;

            // Scaled so a CENTRED copy comes out at 1.0 a side, not at 0.707.
            //
            // The plain oscillator has no pan law applied to it at all. Leaving
            // the textbook 0.707 in meant that merely switching unison on -- even
            // at Width 0, where every copy is centred and nothing has moved --
            // cost 3.01 dB against the oscillator it replaced. That is a constant
            // offset, it is inaudible as anything but "unison is quieter", and
            // the unit test could not see it because its own reference went
            // through the same 0.707 (Giuseppe, 2026-08-26).
            //
            // Equal power still holds: this scales both sides by the same number,
            // so every copy has the same total power wherever it sits.
            constexpr double centreNormalise = 1.4142135623730951;  // sqrt(2)

            out[i].gainLeft = (float) (centreNormalise * std::cos (angle));
            out[i].gainRight = (float) (centreNormalise * std::sin (angle));
        }

        // How much of the copies' sum is coherent -- and so what the sum has to
        // be divided by to leave the level where it was.
        //
        // At zero detune every copy is the same waveform at the same phase. They
        // add up: N copies are N times one, and the divisor is N. Open the detune
        // and they drift apart, sum incoherently, and land near the square root of
        // N instead -- so the divisor is the square root.
        //
        // Which of the two it is depends on how far apart the copies are. A cent
        // or two and they stay locked together for seconds, which is longer than
        // most notes last; ten cents and they have separated well inside one note.
        // So the exponent runs from 1 down to 1/2 as the spread opens, and the
        // level holds still across the whole of Detune rather than only at the top
        // of it (Giuseppe, 2026-08-26: "as I introduce unison the synth gets
        // quieter, which is the opposite of what I would expect").
        const double spreadCents = maxDetuneCents * detune;
        const double coherence = std::exp (-spreadCents * 0.5);
        const double exponent = 0.5 + 0.5 * coherence;

        return (float) (1.0 / std::pow ((double) voices, exponent));
    }
}

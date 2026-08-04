#pragma once

/**
    Note Lock -- quantises a filter cutoff to musically meaningful frequencies.

    Key Tracking is what makes this work with one grid instead of one per note.
    SynthVoice offsets the cutoff by (note - 60) semitones in log-frequency space
    (see the masterKeyTrackOffset maths in SynthVoice::renderNextBlock), so the
    cutoff parameter is a RATIO to middle C, not an absolute frequency:

        cutoff heard = cutoffParam * 2^((note - 60) / 12)

    Put cutoffParam exactly n steps above middle C and the cutoff therefore sits
    exactly n steps above whatever note is played, for every note. So quantising
    the knob to a grid is the entire feature: nothing per-voice, no DSP change,
    no audio-thread work.

    Deliberately free of any JUCE dependency. This is the one part of the feature
    that is pure arithmetic with a right answer, so it lives on its own where it
    can be tested directly rather than only through the UI -- see
    tools/notelocktest/notelock_test.cpp.
*/
namespace NoteLock
{
    /** Hz of MIDI note 60 -- the note Key Tracking is neutral at. */
    inline constexpr double referenceHz = 261.62556530059868;

    /** Which set of frequencies the cutoff is allowed to land on.

        Semitones is 12-TET: the cutoff sits a whole number of half steps above the
        played note. Even spacing, 120 detents over the cutoff's ten octaves.

        Harmonics is the note's actual overtone series -- k * root going up, root / k
        going down. These are the partials the oscillators genuinely produce, and they
        are NOT the same as the semitone grid: the 3rd partial is 2 cents sharp of a
        fifth, the 5th is 14 cents flat of a major third, the 7th is 31 cents flat.
        Parking a resonant peak on a real partial is what makes it ring.

        Their distribution is deliberately lopsided, because the harmonic series is:
        one detent between the root and its octave, but 32 in the octave from the 32nd
        to the 64th partial. Total detent count works out close to Semitones (88 vs
        120), just bunched towards the top. */
    enum class Grid { Semitones, Harmonics };

    /** Nearest grid frequency to freqHz, kept inside [minHz, maxHz].

        "Nearest" is judged in LOG frequency: 20 Hz of error is an enormous interval
        at the bottom of the range and inaudible at the top. Returns a clamped freqHz
        unchanged if the arguments are nonsense (non-positive frequency, inverted
        range) rather than inventing a detent. */
    double snapHz(double freqHz, double minHz, double maxHz, Grid grid);
}

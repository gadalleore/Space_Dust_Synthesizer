#pragma once

#include <cstddef>
#include <vector>

/**
    Wave analysis -- turns an arbitrary audio sample into an oscillator.

    Two things have to happen before a dropped file can be played as a waveform,
    and both are pure arithmetic with a right answer, so they live here where a
    test can drive them directly rather than only through the UI. See
    tools/wavetabletest/wavetable_test.cpp.

    1. FIND THE PITCH. The file is at whatever pitch it was recorded at. To play
       it in tune with the song we must know that pitch, so the note the user
       plays can be scaled against it. detectFundamental() does this.

    2. TURN IT INTO A CYCLE. The synth's oscillators are phase driven: they hand
       generateWaveform() an angle in [0, 2*pi) and expect one sample back. So a
       waveform here is one period of a repeating shape, sampled into a table.

    Step 2 is done in the FREQUENCY domain rather than by cutting a period out of
    the file with scissors, and that choice buys three things at once:

      - The cycle is periodic by construction. A time-domain cut is one period
        long only to the nearest sample, so its two ends do not meet and the
        oscillator clicks once per cycle -- a buzz at the note frequency that no
        amount of fading hides.
      - Band limiting is free. Playing a table faster than it was built for folds
        every harmonic above Nyquist back down as an out-of-tune whistle. Holding
        the HARMONICS instead of the samples means each band-limited version is
        just a shorter sum, not a filter design.
      - It is the spectral analysis the pitch detector wants anyway.

    So the pipeline is: file -> fundamental -> harmonic amplitudes and phases ->
    a ladder of tables, each built from fewer harmonics than the last.
*/
namespace WaveAnalysis
{
    /** Hz of MIDI note 60. A detected pitch is reported against this, and the
        full-sample mode plays a file at its recorded pitch when the user presses
        this note. */
    inline constexpr double middleCHz = 261.62556530059868;

    /** Samples per table. One period is stored at this resolution regardless of
        the source pitch, so a table is always cheap to read and always wraps. */
    inline constexpr int tableSize = 2048;

    /** How many band-limited versions of a waveform are kept.

        Level 0 holds up to tableSize/2 harmonics and each level after it holds
        half as many, so level 10 is a lone sine. Ten octaves of playback range
        is more than the 20 Hz - 20 kHz a note can occupy, so the top of the
        keyboard is covered even for a table whose fundamental is dragged down
        several octaves by the tuning knobs. */
    inline constexpr int mipLevels = 11;

    /** Harmonics kept at a given level. Level 0 is the full set. */
    inline constexpr int harmonicsAtLevel (int level)
    {
        return (tableSize / 2) >> (level < 0 ? 0 : (level > mipLevels - 1 ? mipLevels - 1 : level));
    }

    //==========================================================================
    /** What detectFundamental() concluded about a piece of audio. */
    struct PitchEstimate
    {
        /** The fundamental, in Hz. Never zero or negative: when nothing pitched
            is found this falls back to middleCHz so callers always have a usable
            number, and confidence says not to trust it. */
        double frequencyHz = middleCHz;

        /** 0 to 1. Above about 0.8 the reading is solid; below about 0.5 the
            source is probably percussive or noisy and has no single pitch.
            Callers use this to decide whether to re-tune the sample at all --
            re-tuning a snare drum to concert pitch is meaningless, and doing it
            anyway would leave the user wondering why their drum plays back
            slower than the file. */
        double confidence = 0.0;

        /** Whether the spectral pass had to overrule the time-domain pass on the
            octave. Reported only so the tests can see it happen. */
        bool octaveCorrected = false;
    };

    /** The fundamental of a mono signal, and how much to believe it.

        Two independent methods run, because each fails in a way the other does
        not. A time-domain difference function (YIN) is accurate to a fraction of
        a sample but sometimes locks to twice or half the true period, since a
        wave that repeats every period also repeats every two. A harmonic product
        spectrum is coarse but nearly immune to that, because it multiplies the
        spectrum against decimated copies of itself and only the true fundamental
        lines all of them up. So YIN gives the value and the spectrum vetoes the
        octave.

        Reads a window from the loudest steady part of the signal, not the start:
        the attack of a note is the least periodic part of it.

        sampleRate must be positive and numSamples at least a few hundred, or the
        fallback estimate is returned untouched. */
    PitchEstimate detectFundamental (const float* samples, int numSamples, double sampleRate);

    //==========================================================================
    /** One cycle of a waveform, held as the harmonics that make it up.

        Kept in this form rather than as samples because every band-limited table
        is a prefix of it: to remove everything above the Nth harmonic, stop the
        sum at N. */
    struct HarmonicSet
    {
        /** Peak amplitude of harmonic h, at index h-1. The DC term is discarded
            on purpose: a table with an offset thumps on every note. */
        std::vector<float> amplitude;

        /** Phase of harmonic h in radians, at index h-1. Kept because discarding
            it (as a magnitude-only analysis would) rebuilds every waveform as a
            cosine pile, which sounds hollow and looks nothing like the source. */
        std::vector<float> phase;

        int size() const { return (int) amplitude.size(); }
        bool isEmpty() const { return amplitude.empty(); }
    };

    /** Measure the harmonics of samples at the given fundamental.

        Evaluates the spectrum only at whole multiples of fundamentalHz instead of
        on an FFT grid, so no harmonic falls between bins and needs interpolating
        -- the analysis window is sized to a whole number of periods and each
        harmonic is read where it actually sits.

        Stops at whichever comes first: tableSize/2 harmonics, or the last one
        below Nyquist. Returns an empty set if the arguments make no sense. */
    HarmonicSet analyseHarmonics (const float* samples, int numSamples, double sampleRate,
                                  double fundamentalHz);

    /** Render one period into out[0 .. tableSize-1], using at most maxHarmonics.

        The result is periodic to the last bit: sample tableSize is sample 0, so
        the oscillator's phase wrap is silent. Peak is normalised to 1 unless the
        set is empty or silent, in which case the table is zeroed. */
    void renderTable (const HarmonicSet& harmonics, int maxHarmonics, float* out);

    /** Render the whole mipLevels ladder, most harmonics first.

        out must have room for mipLevels * tableSize floats. Level L starts at
        out + L * tableSize.

        All levels are scaled by ONE factor, taken from level 0. Normalising each
        level on its own would make a note get louder as it climbed into the next
        level, because dropping harmonics lowers the peak. */
    void renderMipmaps (const HarmonicSet& harmonics, float* out);

    /** Which level to read when a table is played at freqHz.

        Picks the richest level whose top harmonic still lands below Nyquist, so
        nothing folds back. Returns 0 for nonsense arguments -- a caller with no
        sample rate yet is better served by the full-bandwidth table than by
        silence.

        Called once per sample per oscillator on the audio thread, so it answers
        by reading the exponent of the frequency ratio rather than by searching
        the ladder. */
    int levelForFrequency (double freqHz, double sampleRate);

    /** Recover the harmonics of a table produced by renderTable().

        Exact rather than approximate: a table is a whole number of periods long
        by construction, so every harmonic sits on its own bin and a transform
        reads them back without leakage.

        This is what lets a saved waveform be stored as one table -- eight
        kilobytes -- instead of the whole eleven-level ladder, and rebuilt on
        load. table must be tableSize samples long. */
    HarmonicSet harmonicsFromTable (const float* table);

    //==========================================================================
    /** What to multiply an oscillator's phase increment by to play a whole sample.

        THE PROBLEM. The oscillator's phase increment means "one turn per period
        of the played note". To play a whole file instead, one turn has to cover
        the whole file -- and at a speed that puts the file's recorded pitch on
        the note the player pressed.

        THE RESULT. Those two requirements produce a single constant per sample,
        because the note frequency cancels out of them:

            samples consumed per output sample = (note / root) * (fileRate / hostRate)
            one turn must cover                = loopLength samples
            so increment = 2*pi * (note/root) * fileRate / (hostRate * loopLength)
            and the plain increment is 2*pi * note / hostRate
            leaving the ratio          fileRate / (root * loopLength)

        Because the note cancels, everything that drives the note -- coarse tune,
        detune, glide, pitch bend, LFO -- keeps working with nothing added. And a
        note of middle C plays the file at exactly the speed that puts its
        recorded pitch on middle C, which is the whole point.

        rootHz is the pitch the file was recorded at, or middle C when no pitch
        could be found -- which makes middle C play it at its original speed.

        Returns 1 for nonsense arguments, which is the harmless no-op value. */
    double playbackPhaseScale (double fileSampleRate, double rootHz, int loopLength);

    //==========================================================================
    /** Semitones from middle C to freqHz, positive for higher. Fractional: .5 is
        a quarter tone. Returns 0 for a non-positive frequency. */
    double semitonesFromMiddleC (double freqHz);

    /** Fill name with the nearest note to freqHz and the cents error, as
        "F#3 +12c". capacity includes the terminator. Writes an empty string if
        freqHz is not positive. A2 = 110 Hz, so middle C is C4. */
    void describePitch (double freqHz, char* name, std::size_t capacity);

    //==========================================================================
    /** In-place radix-2 FFT of a power-of-two block. real and imag are both
        numSamples long. Exposed because the tests check it against a direct
        transform; nothing outside this file needs it. */
    void fft (float* real, float* imag, int numSamples, bool inverse);
}

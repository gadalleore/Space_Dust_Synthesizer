#pragma once

/**
    LFO waveform generation.

    Split out of PluginProcessor so it can be measured directly -- see
    tools/lfotest/lfo_test.cpp, which renders each shape and reports how much energy
    lands away from the fundamental.

    The shapes deliberately ease their discontinuities rather than stepping, which is
    what keeps them click-free. The catch is that the easing was expressed purely as a
    FRACTION OF A CYCLE (8% for the saws, 3% for the square). That is generous at 1 Hz
    -- 30 ms for the square -- but at 2 kHz the same 3% is 15 us, well under one sample
    at 48 kHz, so the edge becomes an instantaneous step and folds back all over the
    spectrum. The audible pitch is then not the rate that was asked for, which defeats
    the whole point of locking that rate to a note.

    So the easing is now the WIDER of that fraction and a small number of samples. At
    LFO rates the fraction always wins and the shapes are bit-identical to before; only
    once a cycle gets short enough for the fraction to fall below a couple of samples
    does the sample floor take over and band-limit the edge.
*/
namespace LfoWaveform
{
    enum Shape
    {
        Sine       = 0,
        Triangle   = 1,
        SawUp      = 2,
        SawDown    = 3,
        Square     = 4,
        SampleHold = 5
    };

    /** Minimum edge width, in samples.

        Chosen by measurement, not taste -- see tools/lfotest, which sweeps it. It has
        to be wide enough that the edge is genuinely band-limited at audio rates, and
        the honest consequence is that a square at 2 kHz (only 24 samples per cycle)
        softens towards a sine. That is not a compromise so much as the truth: at that
        rate only a handful of harmonics fit under Nyquist, so a hard square cannot
        exist without folding back. */
    inline constexpr double minEdgeSamples = 4.0;

    /** One sample of the shape.

        @param phase     cycle position; wrapped internally, may be outside [0,1)
        @param waveform  a Shape
        @param dt        phase advance per sample (frequency / sample rate). Pass 0 to
                         get the original fraction-only easing, e.g. for offline use.
    */
    float generate(double phase, int waveform, double dt);

    /** As above with an explicit edge floor, so the test can sweep it. */
    float generate(double phase, int waveform, double dt, double edgeFloorSamples);
}

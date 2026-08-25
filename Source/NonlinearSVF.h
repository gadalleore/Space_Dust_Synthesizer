#pragma once

#include <juce_dsp/juce_dsp.h>
#include <cmath>

//==============================================================================
/**
    Self-oscillating state-variable filter (TPT / zero-delay-feedback) with an
    amplitude-regulated oscillator.

    Drop-in replacement for juce::dsp::StateVariableTPTFilter as used by the synth
    voice. Two regimes:

      * Resonance knob 0.0 .. kSelfOscKnobStart  -> a damped (non-self-oscillating)
        SVF. Resonance maps to Q via an EXPONENTIAL taper (kQMin..kQMax) so the knob
        ramps up in even perceptual steps and the peak is pronounced near the top of
        this range. (Earlier this was a linear 0.1..16 map identical to
        juce::dsp::StateVariableTPTFilter; the exponential taper changes the timbre
        across the whole resonance range.)

      * Resonance knob kSelfOscKnobStart .. 1.0   -> the filter self-oscillates.
        Instead of a fixed negative damping clamped by a waveshaper (which always
        leaves the sine distorted), an automatic-gain loop holds the effective
        damping right at the lossless point (R2 ~ 0), where the SVF is a pure sine
        oscillator. A slow amplitude follower nudges the damping to keep the
        oscillation at a TARGET amplitude. Because the loop keeps R2 ~ 0, the
        waveform stays a clean sine; the target amplitude alone decides character:

            target  > 1  -> the clean sine overshoots the +/-1 output clip and is
                            squared off  -> aggressive, gritty scream.
            target <= 1  -> the clean sine passes the clip untouched -> pure tone.

        targetAmp RISES with the knob: a gentle near-clean sine just past the onset,
        building to the full gritty (clipped) scream at maximum resonance — so the
        intensity increases monotonically as you turn the knob up. The self-oscillation
        also fades in across an onset band (kSelfOscKnobStart..kSelfOscFull) rather than
        switching on abruptly. The target scales with the amplitude envelope so a note's
        release fades the singing filter out smoothly.

    The self-oscillation frequency is the cutoff, so with key tracking on it sings
    at the played note's pitch.
*/
class NonlinearSVF
{
public:
    /** Output taps. lowpass/bandpass/highpass are the raw SVF outputs; notch and
        peak are the standard linear combinations of them:

          notch = LP + HP  -> flat, with a full null at the cutoff. Resonance sets
                              the WIDTH of the notch, not its depth (the null is
                              total at any Q).
          peak  = LP - HP  -> flat (0 dB at DC and Nyquist) with a bell at the
                              cutoff whose gain is 2Q. Note that Q < 0.5 (the
                              bottom of the resonance knob) makes that bell a
                              shallow DIP; the peak only appears once resonance is
                              turned up. */
    /**
        The five original taps, then eight more built on the same core.

        ORDER IS A PARAMETER CONTRACT. These indices are stored in presets and in
        host automation lanes, so the first five keep their places for good and
        anything new goes on the END.

        The second group needs a SECOND pass of the same filter -- a 24 dB slope
        is two 12 dB stages, and a wider notch is two notches. That pass has its
        own integrator states (s3, s4) and is skipped entirely for the modes that
        do not use it, so a plain Low Pass costs exactly what it always did.

        The last three are the dirty ones. Their character is distortion in or
        around the resonance path rather than a different slope, which is why they
        are named for what they sound like instead of for their shape. Massive has
        filters of similar character called Scream, Daft and Acid; these are ours,
        built here, and named ours. */
    enum Mode
    {
        lowpass = 0,
        bandpass = 1,
        highpass = 2,
        notch = 3,
        peak = 4,

        // -- Two-stage: the same core run twice --
        lowpass4 = 5,      // 24 dB/oct low pass
        highpass4 = 6,     // 24 dB/oct high pass
        allpass = 7,       // flat, all phase
        doubleNotch = 8,   // two notches, spread apart
        bandReject = 9,    // one notch, twice as deep and wider

        // -- The dirty three --
        howl = 10,         // low pass whose resonance saturates and screams
        grit = 11,         // 24 dB low pass with distortion between the stages
        punch = 12,        // low pass driven hard and clipped: blunt and loud

        numModes = 13
    };

    /** The names, in index order, for the dropdown and for the parameter. */
    static const char* const* modeNames() noexcept
    {
        static const char* const names[] =
        {
            "Low Pass", "Band Pass", "High Pass", "Notch", "Peak",
            "Low Pass 4", "High Pass 4", "All Pass", "Double Notch", "Band Reject",
            "Howl", "Grit", "Punch"
        };

        return names;
    }

    void prepare (const juce::dsp::ProcessSpec& spec) noexcept
    {
        baseSampleRate = spec.sampleRate > 0.0 ? static_cast<float> (spec.sampleRate) : 44100.0f;
        sampleRate     = baseSampleRate * static_cast<float> (rateScale);
        envReleaseEff  = kEnvRelease / static_cast<float> (rateScale);
        reset();
        updateG();
    }

    /** Run the filter maths at an integer multiple of the host rate. The caller
        (OversampledStage) feeds `rateScale` sub-samples per host sample, so `g`
        must be computed at the oversampled rate. scale == 1 restores host rate
        and is bit-identical to the un-oversampled filter. */
    void setSampleRateScale (int scale) noexcept
    {
        rateScale = juce::jmax (1, scale);
        sampleRate = baseSampleRate * static_cast<float> (rateScale);
        // The amplitude follower's release is a per-sample time constant; at Nx the
        // rate it would otherwise release Nx faster. Divide so the self-oscillation
        // build-up/decay stays the same in real time as at host rate. (Damping/g
        // already scale with rate, so the oscillation speed itself is invariant.)
        envReleaseEff = kEnvRelease / static_cast<float> (rateScale);
        updateG();
    }

    void reset() noexcept
    {
        s1[0] = s1[1] = s2[0] = s2[1] = 0.0f;
        s3[0] = s3[1] = s4[0] = s4[1] = 0.0f;
        oscEnv[0] = oscEnv[1] = 0.0f;
    }

    void setMode (int newMode) noexcept { mode = juce::jlimit (0, numModes - 1, newMode); }
    int  getMode() const noexcept       { return mode; }

    void setCutoffFrequency (float freqHz) noexcept
    {
        cutoff = juce::jlimit (20.0f, 20000.0f, freqHz);
        updateG();
    }

    /** Pass the raw normalized knob value (0..1). The class owns the resonance
        curve, including the self-oscillation region at the top of the range. */
    void setResonanceNormalized (float res) noexcept
    {
        res = juce::jlimit (0.0f, 1.0f, res);

        // Kept as the knob position, not just as damping. Howl, Grit and Punch
        // read it directly: how hard they distort is the Resonance knob, because
        // on those three the dirt IS the resonance and one control for both is
        // what makes them playable.
        resonanceKnob = res;

        // Resonance -> damping (R2 = 1/Q). EXPONENTIAL Q taper (kQMin..kQMax across the
        // damped range) so resonance ramps up in equal PERCEPTUAL steps and reaches a
        // pronounced peak just before self-oscillation. The old linear 0.1..16 map
        // bunched its change low and felt flat across the upper damped sweep. ALWAYS
        // computed: below the self-osc region it IS the damping; inside the region it is
        // the blend anchor at the onset so there is no jump at the start.
        const float qNorm = juce::jlimit (0.0f, 1.0f, res / kSelfOscKnobStart);
        const float Q     = kQMin * std::pow (kQMax / kQMin, qNorm);
        R2 = 1.0f / Q;
        if (res <= kSelfOscKnobStart)
        {
            selfOsc      = false;
            selfOscBlend = 0.0f;
        }
        else
        {
            selfOsc   = true;
            targetAmp = computeTargetAmp (res);
            // Fade the self-oscillation in across [kSelfOscKnobStart .. kSelfOscFull]
            // instead of slamming it on at the first step past the threshold. The blend
            // (used per-sample below) weights the AGC's negative damping against the
            // legacy positive damping: 0 at the onset (== legacy R2, fully continuous)
            // rising to 1 (pure self-oscillation) at kSelfOscFull. This turns the old
            // hard 0.80->0.81 character jump into a smooth ramp into the scream.
            float t = (res - kSelfOscKnobStart) / (kSelfOscFull - kSelfOscKnobStart);
            t = juce::jlimit (0.0f, 1.0f, t);
            selfOscBlend = t * t * (3.0f - 2.0f * t); // smoothstep (ease-in/out)
        }
    }

    /** Set the damping directly from a Q value, bypassing the knob taper and the
        self-oscillation region entirely. For callers that own their own resonance
        curve (e.g. the master-chain mirror filters) and only want the plain SVF. */
    void setResonanceQ (float Q) noexcept
    {
        R2 = 1.0f / juce::jmax (0.025f, Q);
        selfOsc      = false;
        selfOscBlend = 0.0f;

        // A caller that owns its own resonance curve wants the plain filter, so
        // the dirty modes are given nothing to drive with.
        resonanceKnob = 0.0f;
    }

    /** Amplitude envelope (0..1) for the current sample. Scales the self-osc target
        so release fades the singing filter instead of ringing at full level. */
    void setEnvelope (float env) noexcept { envAmount = juce::jlimit (0.0f, 1.0f, env); }

    /** True while the filter's integrator state still holds audible energy — i.e.
        a resonant ring. Covers BOTH self-oscillation (top of the knob) and a plain
        high-Q resonant ring (Q up to ~16), since both keep producing output that is
        NOT enveloped to zero. The voice uses this to fade out cleanly at note-end
        instead of hard-cutting a ringing resonator (which clicks; worst on low
        notes, where the residual sine sits far from a zero crossing), and to reset
        the filter on a mono retrigger so the old ring doesn't bleed into the new
        note's attack. A settled / low-resonance filter decays below kRingingFloor,
        so it reports false and the original (bit-identical) paths run. */
    bool isRinging() const noexcept
    {
        const float e = juce::jmax (juce::jmax (std::abs (s1[0]), std::abs (s1[1])),
                                    juce::jmax (std::abs (s2[0]), std::abs (s2[1])));
        return e > kRingingFloor;
    }

    float processSample (int channel, float x) noexcept
    {
        auto& ls1 = s1[(size_t) channel];
        auto& ls2 = s2[(size_t) channel];

        float effR2 = R2;
        if (selfOsc)
        {
            // AGC: drive net damping from the amplitude error. Below target -> negative
            // damping (grow); above target -> positive damping (decay). At target the
            // damping sits at ~0, i.e. a lossless (pure-sine) resonator.
            const float target = targetAmp * envAmount;
            const float agcR2 = juce::jlimit (kMaxGrowth, kMaxDecay, (oscEnv[(size_t) channel] - target) * kAgcStrength);
            // Blend legacy damping (R2) -> AGC damping across the onset band so the
            // self-oscillation ramps in instead of stepping on. At selfOscBlend==0 this
            // is exactly R2 (continuous with the sub-threshold region); at ==1 it is the
            // pure AGC value (unchanged top-of-knob scream). The blend also makes the
            // settled oscillation amplitude rise smoothly from 0 to target across the band.
            effR2 = R2 + selfOscBlend * (agcR2 - R2);
        }

        // Guard the denominator: a negative R2 must never make it ill-conditioned.
        const float denom = juce::jmax (1.0f + effR2 * g + g * g, 1.0e-4f);
        const float h = 1.0f / denom;

        const float yHP = h * (x - ls1 * (g + effR2) - ls2);
        const float yBP = yHP * g + ls1;
        ls1 = yHP * g + yBP;
        const float yLP = yBP * g + ls2;
        ls2 = yBP * g + yLP;

        if (selfOsc)
        {
            // Peak-follow the resonant (bandpass) amplitude for the AGC: instant
            // attack, slow release so it measures the oscillation envelope, not the
            // waveform. Then a hard safety clamp in case the loop is ever outrun.
            auto& env = oscEnv[(size_t) channel];
            const float a = std::abs (yBP);
            env = (a > env) ? a : env + envReleaseEff * (a - env);
            ls1 = juce::jlimit (-kSafetyClamp, kSafetyClamp, ls1);
            ls2 = juce::jlimit (-kSafetyClamp, kSafetyClamp, ls2);
        }

        // notch/peak are exact sums of the taps above, so they inherit the same
        // cutoff/Q and the same self-oscillation behaviour. In the self-osc region
        // the LP and HP components of the oscillation are equal and opposite, so
        // notch cancels the singing (it passes the input) while peak doubles it.
        float out;
        switch (mode)
        {
            case bandpass:  out = yBP;         break;
            case highpass:  out = yHP;         break;
            case notch:     out = yLP + yHP;   break; // notch
            case peak:      out = yLP - yHP;   break; // peak (flat + resonant bell)

            //-- The modes that run the core a second time ---------------------
            // Everything below feeds one of the taps above back through the same
            // maths with its own states. Second-stage damping is deliberately
            // NOT the AGC value: two self-oscillating stages in series fight each
            // other and the result howls out of control rather than singing. The
            // second stage always uses the plain R2.

            case lowpass4:
                out = secondStage (channel, yLP, R2, g).lp;
                break;

            case highpass4:
                out = secondStage (channel, yHP, R2, g).hp;
                break;

            case allpass:
                // The standard TPT all-pass: the input with twice the damped
                // resonance taken out. Flat in level, and everything it does is
                // to the phase -- which is what makes it useful in a chain rather
                // than on its own.
                out = x - 2.0f * effR2 * yBP;
                break;

            case doubleNotch:
            {
                // Two notches at different frequencies, so a whole band is
                // hollowed out instead of one exact pitch. The second sits an
                // octave and a half up -- far enough apart to hear as two.
                const float first = yLP + yHP;
                const auto second = secondStage (channel, first, R2, juce::jmin (g * 3.0f, 12.0f));
                out = second.lp + second.hp;
                break;
            }

            case bandReject:
            {
                // One notch through a second at the SAME frequency: twice as deep
                // and twice as wide as the plain Notch, which is the difference
                // between taking a pitch out and taking a band out.
                const float first = yLP + yHP;
                const auto second = secondStage (channel, first, R2, g);
                out = second.lp + second.hp;
                break;
            }

            //-- The dirty three ------------------------------------------------
            case howl:
            {
                // A low pass whose resonance is driven until it screams. The
                // bandpass tap IS the resonance, so saturating it and adding it
                // back is the resonance shouting rather than the whole signal
                // distorting -- the body of the note stays clean and the peak
                // tears.
                const float resonanceDrive = 1.0f + 8.0f * resonanceKnob;
                out = yLP + std::tanh (yBP * resonanceDrive) * resonanceKnob;
                break;
            }

            case grit:
            {
                // 24 dB of slope with the signal bent between the two stages.
                // Distorting in the MIDDLE means the second stage filters the
                // harmonics the distortion just made, so it grinds without
                // getting fizzy on top.
                const float bent = std::tanh (yLP * (1.0f + 6.0f * resonanceKnob));
                out = secondStage (channel, bent, R2, g).lp;
                break;
            }

            case punch:
            {
                // Driven hard into a hard clip rather than a soft one. No curve,
                // no rounding: it is loud, blunt, and the same every time, which
                // is the point of it.
                const float driven = yLP * (1.5f + 3.0f * resonanceKnob);
                out = juce::jlimit (-1.0f, 1.0f, driven);
                break;
            }

            default: out = yLP;         break;
        }
        if (! std::isfinite (out))
        {
            out = 0.0f;
            reset();
        }
        return out;
    }

private:
    /** The three taps of one pass of the filter. */
    struct Taps
    {
        float lp = 0.0f;
        float bp = 0.0f;
        float hp = 0.0f;
    };

    /**
        Run the same TPT core a second time, on its own integrators.

        Its damping is passed in rather than taken from effR2, and every caller
        passes the plain R2. Two self-oscillating stages in series drive each
        other and the result runs away into a howl instead of singing at the
        cutoff -- so the AGC belongs to the first stage alone.

        Its g is passed in too, because Double Notch wants its second notch at a
        different frequency from its first.
    */
    Taps secondStage (int channel, float x, float damping, float stageG) noexcept
    {
        auto& ls3 = s3[(size_t) channel];
        auto& ls4 = s4[(size_t) channel];

        const float denom = juce::jmax (1.0f + damping * stageG + stageG * stageG, 1.0e-4f);
        const float h = 1.0f / denom;

        Taps t;
        t.hp = h * (x - ls3 * (stageG + damping) - ls4);
        t.bp = t.hp * stageG + ls3;
        ls3 = t.hp * stageG + t.bp;
        t.lp = t.bp * stageG + ls4;
        ls4 = t.bp * stageG + t.lp;

        // The first stage guards its own output; this one guards its states, so a
        // burst that got through cannot sit in the integrators for good.
        if (! std::isfinite (ls3) || ! std::isfinite (ls4))
        {
            ls3 = ls4 = 0.0f;
            t.lp = t.bp = t.hp = 0.0f;
        }

        return t;
    }

    static float computeTargetAmp (float res) noexcept
    {
        // Intensity RISES with the knob: a gentle, near-clean sine just past the onset
        // building to the full gritty scream at MAXIMUM resonance. (This used to be
        // inverted — gritty at the bottom of the self-osc region, cleaning to a quiet
        // sub-unity sine at the top — which made res 100 sound attenuated next to 81.)
        float u = (res - kSelfOscKnobStart) / (1.0f - kSelfOscKnobStart);
        u = juce::jlimit (0.0f, 1.0f, u);
        u = u * u * (3.0f - 2.0f * u);              // smoothstep (ease-in/out)
        return juce::jmap (u, kCleanAmp, kGritAmp); // 0.9 gentle sine -> 2.5 clipped scream
    }

    void updateG() noexcept
    {
        g = std::tan (juce::MathConstants<float>::pi * cutoff / sampleRate);
    }

    // Tunables ----------------------------------------------------------------
    static constexpr float kSelfOscKnobStart = 0.80f;  // self-oscillation BEGINS to fade in here
    static constexpr float kSelfOscFull      = 0.88f;  // ...and is fully engaged here (onset ramp band)
    static constexpr float kQMin             = 0.25f;  // Q at resonance 0 (clean, no resonance)
    static constexpr float kQMax             = 20.0f;  // Q at the self-osc onset (sharp, pronounced peak)
    static constexpr float kGritAmp          = 2.5f;   // self-osc target at MAX resonance (>1 => clipped scream)
    static constexpr float kCleanAmp         = 0.9f;   // self-osc target at the onset (<1 => gentle clean sine)
    static constexpr float kAgcStrength      = 0.2f;   // how hard the amplitude loop corrects damping
    static constexpr float kMaxGrowth        = -0.15f; // most negative damping (fastest oscillation build-up)
    static constexpr float kMaxDecay         = 0.5f;   // most positive damping (fastest decay / overshoot tame)
    static constexpr float kEnvRelease       = 0.0008f;// amplitude-follower release coefficient
    static constexpr float kSafetyClamp      = 8.0f;   // hard state clamp (AGC failsafe)
    static constexpr float kRingingFloor     = 1.0e-3f;// integrator energy below which isRinging() reports silent

    float baseSampleRate = 44100.0f; // host rate (set in prepare)
    int   rateScale      = 1;        // oversampling factor applied to the maths
    float envReleaseEff  = kEnvRelease; // rate-compensated follower release (= kEnvRelease / rateScale)
    float sampleRate = 44100.0f;     // effective rate = baseSampleRate * rateScale
    float cutoff     = 1000.0f;
    float R2         = 2.0f;     // damping = 1/Q (used outside the self-osc region)
    float g          = 0.0f;     // tan(pi * fc / fs)
    float targetAmp  = 0.9f;     // self-osc target amplitude (set from the knob)
    float selfOscBlend = 0.0f;   // 0..1 onset ramp: legacy damping -> AGC self-oscillation
    int   mode       = 0;
    bool  selfOsc    = false;
    float envAmount  = 1.0f;
    float s1[2] { 0.0f, 0.0f };  // integrator states (per channel)
    float s2[2] { 0.0f, 0.0f };

    /** The second stage's integrators, for the modes that need the core run
        twice -- the 24 dB slopes, the paired notches and the two dirty filters
        that are built on a 24 dB slope. Untouched, and not even read, by the five
        original modes. */
    float s3[2] { 0.0f, 0.0f };
    float s4[2] { 0.0f, 0.0f };

    /** The Resonance knob as the player set it, 0..1. Howl, Grit and Punch drive
        their distortion from this directly -- see setResonanceNormalized. */
    float resonanceKnob = 0.0f;
    float oscEnv[2] { 0.0f, 0.0f }; // amplitude follower (per channel)
};

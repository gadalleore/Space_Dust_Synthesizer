#pragma once

#include <juce_dsp/juce_dsp.h>
#include <cmath>

//==============================================================================
/**
    Self-oscillating state-variable filter (TPT / zero-delay-feedback) with an
    amplitude-regulated oscillator.

    Drop-in replacement for juce::dsp::StateVariableTPTFilter as used by the synth
    voice. Two regimes:

      * Resonance knob 0.0 .. kSelfOscKnobStart  -> mathematically IDENTICAL to
        juce::dsp::StateVariableTPTFilter (same h / R2 / g equations). Existing
        presets in this range sound exactly as before.

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

        targetAmp is held high (gritty) across most of the self-oscillation range
        and falls to just under unity at maximum resonance, so the very top of the
        knob produces a CLEAN sine wave. The target also scales with the amplitude
        envelope so a note's release fades the singing filter out smoothly.

    The self-oscillation frequency is the cutoff, so with key tracking on it sings
    at the played note's pitch.
*/
class NonlinearSVF
{
public:
    enum Mode { lowpass = 0, bandpass = 1, highpass = 2 };

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
        oscEnv[0] = oscEnv[1] = 0.0f;
    }

    void setMode (int newMode) noexcept { mode = juce::jlimit (0, 2, newMode); }
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
        if (res <= kSelfOscKnobStart)
        {
            // Legacy mapping Q = 0.1..~16, R2 = 1/Q. Identical to the previous filter.
            R2 = 1.0f / (0.1f + res * 19.9f);
            selfOsc = false;
        }
        else
        {
            selfOsc   = true;
            targetAmp = computeTargetAmp (res);
        }
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
            effR2 = juce::jlimit (kMaxGrowth, kMaxDecay, (oscEnv[(size_t) channel] - target) * kAgcStrength);
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

        float out = (mode == 0 ? yLP : (mode == 1 ? yBP : yHP));
        if (! std::isfinite (out))
        {
            out = 0.0f;
            reset();
        }
        return out;
    }

private:
    static float computeTargetAmp (float res) noexcept
    {
        // Hold a high (clipping = gritty) target across most of the self-osc range,
        // then smoothly clean up to just under unity at maximum resonance so the very
        // top of the knob is a pure, un-clipped sine.
        if (res < kCleanStart)
            return kGritAmp;
        float u = (res - kCleanStart) / (1.0f - kCleanStart); // 0..1
        u = u * u * (3.0f - 2.0f * u);                        // smoothstep
        return juce::jmap (u, kGritAmp, kCleanAmp);
    }

    void updateG() noexcept
    {
        g = std::tan (juce::MathConstants<float>::pi * cutoff / sampleRate);
    }

    // Tunables ----------------------------------------------------------------
    static constexpr float kSelfOscKnobStart = 0.80f;  // top 20% of the knob self-oscillates
    static constexpr float kCleanStart       = 0.94f;  // knob value where the scream cleans into a sine
    static constexpr float kGritAmp          = 2.5f;   // self-osc target amplitude in the gritty region (>1 => clips)
    static constexpr float kCleanAmp         = 0.9f;   // self-osc target at max resonance (<1 => clean sine)
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
    int   mode       = 0;
    bool  selfOsc    = false;
    float envAmount  = 1.0f;
    float s1[2] { 0.0f, 0.0f };  // integrator states (per channel)
    float s2[2] { 0.0f, 0.0f };
    float oscEnv[2] { 0.0f, 0.0f }; // amplitude follower (per channel)
};

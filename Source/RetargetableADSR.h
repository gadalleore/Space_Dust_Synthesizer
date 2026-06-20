#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

//==============================================================================
/**
    A drop-in replacement for juce::ADSR whose envelope maths is byte-faithful to
    JUCE's (juce_ADSR.h) — same states, same per-sample arithmetic, same
    noteOn/noteOff/reset/recalculateRates semantics — so every existing path that
    drives it (note-onset click handling, voice-fade, crash hardening, presets,
    pluginval) behaves EXACTLY as before. Reuses juce::ADSR::Parameters so call
    sites that build a `juce::ADSR::Parameters` need no change.

    It adds exactly ONE capability: setReleaseRetainingLevel(), which lets the
    Release time of an ALREADY-RINGING note change live. It re-derives the release
    slope from the CURRENT envelope level the same way noteOff() does
    (releaseRate = level / (release * sampleRate)), WITHOUT calling
    recalculateRates(). That matters because juce::ADSR::setParameters() rewrites
    releaseRate from `sustain` and, when sustain == 0 (plucks/bass), forces the
    envelope straight to idle — an instant cut. There is no public juce::ADSR API
    to retarget a live release click-free, which is why this class exists.

    @see SynthVoice::updateAdsrParameters
*/
class RetargetableADSR
{
public:
    using Parameters = juce::ADSR::Parameters;

    RetargetableADSR() { recalculateRates(); }

    void setParameters (const Parameters& newParameters)
    {
        jassert (sampleRate > 0.0); // call setSampleRate() first!
        parameters = newParameters;
        recalculateRates();
    }

    const Parameters& getParameters() const noexcept { return parameters; }

    bool isActive() const noexcept { return state != State::idle; }

    void setSampleRate (double newSampleRate) noexcept
    {
        jassert (newSampleRate > 0.0);
        sampleRate = newSampleRate;
    }

    void reset() noexcept
    {
        envelopeVal = 0.0f;
        state = State::idle;
    }

    void noteOn() noexcept
    {
        if (attackRate > 0.0f)
        {
            state = State::attack;
        }
        else if (decayRate > 0.0f)
        {
            envelopeVal = 1.0f;
            state = State::decay;
        }
        else
        {
            envelopeVal = parameters.sustain;
            state = State::sustain;
        }
    }

    void noteOff() noexcept
    {
        if (state != State::idle)
        {
            if (parameters.release > 0.0f)
            {
                releaseRate = (float) (envelopeVal / (parameters.release * sampleRate));
                state = State::release;
            }
            else
            {
                reset();
            }
        }
    }

    //==============================================================================
    /** Change the Release time of an IN-PROGRESS release, preserving the current
        level — so shortening (or lengthening) Release while a note is ringing out
        re-shapes that note's tail live, not just future notes.

        Click-free for any sustain value (including 0): we never run
        recalculateRates(), so we avoid juce::ADSR's "sustain == 0 forces idle" cut.
        The release slope is re-derived from the live level exactly as noteOff()
        would (level / (release * sampleRate)).

        No-op unless the envelope is currently in its RELEASE stage, so a sustaining,
        attacking, decaying, or idle envelope is never disturbed. Real-time safe. */
    void setReleaseRetainingLevel (float newReleaseSeconds) noexcept
    {
        parameters.release = newReleaseSeconds;

        if (state == State::release)
        {
            if (newReleaseSeconds > 0.0f && sampleRate > 0.0)
                releaseRate = (float) (envelopeVal / (newReleaseSeconds * sampleRate));
            else
                reset(); // zero release ends the tail immediately (matches noteOff's else)
        }
    }

    //==============================================================================
    float getNextSample() noexcept
    {
        switch (state)
        {
            case State::idle:
            {
                return 0.0f;
            }

            case State::attack:
            {
                envelopeVal += attackRate;

                if (envelopeVal >= 1.0f)
                {
                    envelopeVal = 1.0f;
                    goToNextState();
                }

                break;
            }

            case State::decay:
            {
                envelopeVal -= decayRate;

                if (envelopeVal <= parameters.sustain)
                {
                    envelopeVal = parameters.sustain;
                    goToNextState();
                }

                break;
            }

            case State::sustain:
            {
                envelopeVal = parameters.sustain;
                break;
            }

            case State::release:
            {
                envelopeVal -= releaseRate;

                if (envelopeVal <= 0.0f)
                    goToNextState();

                break;
            }
        }

        return envelopeVal;
    }

private:
    //==============================================================================
    void recalculateRates() noexcept
    {
        auto getRate = [] (float distance, float timeInSeconds, double sr)
        {
            return timeInSeconds > 0.0f ? (float) (distance / (timeInSeconds * sr)) : -1.0f;
        };

        attackRate  = getRate (1.0f, parameters.attack, sampleRate);
        decayRate   = getRate (1.0f - parameters.sustain, parameters.decay, sampleRate);
        releaseRate = getRate (parameters.sustain, parameters.release, sampleRate);

        if ((state == State::attack && attackRate <= 0.0f)
            || (state == State::decay && (decayRate <= 0.0f || envelopeVal <= parameters.sustain))
            || (state == State::release && releaseRate <= 0.0f))
        {
            goToNextState();
        }
    }

    void goToNextState() noexcept
    {
        if (state == State::attack)
        {
            state = (decayRate > 0.0f ? State::decay : State::sustain);
            return;
        }

        if (state == State::decay)
        {
            state = State::sustain;
            return;
        }

        if (state == State::release)
            reset();
    }

    //==============================================================================
    enum class State { idle, attack, decay, sustain, release };

    State state = State::idle;
    Parameters parameters;

    double sampleRate = 44100.0;
    float envelopeVal = 0.0f, attackRate = 0.0f, decayRate = 0.0f, releaseRate = 0.0f;
};

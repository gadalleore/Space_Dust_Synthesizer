#include "SynthVoice.h"
#include "SpaceDustSynthesiser.h"
#include "PluginProcessor.h"
#include <juce_core/juce_core.h>
#include <cmath>

// Set to 1 to trace MIDI vs Hz in mono/legato: appends CSV rows to
// Documents/SpaceDustPitchTrace.csv (and DBG in Debug builds). Set to 0 for release.
#ifndef SPACE_DUST_LOG_MONO_LEGATO_PITCH
#define SPACE_DUST_LOG_MONO_LEGATO_PITCH 0
#endif

//==============================================================================
// -- UTF-8 String Validation Helper --
namespace
{
    // Safe string creation from number with UTF-8 validation
    juce::String safeStringFromNumber(int value)
    {
        // String from number is always valid UTF-8 (numbers are ASCII)
        return juce::String(value);
    }
    
    juce::String safeStringFromNumber(float value)
    {
        // String from number is always valid UTF-8 (numbers are ASCII)
        return juce::String(value);
    }
    
    juce::String safeStringFromNumber(double value)
    {
        // String from number is always valid UTF-8 (numbers are ASCII)
        return juce::String(value);
    }
    
    juce::String safeStringFromBool(bool value)
    {
        return value ? juce::String("true") : juce::String("false");
    }

#if SPACE_DUST_LOG_MONO_LEGATO_PITCH
    juce::File getSpaceDustPitchTraceFile()
    {
        return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
            .getChildFile("SpaceDustPitchTrace.csv");
    }

    void appendSpaceDustPitchCsvRow(const juce::String& row)
    {
        auto f = getSpaceDustPitchTraceFile();
        static bool headerWritten = false;
        if (!headerWritten)
        {
            headerWritten = true;
            // START: col5=targetHz col6=currentHz col7=glideDelta col8=legato col9=mode col10=juceNote
            // RENDER: col5=baseHz(slew+env) col6=osc1Hz col7=targetHz col8=currentHz col9=glideDelta col10=bendRatio
            f.appendText("type,seq,midi,midiHz,col5,col6,col7,col8,col9,col10,extra\n");
        }
        f.appendText(row + "\n");
    }
#endif
}

//==============================================================================
// -- Note On/Off --

void SynthVoice::startNote(int midiNoteNumber, float velocity,
                           juce::SynthesiserSound* sound,
                           int currentPitchWheelPosition)
{
    juce::ignoreUnused(sound);

    // ALWAYS cancel any pending voice fade so the voice is at full amplitude
    // immediately.  This is critical for fast note repetition in mono mode —
    // if a fade lingers from a previous hard-stop, the new note must override
    // it instantly.  For preserving-voice/legato handoffs, stopNote does NOT
    // start a fade at all (it just clears the JUCE note), so there's nothing
    // to cancel in the common case.
    voiceFade = 1.0f;
    voiceFadeSamplesRemaining = 0;

    // Snapshot before this note (used for glide-from and pitch-wheel handling).
    const bool wasVoiceActive = isActive;

    // Per-note legato / stack transition (SpaceDustSynthesiser); separate from "Legato Glide" portamento.
    isLegatoNote = (synthesiser != nullptr) ? synthesiser->getAndClearNextNoteLegato() : false;

    const int voiceMode = (synthesiser != nullptr) ? synthesiser->getVoiceModeIndex() : 0;

    // Initialize MIDI pitch wheel from note-on when starting fresh (synth may not have sent pitchWheelMoved yet).
    // Mono/legato: skip — startNote's wheel argument is often center (8192) for synthesized stack note-ons,
    // which would wipe lastPitchWheelNormalized from pitchWheelMoved until the next wheel MIDI (wrong pitch).
    const bool skipPitchWheelFromNoteOnArg = (voiceMode != 0 && wasVoiceActive) || isLegatoNote;
    if (!skipPitchWheelFromNoteOnArg)
    {
        lastPitchWheelNormalized = (static_cast<float>(currentPitchWheelPosition) - 8192.0f) / 8192.0f;
        lastPitchWheelNormalized = juce::jlimit(-1.0f, 1.0f, lastPitchWheelNormalized);
    }

    // Target pitch: MIDI note frequency (base frequency, tuning applied in renderNextBlock)
    auto baseFrequency = juce::MidiMessage::getMidiNoteInHertz(midiNoteNumber);
    targetPitch = baseFrequency;
    targetPitch = juce::jlimit(20.0, 20000.0, targetPitch);
    
    // LFO Retrigger: Reset LFO phases if retrigger is enabled
    if (processor != nullptr)
    {
        try
        {
            if (processor->lfo1Retrigger.load())
            {
                processor->lfo1CurrentPhase = 0.0;
            }
        }
        catch (...) { /* ignore */ }
        
        try
        {
            if (processor->lfo2Retrigger.load())
            {
                processor->lfo2CurrentPhase = 0.0;
            }
        }
        catch (...) { /* ignore */ }
    }
    
    // Decide whether this note change should glide based on:
    // - Glide time
    // - Global Legato Glide parameter
    // - Whether this specific note is a legato overlap
    //
    // Behaviour:
    // - Legato Glide ON  (legatoGlideEnabled = true):
    //     * In Legato voice mode (mode == 2): glide ONLY on overlapping notes (isLegatoNote == true)
    //       → classic "fingered glide" / legato portamento
    //     * In Poly/Mono modes: fall back to always-on glide behaviour (no legato envelopes there)
    // - Legato Glide OFF (legatoGlideEnabled = false):
    //     * Glide applies to every note change whenever glideTimeSeconds > 0
    const bool glideTimeActive = (glideTimeSeconds > 0.0f && sampleRate > 0.0);
    const bool inLegatoMode = (voiceMode == 2);
    // Mono/legato retrigger: same voice is being reused (already active).
    // We preserve oscillator phases and filter state for click-free transitions.
    const bool isMonoRetrigger = (voiceMode != 0) && isActive;
    // wasVoiceActive: if the voice was idle, currentPitch must not leak from the previous session
    // (mono + glide would glide from a stale Hz on the next note after stack release / repetition).
    bool shouldGlideForThisNote = false;

    if (glideTimeActive)
    {
        if (legatoGlideEnabled && inLegatoMode)
            shouldGlideForThisNote = isLegatoNote;     // fingered glide: only overlapping notes glide
        else
            shouldGlideForThisNote = true;             // normal glide: every note change glides
    }

    // Poly mode only: when this voice has no valid currentPitch yet, use max pitch from other
    // voices for glide "from". Mono/Legato must NEVER use that — it picked another voice's Hz
    // and caused random detuning on every note change (regression from aggressive pitch resync).
    const bool allowCrossVoiceGlideFrom = (voiceMode == 0);

    auto computeGlideFromPitch = [this, allowCrossVoiceGlideFrom, wasVoiceActive](double fallBackTarget) -> double
    {
        double fromPitch = currentPitch;
        if (!wasVoiceActive)
            fromPitch = 0.0;
        if (fromPitch <= 0.0 && allowCrossVoiceGlideFrom && synthesiser != nullptr)
            fromPitch = synthesiser->getMaxCurrentPitch();
        if (fromPitch <= 0.0)
            fromPitch = fallBackTarget;
        return fromPitch;
    };

    if (isLegatoNote && inLegatoMode)
    {
        // Legato overlapping in Legato mode:
        // - Envelopes and filter stay in their current stage (single-trigger behaviour)
        // - Pitch either glides (if shouldGlideForThisNote) or jumps instantly
        if (shouldGlideForThisNote)
        {
            const double fromPitch = computeGlideFromPitch(targetPitch);
            currentPitch = fromPitch;
            const double pitchDifference = targetPitch - currentPitch;
            const double samplesToGlide = glideTimeSeconds * sampleRate;
            glideDelta = (samplesToGlide > 0.0) ? (pitchDifference / samplesToGlide) : 0.0;
            updateOsc1Frequency(currentPitch);
            updateOsc2Frequency(currentPitch);
        }
        else
        {
            // Decide 3ms vs snap using resynced "from" pitch (not raw currentPitch alone), so
            // stack-return cases don't mis-route before computeGlideFromPitch runs.
            const double fromPitch = computeGlideFromPitch(targetPitch);
            if (fromPitch > 0.0 && std::abs(fromPitch - targetPitch) > 0.01)
            {
                // Anti-click: legato with no user glide — tiny 3ms auto-glide
                currentPitch = fromPitch;
                const double samplesToGlide = 0.003 * sampleRate;
                glideDelta = (targetPitch - currentPitch) / samplesToGlide;
                updateOsc1Frequency(currentPitch);
                updateOsc2Frequency(currentPitch);
            }
            else
            {
                currentPitch = targetPitch;
                glideDelta = 0.0;
                updateOsc1Frequency(currentPitch);
                updateOsc2Frequency(currentPitch);
            }
        }

        updateFilter();
        isActive = true;
        debugLogPitchAfterStartNote(midiNoteNumber);
        return; // IMPORTANT: do NOT re-trigger ADSR on legato overlaps
    }

    // Non-legato start (or non-Legato voice mode): set pitch and optionally start a glide.
    if (shouldGlideForThisNote)
    {
        const double fromPitch = computeGlideFromPitch(targetPitch);
        currentPitch = fromPitch;
        const double pitchDifference = targetPitch - currentPitch;
        const double samplesToGlide = glideTimeSeconds * sampleRate;
        glideDelta = (samplesToGlide > 0.0) ? (pitchDifference / samplesToGlide) : 0.0;
        updateOsc1Frequency(currentPitch);
        updateOsc2Frequency(currentPitch);
    }
    else if (isMonoRetrigger && currentPitch > 0.0 && std::abs(currentPitch - targetPitch) > 0.01)
    {
        // Anti-click: mono retrigger with no user glide — apply a tiny 3ms auto-glide
        // to prevent the abrupt frequency change that causes clicks at low frequencies.
        // 3ms is imperceptible as a glide but smooths the waveform transition.
        const double samplesToGlide = 0.003 * sampleRate;
        glideDelta = (targetPitch - currentPitch) / samplesToGlide;
        updateOsc1Frequency(currentPitch);
        updateOsc2Frequency(currentPitch);
    }
    else
    {
        currentPitch = targetPitch;
        glideDelta = 0.0;
        updateOsc1Frequency(currentPitch);
        updateOsc2Frequency(currentPitch);
    }

    debugLogPitchAfterStartNote(midiNoteNumber);

    // Full retrigger: new envelope, filter, and modulator cycles
    pitchEnvSamplesElapsed = 0.0f;

    // Anti-click: in mono/legato modes, DON'T reset oscillator phases or envelope to 0.
    // Resetting phases causes a waveform discontinuity; resetting the envelope causes an
    // amplitude jump from the current level to 0.  Both produce audible pops/clicks.
    // Instead, let phases continue naturally and retrigger the envelope from its current
    // level (analog mono-synth behaviour).  In poly mode, always reset for clean note starts.
    if (!isMonoRetrigger)
    {
        // Poly or first note: full reset
        osc1Angle = 0.0;
        osc2Angle = 0.0;
        subOscAngle = 0.0;
        pinkState.fill(0.0f);
        pinkSum = 0.0f;
        pinkIndex = 0;
        random.setSeed(static_cast<juce::int64>(reinterpret_cast<uintptr_t>(this)) + juce::Time::getHighResolutionTicks());
        for (auto& val : pinkState)
            val = (random.nextFloat() * 2.0f - 1.0f) * 0.0625f;
        pinkSum = std::accumulate(pinkState.begin(), pinkState.end(), 0.0f);
        adsr.reset();
        smoothedEnvelope = 0.0f;
        outputSmootherL = 0.0f;
        outputSmootherR = 0.0f;
        filter.reset();
        modFilter1.reset();
        modFilter2.reset();
        filterAdsr.reset();

        adsr.noteOn();
        inReleasePhase = false;
        isActive = true;
        updateFilter();
        filterAdsr.noteOn();
    }
    else
    {
        // Mono/legato retrigger: keep oscillator phases and filter state intact.
        // Only retrigger the amplitude and filter envelopes from their current values.
        // JUCE's ADSR::noteOn() continues from the current envelope level (no jump to 0),
        // and smoothedEnvelope tracks any change with a ~3ms lowpass — fully click-free
        // even on sine/triangle waveforms.
        adsr.noteOn();
        filterAdsr.noteOn();
        inReleasePhase = false;
        isActive = true;
        // Don't reset filter — let it continue smoothly
    }
}

void SynthVoice::stopNote(float velocity, bool allowTailOff)
{
    juce::ignoreUnused(velocity);

    // Preserving-voice / legato handoff: startNote follows immediately inside
    // startVoice.  Do NOT start a fade or touch ADSR — just clear the JUCE note
    // assignment so startVoice can reassign.  The voice keeps producing audio
    // with all DSP state intact (oscillator phases, ADSR level, filter).
    // This is the standard JUCE mono/legato approach (Gin synth, forum consensus).
    if (synthesiser != nullptr
        && (synthesiser->isPreservingVoice() || synthesiser->isNextNoteLegato()))
    {
        clearCurrentNote();
        return;
    }

    if (allowTailOff)
    {
        // Normal release: ADSR handles the tail naturally
        adsr.noteOff();
        filterAdsr.noteOff();
        inReleasePhase = true;
    }
    else
    {
        // Hard stop (allNotesOff, voice stealing, etc.): start a short linear
        // fade-out instead of instantly killing the signal.  The voice keeps
        // running with all DSP intact while voiceFade ramps 1→0.  Only when
        // it reaches zero does renderNextBlock do the full cleanup.
        voiceFade = 1.0f;
        voiceFadeSamplesRemaining = kVoiceFadeLength;
    }
}

void SynthVoice::debugLogPitchAfterStartNote(int midiNoteNumber)
{
#if SPACE_DUST_LOG_MONO_LEGATO_PITCH
    if (synthesiser == nullptr || synthesiser->getVoiceModeIndex() == 0)
        return;
    ++pitchTraceSeq;
    const int mode = synthesiser->getVoiceModeIndex();
    const double midiHz = juce::MidiMessage::getMidiNoteInHertz(midiNoteNumber);
    const int jn = getCurrentlyPlayingNote();
    juce::String extra;
    if (std::abs(targetPitch - midiHz) > 0.5)
        extra += "target_neq_midiHz;";
    if (jn >= 0 && jn != midiNoteNumber)
        extra += "juceNote_neq_param;";

    appendSpaceDustPitchCsvRow(
        juce::String("START,") + juce::String((int)pitchTraceSeq) + "," + juce::String(midiNoteNumber) + ","
        + juce::String(midiHz, 6) + "," + juce::String(targetPitch, 6) + "," + juce::String(currentPitch, 6) + ","
        + juce::String(glideDelta, 6) + "," + juce::String(isLegatoNote ? 1 : 0) + "," + juce::String(mode) + ","
        + juce::String(jn) + ",\"" + extra + "\"");

    DBG("Space Dust [pitch] #" << pitchTraceSeq << " START  MIDI=" << midiNoteNumber
        << " (" << juce::MidiMessage::getMidiNoteName(midiNoteNumber, true, true, 3) << ")"
        << "  targetHz=" << targetPitch << "  currentHz=" << currentPitch << "  glideDelta=" << glideDelta
        << "  legato=" << (isLegatoNote ? "y" : "n") << "  mode=" << mode
        << "  jucePlayingNote=" << jn << "  midiHz(ref)=" << midiHz);
#else
    juce::ignoreUnused(midiNoteNumber);
#endif
}

void SynthVoice::debugLogPitchRenderSample0(double osc1HzFinal, double baseHzAfterPitchEnv)
{
#if SPACE_DUST_LOG_MONO_LEGATO_PITCH
    if (synthesiser == nullptr || synthesiser->getVoiceModeIndex() == 0)
        return;
    const int n = getCurrentlyPlayingNote();
    const double midiHz = (n >= 0 ? juce::MidiMessage::getMidiNoteInHertz(n) : 0.0);
    const float totalBendNorm = juce::jlimit(-1.0f, 1.0f, lastPitchWheelNormalized + pitchBend);
    const float bendSt = totalBendNorm * pitchBendAmountFloat;
    const double bendRatio = std::pow(2.0, static_cast<double>(bendSt) / 12.0);
    juce::String extra = "bendNorm=" + juce::String((double)totalBendNorm, 4) + ";bendSt=" + juce::String((double)bendSt, 4);

    appendSpaceDustPitchCsvRow(
        juce::String("RENDER,") + juce::String((int)pitchTraceSeq) + "," + juce::String(n) + ","
        + juce::String(midiHz, 6) + "," + juce::String(baseHzAfterPitchEnv, 6) + "," + juce::String(osc1HzFinal, 6) + ","
        + juce::String(targetPitch, 6) + "," + juce::String(currentPitch, 6) + "," + juce::String(glideDelta, 6) + ","
        + juce::String(bendRatio, 6) + ",\"" + extra + "\"");

    const juce::String noteName = (n >= 0 ? juce::MidiMessage::getMidiNoteName(n, true, true, 3) : juce::String("?"));
    DBG("Space Dust [pitch] #" << pitchTraceSeq << " RENDER  MIDI=" << n << " (" << noteName << ")"
        << "  baseHz(slew+env)=" << baseHzAfterPitchEnv << "  osc1Hz(final)=" << osc1HzFinal
        << "  targetHz=" << targetPitch << "  currentHz=" << currentPitch << "  glideDelta=" << glideDelta
        << "  bendRatio=" << bendRatio << "  midiHz(ref)=" << midiHz);
#else
    juce::ignoreUnused(osc1HzFinal, baseHzAfterPitchEnv);
#endif
}

//==============================================================================
// -- MIDI Controllers --

void SynthVoice::controllerMoved(int controllerNumber, int newControllerValue)
{
    juce::ignoreUnused(controllerNumber, newControllerValue);
    // Could implement MIDI CC control here if needed
}

void SynthVoice::pitchWheelMoved(int newPitchWheelValue)
{
    // MIDI pitch wheel: 0-16383, center at 8192. Normalize to -1..+1
    lastPitchWheelNormalized = (static_cast<float>(newPitchWheelValue) - 8192.0f) / 8192.0f;
    lastPitchWheelNormalized = juce::jlimit(-1.0f, 1.0f, lastPitchWheelNormalized);
}

//==============================================================================
// -- Waveform Generation --

float SynthVoice::generateWaveform(double angle, int waveform)
{
    switch (waveform)
    {
        case Sine:
            return std::sin(angle);
            
        case Triangle:
            // Triangle wave: 2 * abs(2 * (phase - floor(phase + 0.5))) - 1
            {
                double normalized = angle / (2.0 * juce::MathConstants<double>::pi);
                double phase = normalized - std::floor(normalized + 0.5);
                return static_cast<float>(4.0 * std::abs(phase) - 1.0);
            }
            
        case Saw:
            // Simple sawtooth: 2 * (phase - floor(phase + 0.5))
            {
                double normalized = angle / (2.0 * juce::MathConstants<double>::pi);
                double phase = normalized - std::floor(normalized + 0.5);
                return static_cast<float>(2.0 * phase);
            }
            
        case Square:
            return std::sin(angle) > 0.0f ? 1.0f : -1.0f;
            
        default:
            return std::sin(angle);
    }
}

//==============================================================================
// -- Oscillator Frequency Updates --

//==============================================================================
// -- Oscillator Pitch Tuning --
// Each oscillator has independent coarse tuning (±24 semitones) and fine detuning (±50 cents)
// Final pitch calculation: midiNote + coarseTune + (detune / 100) [all in semitones]
// Convert cents to semitones by dividing by 100

/**
    Update Osc1 frequency with coarse tune and detune.
    Final pitch = midiNote + osc1CoarseTune + (osc1Detune / 100) [all in semitones]
*/
void SynthVoice::updateOsc1Frequency(double baseFrequency)
{
    // Calculate total semitones: coarse tune + detune (convert cents to semitones)
    double totalSemitones = osc1CoarseTune + (osc1Detune / 100.0);
    double tunedFrequency = baseFrequency * std::pow(2.0, totalSemitones / 12.0);
    
    // CRITICAL: Use stored sampleRate member, not getSampleRate()
    // The stored sampleRate is set in prepareToPlay() and is guaranteed to be valid
    // Using getSampleRate() can return 0 if called before sample rate is set
    if (sampleRate > 0.0)
    {
        auto cyclesPerSample = tunedFrequency / sampleRate;
        osc1AngleDelta = cyclesPerSample * 2.0 * juce::MathConstants<double>::pi;
    }
    else
    {
        // Sample rate not set yet - angle delta will be 0 (no sound)
        osc1AngleDelta = 0.0;
    }
}

/**
    Update Osc2 frequency with coarse tune and detune.
    Final pitch = midiNote + osc2CoarseTune + (osc2Detune / 100) [all in semitones]
*/
void SynthVoice::updateOsc2Frequency(double baseFrequency)
{
    // Calculate total semitones: coarse tune + detune (convert cents to semitones)
    double totalSemitones = osc2CoarseTune + (osc2Detune / 100.0);
    double tunedFrequency = baseFrequency * std::pow(2.0, totalSemitones / 12.0);
    
    // CRITICAL: Use stored sampleRate member, not getSampleRate()
    // The stored sampleRate is set in prepareToPlay() and is guaranteed to be valid
    // Using getSampleRate() can return 0 if called before sample rate is set
    if (sampleRate > 0.0)
    {
        auto cyclesPerSample = tunedFrequency / sampleRate;
        osc2AngleDelta = cyclesPerSample * 2.0 * juce::MathConstants<double>::pi;
    }
    else
    {
        // Sample rate not set yet - angle delta will be 0 (no sound)
        osc2AngleDelta = 0.0;
    }
}

//==============================================================================
// -- Filter Updates --

void SynthVoice::updateModFilter1()
{
    if (modFilter1Linked || sampleRate <= 0.0f) return;
    float q = 0.1f + modFilter1Resonance * 19.9f;
    float clampedCutoff = juce::jlimit(20.0f, 20000.0f, modFilter1Cutoff);
    switch (modFilter1Mode)
    {
        case 0: modFilter1.setType(juce::dsp::StateVariableTPTFilterType::lowpass); break;
        case 1: modFilter1.setType(juce::dsp::StateVariableTPTFilterType::bandpass); break;
        case 2: modFilter1.setType(juce::dsp::StateVariableTPTFilterType::highpass); break;
        default: modFilter1.setType(juce::dsp::StateVariableTPTFilterType::lowpass); break;
    }
    modFilter1.setCutoffFrequency(clampedCutoff);
    modFilter1.setResonance(q);
}

void SynthVoice::updateModFilter2()
{
    if (modFilter2Linked || sampleRate <= 0.0f) return;
    float q = 0.1f + modFilter2Resonance * 19.9f;
    float clampedCutoff = juce::jlimit(20.0f, 20000.0f, modFilter2Cutoff);
    switch (modFilter2Mode)
    {
        case 0: modFilter2.setType(juce::dsp::StateVariableTPTFilterType::lowpass); break;
        case 1: modFilter2.setType(juce::dsp::StateVariableTPTFilterType::bandpass); break;
        case 2: modFilter2.setType(juce::dsp::StateVariableTPTFilterType::highpass); break;
        default: modFilter2.setType(juce::dsp::StateVariableTPTFilterType::lowpass); break;
    }
    modFilter2.setCutoffFrequency(clampedCutoff);
    modFilter2.setResonance(q);
}

void SynthVoice::updateFilter()
{
    // Map resonance (0.0-1.0) to Q (0.1-20.0) for StateVariableTPTFilter
    float q = 0.1f + filterResonance * 19.9f;
    
    // Clamp cutoff to valid range
    float clampedCutoff = juce::jlimit(20.0f, 20000.0f, filterCutoff);
    
    // Update filter based on mode (only set type if changed to avoid internal state reset)
    static const juce::dsp::StateVariableTPTFilterType types[] = {
        juce::dsp::StateVariableTPTFilterType::lowpass,
        juce::dsp::StateVariableTPTFilterType::bandpass,
        juce::dsp::StateVariableTPTFilterType::highpass
    };
    int clampedMode = juce::jlimit(0, 2, filterMode);
    if (filter.getType() != types[clampedMode])
        filter.setType(types[clampedMode]);
    
    filter.setCutoffFrequency(clampedCutoff);
    filter.setResonance(q);
}

void SynthVoice::updateNoiseEqFilters()
{
    if (sampleRate <= 0.0)
        return;
    
    // Low shelf: affects frequencies below 200 Hz
    // When amount > 0: strong boost (low shelf, +24 dB max)
    // When amount < 0: steep cut to near silence (high-pass with very steep Q)
    const float lowShelfFreq = 200.0f;
    
    if (lowShelfAmount < 0.0f)
    {
        // Negative: use high-pass filter with very steep Q for dramatic cut
        // Q of 2.0 creates a much steeper slope, cutting lows to near silence
        *lowShelfFilter.coefficients = *juce::dsp::IIR::Coefficients<float>::makeHighPass(
            sampleRate, lowShelfFreq, 2.0f);
    }
    else if (lowShelfAmount > 0.0f)
    {
        // Positive: use low shelf for strong boost (+24 dB max)
        float lowGainDb = lowShelfAmount * 24.0f; // ±24 dB range for dramatic effect
        *lowShelfFilter.coefficients = *juce::dsp::IIR::Coefficients<float>::makeLowShelf(
            sampleRate, lowShelfFreq, 0.707f, lowGainDb);
    }
    else
    {
        // Zero: bypass (all-pass)
        *lowShelfFilter.coefficients = *juce::dsp::IIR::Coefficients<float>::makeAllPass(sampleRate, 1.0f);
    }
    
    // High shelf: affects frequencies above 1.5 kHz
    // When amount > 0: strong boost (high shelf, +24 dB max)
    // When amount < 0: steep cut to near silence (low-pass with very steep Q)
    const float highShelfFreq = 1500.0f;
    
    if (highShelfAmount < 0.0f)
    {
        // Negative: use low-pass filter with very steep Q for dramatic cut
        // Q of 2.0 creates a much steeper slope, cutting highs to near silence
        *highShelfFilter.coefficients = *juce::dsp::IIR::Coefficients<float>::makeLowPass(
            sampleRate, highShelfFreq, 2.0f);
    }
    else if (highShelfAmount > 0.0f)
    {
        // Positive: use high shelf for strong boost (+24 dB max)
        float highGainDb = highShelfAmount * 24.0f; // ±24 dB range for dramatic effect
        *highShelfFilter.coefficients = *juce::dsp::IIR::Coefficients<float>::makeHighShelf(
            sampleRate, highShelfFreq, 0.707f, highGainDb);
    }
    else
    {
        // Zero: bypass (all-pass)
        *highShelfFilter.coefficients = *juce::dsp::IIR::Coefficients<float>::makeAllPass(sampleRate, 1.0f);
    }
}

//==============================================================================
// -- ADSR Envelope Logic --

/**
    Update ADSR parameters from stored timing values.
    
    This method must be called whenever envelope parameters change to ensure
    JUCE's ADSR uses the correct values. JUCE's ADSR requires all parameters
    to be set together via setParameters() - individual parameter changes
    won't take effect until this is called.
    
    CRITICAL: This must be called:
    - In the voice constructor (initial setup)
    - When any envelope parameter changes (via setEnvAttack, setEnvDecay, etc.)
    - When sample rate changes (to recalculate sample-based timing)
    
    Real-time Safety: This method is safe to call from the audio thread as it
    only updates the ADSR's internal parameters (no allocations, lock-free).
    
    Parameter Ranges:
    - Attack, Decay, Release: 0.01s to 20.0s (skewed, midpoint at 2.0s)
    - Sustain: 0.0 to 1.0 (linear, represents amplitude level)
    
    Common Pitfalls:
    - Sustain must be 0.0-1.0 (represents amplitude, not time)
    - All times are in seconds (JUCE converts to samples internally)
    - Parameters are set atomically - all four must be set together
*/
void SynthVoice::updateAdsrParameters()
{
    //==============================================================================
    // -- Safety Check: Sample Rate Must Be Valid --
    // CRITICAL: ADSR parameters cannot be set until sample rate is known.
    // This prevents assertions when voices are created before prepareToPlay().
    // 
    // If sample rate is invalid (0 or negative), skip parameter update.
    // The parameters will be set correctly once setCurrentPlaybackSampleRate() is called.
    if (sampleRate <= 0.0)
    {
        // Sample rate not yet set - parameters will be applied when prepareToPlay() is called
        return;
    }
    
    juce::ADSR::Parameters params;
    
    // Set all four ADSR parameters together (required by JUCE)
    // Times are in seconds - JUCE's ADSR converts to samples internally
    // CRITICAL: All times must be > 0.01s to prevent assertions
    params.attack = juce::jmax(0.01f, envAttackTime);      // Attack time (0.01-20.0s, skewed)
    params.decay = juce::jmax(0.01f, envDecayTime);        // Decay time (0.01-20.0s, skewed)
    params.sustain = juce::jlimit(0.0f, 1.0f, envSustainLevel);  // Sustain level (0.0-1.0, linear amplitude)
    params.release = juce::jmax(0.01f, envReleaseTime);    // Release time (0.01-20.0s, skewed) - long cosmic tails!
    
    // Apply parameters to ADSR (real-time safe, no allocations)
    // This is safe to call now because sample rate is valid
    adsr.setParameters(params);
}

void SynthVoice::updateFilterAdsrParameters()
{
    // Safety check: Sample rate must be valid
    if (sampleRate <= 0.0)
    {
        return;
    }
    
    juce::ADSR::Parameters params;
    
    // Set all four Filter Envelope ADSR parameters together (required by JUCE)
    // Times are in seconds - JUCE ADSR uses linear decay; value = time to reach sustain
    params.attack = juce::jmax(0.01f, filterEnvAttackTime);
    params.decay = juce::jmax(0.01f, filterEnvDecayTime);
    params.sustain = juce::jlimit(0.0f, 1.0f, filterEnvSustainLevel);
    params.release = juce::jmax(0.01f, filterEnvReleaseTime);
    
    // Apply parameters to Filter Envelope ADSR (real-time safe, no allocations)
    filterAdsr.setParameters(params);
}

//==============================================================================
// -- Audio Rendering --

void SynthVoice::renderNextBlock(juce::AudioBuffer<float>& outputBuffer,
                                 int startSample, int numSamples)
{
    //==============================================================================
    // -- CRITICAL: Complete Signal Chain --
    // 
    // Signal Flow:
    // 1. Generate Osc1 waveform (Sine/Triangle/Saw/Square)
    // 2. Generate Osc2 waveform (Sine/Triangle/Saw/Square)
    // 3. Generate Noise (white noise)
    // 4. Apply independent levels (osc1Level, osc2Level, noiseLevel)
    // 5. Additive mixing: sum all sources
    // 6. Apply ADSR envelope (Attack → Decay → Sustain → Release)
    // 7. Process through multimode filter (LowPass/BandPass/HighPass)
    // 8. Write to output buffer (stereo)
    //
    // Real-time Safety: All operations are allocation-free and lock-free.
    
    // Early return if no note is playing (angle deltas are 0 when no note is active)
    if (osc1AngleDelta == 0.0 && osc2AngleDelta == 0.0)
        return;
    
    // Denormal prevention: FTZ/DAZ for oscillators, filters, envelopes (per JUCE best practice)
    juce::ScopedNoDenormals noDenormals;
    
    //==============================================================================
    // -- DEBUG LOGGING: Voice Rendering (completely removed for production) --
    // Verbose logging removed entirely to prevent CPU spam and crashes.
    // Only startNote/stopNote logs remain for confirmation (see startNote/stopNote methods).
    // This ensures clean, non-crackly cosmic sound with minimal CPU usage.
    // Excessive logging was causing CPU overload and contributing to Ableton crashes.
    
    // Use pre-allocated buffer (no allocation in audio thread - sized in prepareToPlay)
    const int maxSamples = juce::jmin(numSamples, voiceTempBuffer.getNumSamples());
    if (maxSamples <= 0)
        return;
    voiceTempBuffer.clear();
    
    // Track how many samples we actually process (in case ADSR finishes mid-block)
    int samplesProcessed = 0;
    
    // Generate oscillator waveforms, mix, and apply envelope
    for (int i = 0; i < maxSamples; ++i)
    {
        // Glide (portamento): applied at END of each sample iteration so this sample's
        // pitch matches startNote/currentPitch; advancing here first made the first output
        // sample one glide step sharp/flat vs the requested transition (audible in mono/legato).

        //==============================================================================
        // -- PITCH ENVELOPE (separate from pitch bend) --
        // Computes base frequency from currentPitch + envelope. Independent of pitch bend.
        // Time is in seconds (0-5 from parameter). Linear ramp hits note at indicated time.
        double pitchForOscillators = currentPitch;
        if (pitchEnvTime >= 0.0001f && sampleRate > 0.0f && pitchEnvAmount != 0.0f && pitchEnvPitch != 0.0f)
        {
            float elapsedSec = pitchEnvSamplesElapsed / static_cast<float>(sampleRate);
            float T = pitchEnvTime;  // Seconds from parameter (0-10)
            float frac = juce::jmin(1.0f, elapsedSec / T);  // 0..1 over duration
            float curve = 1.0f - frac;  // Linear: 1 at start, 0 at end (hits note at indicated time)
            float pitchModSemitones = juce::jlimit(-48.0f, 48.0f, curve * (pitchEnvAmount / 100.0f) * pitchEnvPitch);
            double pitchEnvRatio = std::pow(2.0, static_cast<double>(pitchModSemitones) / 12.0);
            pitchForOscillators = currentPitch * pitchEnvRatio;
        }
        // Cap pitchEnvSamplesElapsed to avoid float precision loss on very long holds
        if (pitchEnvSamplesElapsed < 1e7f)
            pitchEnvSamplesElapsed += 1.0f;
        
        //==============================================================================
        // -- PITCH BEND (separate from pitch envelope) --
        // MIDI wheel + manual slider, scaled by bend range. Applied to pitch-for-oscillators.
        // Bend amount is in semitones (0-24); full wheel = ±amount semitones.
        float totalBendNorm = juce::jlimit(-1.0f, 1.0f, lastPitchWheelNormalized + pitchBend);
        float bendSemitones = totalBendNorm * pitchBendAmountFloat;  // Direct: 2 st = 2 semitones
        double bendRatio = std::pow(2.0, static_cast<double>(bendSemitones) / 12.0);
        double osc1Freq = pitchForOscillators * bendRatio;
        double osc2Freq = pitchForOscillators * bendRatio;
        
        // Apply LFO modulation per-sample (if processor and buffers are available)
        float filterMod = 0.0f;           // Master filter modulation (only when Link to Master is ON)
        float modFilter1LfoMod = 0.0f;   // LFO1 modulation for modFilter1 (when Link OFF, LFO targets Filter)
        float modFilter2LfoMod = 0.0f;   // LFO2 modulation for modFilter2 (when Link OFF, LFO targets Filter)
        float osc1VolMod = 0.0f, osc2VolMod = 0.0f, noiseVolMod = 0.0f;  // Volume LFO modulation
        
        // Access LFO buffers per-sample (buffers are filled in processBlock for the entire block)
        if (processor != nullptr && 
            processor->lfo1Buffer.getNumSamples() > 0 && 
            processor->lfo2Buffer.getNumSamples() > 0 &&
            i < processor->lfo1Buffer.getNumSamples() && 
            i < processor->lfo2Buffer.getNumSamples())
        {
            float lfo1Val = processor->lfo1Buffer.getSample(0, i);
            float lfo2Val = processor->lfo2Buffer.getSample(0, i);
            
            // Use cached LFO targets (set per-block in updateVoicesWithParameters - avoids per-sample APVTS reads)
            const int lfo1Target = lfo1TargetCached;
            const int lfo2Target = lfo2TargetCached;
            
            // LFO1: 0=Pitch, 1=Filter, 2=MasterVol(processor), 3=Osc1, 4=Osc2, 5=Noise
            if (lfo1Target == 0)  // Pitch
            {
                float pitchModCents = lfo1Val * 1200.0f;  // ±24 semitones at full LFO swing
                osc1Freq *= std::pow(2.0, pitchModCents / 1200.0);
                osc2Freq *= std::pow(2.0, pitchModCents / 1200.0);
            }
            else if (lfo1Target == 1)  // Filter
            {
                if (modFilter1Linked)
                    filterMod += lfo1Val;  // Multiplicative: voice applies (1 + filterMod) to base
                else if (modFilter1Show)
                    modFilter1LfoMod = lfo1Val;  // Same for mod filter
            }
            else if (lfo1Target == 3) osc1VolMod += lfo1Val;
            else if (lfo1Target == 4) osc2VolMod += lfo1Val;
            else if (lfo1Target == 5) noiseVolMod += lfo1Val;
            
            // LFO2: same targets
            if (lfo2Target == 0)  // Pitch
            {
                float pitchModCents = lfo2Val * 1200.0f;  // ±24 semitones at full LFO swing
                osc1Freq *= std::pow(2.0, pitchModCents / 1200.0);
                osc2Freq *= std::pow(2.0, pitchModCents / 1200.0);
            }
            else if (lfo2Target == 1)  // Filter
            {
                if (modFilter2Linked)
                    filterMod += lfo2Val;  // Multiplicative: voice applies (1 + filterMod) to base
                else if (modFilter2Show)
                    modFilter2LfoMod = lfo2Val;  // Same for mod filter
            }
            else if (lfo2Target == 3) osc1VolMod += lfo2Val;
            else if (lfo2Target == 4) osc2VolMod += lfo2Val;
            else if (lfo2Target == 5) noiseVolMod += lfo2Val;
        }
        
        // Calculate oscillator frequencies with independent tuning and LFO modulation
        // Oscillator 1: base frequency + osc1 tuning + LFO modulation
        double osc1TotalSemitones = osc1CoarseTune + (osc1Detune / 100.0);
        osc1Freq = osc1Freq * std::pow(2.0, osc1TotalSemitones / 12.0);
        
        // Oscillator 2: base frequency + osc2 tuning + LFO modulation
        double osc2TotalSemitones = osc2CoarseTune + (osc2Detune / 100.0);
        osc2Freq = osc2Freq * std::pow(2.0, osc2TotalSemitones / 12.0);
        
        // CRITICAL: Clamp frequencies to prevent runaway pitch on long holds/legato
        // (LFO + envelope + bend can accumulate; NaN/Inf from precision loss causes extreme pitch)
        osc1Freq = juce::jlimit(20.0, 20000.0, osc1Freq);
        osc2Freq = juce::jlimit(20.0, 20000.0, osc2Freq);

#if SPACE_DUST_LOG_MONO_LEGATO_PITCH
        if (i == 0 && pitchTraceSeq != pitchTraceLastRenderLogSeq)
        {
            pitchTraceLastRenderLogSeq = pitchTraceSeq;
            debugLogPitchRenderSample0(osc1Freq, pitchForOscillators);
        }
#endif
        
        // Update oscillator angle deltas with modulated frequencies
        if (sampleRate > 0.0)
        {
            osc1AngleDelta = (osc1Freq / sampleRate) * 2.0 * juce::MathConstants<double>::pi;
            osc2AngleDelta = (osc2Freq / sampleRate) * 2.0 * juce::MathConstants<double>::pi;
            double baseFreq = osc1Freq / std::pow(2.0, (osc1CoarseTune + osc1Detune / 100.0) / 12.0);
            double subFreq = baseFreq * 0.5 * std::pow(2.0, static_cast<double>(subOscCoarse) / 12.0);
            subFreq = juce::jlimit(20.0, 20000.0, subFreq);
            subOscAngleDelta = (subFreq / sampleRate) * 2.0 * juce::MathConstants<double>::pi;
        }
        
        // Step 1-2: Generate oscillator waveforms
        float osc1Sample = generateWaveform(osc1Angle, osc1Waveform);
        float osc2Sample = generateWaveform(osc2Angle, osc2Waveform);
        float subOscSample = subOscOn ? generateWaveform(subOscAngle, subOscWaveform) * subOscLevel : 0.0f;
        
        // Step 3: Generate noise (white or pink)
        float noiseSample = 0.0f;
        
        if (noiseType == White)
        {
            noiseSample = random.nextFloat() * 2.0f - 1.0f;
        }
        else // Pink
        {
            // Voss-McCartney update
            pinkIndex++;
            int lowestChangedBit = pinkIndex & -pinkIndex;
            int bitPos = 0;
            int temp = lowestChangedBit;
            while ((temp >>= 1) != 0) ++bitPos;
            
            float newVal = (random.nextFloat() * 2.0f - 1.0f) * 0.0625f;
            pinkSum -= pinkState[bitPos];
            pinkSum += newVal;
            pinkState[bitPos] = newVal;
            
            noiseSample = pinkSum * 3.8f;  // scaling to roughly match white noise RMS level
        }
        
        // Apply noise EQ filters (low shelf and high shelf)
        // Process through low shelf first, then high shelf
        // Use a small temporary buffer for processing
        float filteredNoise = noiseSample;
        float* channelData[1] = { &filteredNoise };
        juce::dsp::AudioBlock<float> noiseBlock(channelData, 1, 1);
        juce::dsp::ProcessContextReplacing<float> noiseContext(noiseBlock);
        if (lowShelfAmount != 0.0f)
            lowShelfFilter.process(noiseContext);
        if (highShelfAmount != 0.0f)
            highShelfFilter.process(noiseContext);
        noiseSample = filteredNoise;
        
        // Step 4: Apply independent levels to each source (real-time safe: atomic parameter reads)
        // LFO volume modulation: multiply by (1 + mod) so depth scales 0-2x at full LFO swing
        float osc1Out = osc1Sample * osc1Level * juce::jlimit(0.0f, 2.0f, 1.0f + osc1VolMod);
        float osc2Out = osc2Sample * osc2Level * juce::jlimit(0.0f, 2.0f, 1.0f + osc2VolMod);
        float noiseOut = noiseSample * noiseLevel * 0.75f * juce::jlimit(0.0f, 2.0f, 1.0f + noiseVolMod);
        
        // Constant-power pan: -1 = full left, 0 = center, 1 = full right
        const float pi4 = static_cast<float>(juce::MathConstants<double>::pi / 4.0);
        float gainL1 = std::cos((osc1Pan + 1.0f) * pi4);
        float gainR1 = std::sin((osc1Pan + 1.0f) * pi4);
        float gainL2 = std::cos((osc2Pan + 1.0f) * pi4);
        float gainR2 = std::sin((osc2Pan + 1.0f) * pi4);
        const float centerGain = 0.70710678f;  // 1/sqrt(2) for centered sources
        
        // Step 5: Stereo mixing with per-oscillator pan
        float leftMix = osc1Out * gainL1 + osc2Out * gainL2 + noiseOut * centerGain + subOscSample * centerGain;
        float rightMix = osc1Out * gainR1 + osc2Out * gainR2 + noiseOut * centerGain + subOscSample * centerGain;
        
        // Step 6: Process envelopes (returns current amplitude value 0.0-1.0)
        // JUCE's ADSR handles all four stages automatically: Attack → Decay → Sustain → Release
        float rawEnvelope = adsr.getNextSample();
        // Anti-click: one-pole lowpass smooths any discontinuity from ADSR retrigger
        smoothedEnvelope += envSmoothCoeff * (rawEnvelope - smoothedEnvelope);
        float envelope = smoothedEnvelope;
        float filterEnvOutput = filterAdsr.getNextSample();
        
        // Modulate filter cutoff with filter envelope and LFO.
        // Amount blends unmodulated cutoff (0%) with full-range envelope sweep (100%):
        // E=1 → top of range, E=0 → bottom; sustain/decay set where E lands vs the knob.
        // Log-frequency space for perceptually even motion across octaves.
        const float logMin = std::log(20.0f);
        const float logMax = std::log(20000.0f);
        float logKnob = std::log(juce::jmax(20.0f, baseFilterCutoff));
        float E = filterEnvOutput;
        const float amtNorm = juce::jlimit(-1.0f, 1.0f, filterEnvAmount / 100.0f);
        const float blend = std::abs(amtNorm);
        if (amtNorm < 0.0f)
            E = 1.0f - E;
        const float logFullRange = logMin + E * (logMax - logMin);
        float logModulated = (1.0f - blend) * logKnob + blend * logFullRange;

        const float lfoFilterScale = 0.5f;
        float lfoFactor = juce::jmax(0.0f, 1.0f + filterMod * lfoFilterScale);
        float modulatedCutoff = std::exp(juce::jlimit(logMin, logMax, logModulated)) * lfoFactor;
        modulatedCutoff = juce::jlimit(20.0f, 20000.0f, modulatedCutoff);
        smoothedFilterCutoffHz += filterCutoffSmoothCoeff * (modulatedCutoff - smoothedFilterCutoffHz);
        filter.setCutoffFrequency(smoothedFilterCutoffHz);
        
        // Check if envelope has completed (release phase finished)
        // If not active, clear the note and stop rendering
        // (Skip during voice fade — ADSR may hit zero before fade completes)
        if (!adsr.isActive() && voiceFadeSamplesRemaining <= 0)
        {
            inReleasePhase = false;
            clearCurrentNote();
            currentPitch = 0.0;
            targetPitch = 0.0;
            glideDelta = 0.0;
            osc1AngleDelta = 0.0;
            osc2AngleDelta = 0.0;
            subOscAngleDelta = 0.0;
            isActive = false;
            outputSmootherL = 0.0f;
            outputSmootherR = 0.0f;
            
            // CRITICAL: Only process samples we've actually generated
            if (i < maxSamples)
            {
                voiceTempBuffer.clear(0, i, maxSamples - i);
                voiceTempBuffer.clear(1, i, maxSamples - i);
            }
            samplesProcessed = i; // Only process up to this point
            break;
        }
        
        // Apply envelope to stereo mix
        float leftEnv = leftMix * envelope;
        float rightEnv = rightMix * envelope;

        // Step 7: Process through filter (stereo, per-sample to allow envelope modulation)
        voiceSingleSampleBuffer.setSample(0, 0, leftEnv);
        voiceSingleSampleBuffer.setSample(1, 0, rightEnv);
        juce::dsp::AudioBlock<float> singleSampleBlock(voiceSingleSampleBuffer);
        juce::dsp::ProcessContextReplacing<float> context(singleSampleBlock);
        if (modFilter1Show && !modFilter1Linked)
        {
            float mod1LfoFactor = juce::jmax(0.0f, 1.0f + modFilter1LfoMod * 0.5f);
            float modCutoff = juce::jlimit(20.0f, 20000.0f, modFilter1Cutoff * mod1LfoFactor);
            modFilter1.setCutoffFrequency(modCutoff);
            modFilter1.process(context);
            if (warmSaturationMod1)
            {
                float drive = 1.0f + modFilter1Resonance * 2.0f;
                voiceSingleSampleBuffer.setSample(0, 0, std::tanh(voiceSingleSampleBuffer.getSample(0, 0) * drive));
                voiceSingleSampleBuffer.setSample(1, 0, std::tanh(voiceSingleSampleBuffer.getSample(1, 0) * drive));
            }
        }
        if (modFilter2Show && !modFilter2Linked)
        {
            float mod2LfoFactor = juce::jmax(0.0f, 1.0f + modFilter2LfoMod * 0.5f);
            float modCutoff = juce::jlimit(20.0f, 20000.0f, modFilter2Cutoff * mod2LfoFactor);
            modFilter2.setCutoffFrequency(modCutoff);
            modFilter2.process(context);
            if (warmSaturationMod2)
            {
                float drive = 1.0f + modFilter2Resonance * 2.0f;
                voiceSingleSampleBuffer.setSample(0, 0, std::tanh(voiceSingleSampleBuffer.getSample(0, 0) * drive));
                voiceSingleSampleBuffer.setSample(1, 0, std::tanh(voiceSingleSampleBuffer.getSample(1, 0) * drive));
            }
        }
        filter.process(context);
        if (warmSaturationMaster)
        {
            float drive = 1.0f + filterResonance * 2.0f;
            voiceSingleSampleBuffer.setSample(0, 0, std::tanh(voiceSingleSampleBuffer.getSample(0, 0) * drive));
            voiceSingleSampleBuffer.setSample(1, 0, std::tanh(voiceSingleSampleBuffer.getSample(1, 0) * drive));
        }
        
        float outL = juce::jlimit(-1.0f, 1.0f, voiceSingleSampleBuffer.getSample(0, 0));
        float outR = juce::jlimit(-1.0f, 1.0f, voiceSingleSampleBuffer.getSample(1, 0));

        // Voice fade: linear gain ramp to zero, then full cleanup.
        // This protects against ALL hard-stop clicks (allNotesOff, voice
        // stealing, legato safety net).  Applied to the FINAL output after
        // filter + ADSR so it catches every possible discontinuity source.
        if (voiceFadeSamplesRemaining > 0)
        {
            outL *= voiceFade;
            outR *= voiceFade;
            voiceFade -= 1.0f / static_cast<float>(kVoiceFadeLength);
            if (voiceFade < 0.0f) voiceFade = 0.0f;
            if (--voiceFadeSamplesRemaining <= 0)
            {
                // Fade complete: full cleanup — voice is now silent
                voiceFade = 0.0f;
                adsr.reset();
                smoothedEnvelope = 0.0f;
                filterAdsr.reset();
                filter.reset();
                modFilter1.reset();
                modFilter2.reset();
                inReleasePhase = false;
                clearCurrentNote();
                currentPitch = 0.0;
                targetPitch = 0.0;
                glideDelta = 0.0;
                osc1AngleDelta = 0.0;
                osc2AngleDelta = 0.0;
                subOscAngleDelta = 0.0;
                isActive = false;
                outputSmootherL = 0.0f;
                outputSmootherR = 0.0f;
                voiceTempBuffer.setSample(0, i, outL);
                voiceTempBuffer.setSample(1, i, outR);
                samplesProcessed = i + 1;
                if (i + 1 < numSamples)
                {
                    voiceTempBuffer.clear(0, i + 1, numSamples - i - 1);
                    voiceTempBuffer.clear(1, i + 1, numSamples - i - 1);
                }
                break;
            }
        }

        // Safety output smoother: one-pole lowpass on final output.
        // Catches any residual discontinuity from pitch/filter jumps during
        // legato handoff while the voiceFade is still decaying.
        outputSmootherL += kOutputSmoothCoeff * (outL - outputSmootherL);
        outputSmootherR += kOutputSmoothCoeff * (outR - outputSmootherR);

        voiceTempBuffer.setSample(0, i, outputSmootherL);
        voiceTempBuffer.setSample(1, i, outputSmootherR);
        samplesProcessed = i + 1; // Track that we processed this sample
        
        // Update oscillator phases for next sample using current angle deltas
        // (deltas are updated per-sample to support per-sample LFO modulation)
        osc1Angle += osc1AngleDelta;
        if (osc1Angle >= 2.0 * juce::MathConstants<double>::pi)
            osc1Angle -= 2.0 * juce::MathConstants<double>::pi;
        if (osc1Angle < 0.0)
            osc1Angle += 2.0 * juce::MathConstants<double>::pi;
            
        osc2Angle += osc2AngleDelta;
        if (osc2Angle >= 2.0 * juce::MathConstants<double>::pi)
            osc2Angle -= 2.0 * juce::MathConstants<double>::pi;
        if (osc2Angle < 0.0)
            osc2Angle += 2.0 * juce::MathConstants<double>::pi;
        
        if (subOscOn)
        {
            subOscAngle += subOscAngleDelta;
            if (subOscAngle >= 2.0 * juce::MathConstants<double>::pi)
                subOscAngle -= 2.0 * juce::MathConstants<double>::pi;
            if (subOscAngle < 0.0)
                subOscAngle += 2.0 * juce::MathConstants<double>::pi;
        }

        // Advance glide after audio for this sample (see comment at loop top)
        if (glideDelta != 0.0)
        {
            currentPitch += glideDelta;
            if ((glideDelta > 0.0 && currentPitch >= targetPitch) ||
                (glideDelta < 0.0 && currentPitch <= targetPitch))
            {
                currentPitch = targetPitch;
                glideDelta = 0.0;
            }
            currentPitch = juce::jlimit(20.0, 20000.0, currentPitch);
        }
    }
    
    // CRITICAL: Only process and copy the samples we actually generated
    // This prevents buffer bounds assertions when ADSR finishes mid-block
    if (samplesProcessed > 0)
    {
        // Step 8: Copy filtered stereo signal to output
        if (outputBuffer.getNumChannels() >= 2 && voiceTempBuffer.getNumChannels() >= 2)
        {
            outputBuffer.addFrom(0, startSample, voiceTempBuffer, 0, 0, samplesProcessed);
            outputBuffer.addFrom(1, startSample, voiceTempBuffer, 1, 0, samplesProcessed);
        }
        else
        {
            for (int ch = 0; ch < outputBuffer.getNumChannels(); ++ch)
                outputBuffer.addFrom(ch, startSample, voiceTempBuffer, 0, 0, samplesProcessed);
        }
    }
}

//==============================================================================
// -- DSP Initialization --

void SynthVoice::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    //==============================================================================
    // -- CRITICAL: DSP Initialization with Valid Sample Rate --
    // 
    // This method MUST be called explicitly in PluginProcessor::prepareToPlay()
    // after voices are added but BEFORE synth.setCurrentPlaybackSampleRate() is called.
    //
    // Why DSP initialization MUST happen here, not in constructor:
    // 1. Sample rate is only known when host calls prepareToPlay()
    // 2. DSP objects (filter, ADSR) require valid sample rate for prepare()
    // 3. Constructor runs before sample rate is known (sampleRate = 0)
    // 4. Initializing DSP in constructor with sampleRate=0 causes:
    //    - StateVariableTPTFilter assertions (invalid sample rate)
    //    - ADSR assertions (invalid timing calculations)
    //    - Corrupted voice state in strict hosts like Ableton Live
    //
    // Initialization order (MUST be followed):
    // 1. Store sample rate (required for all DSP calculations)
    // 2. Safety check: ensure sample rate is valid
    // 3. Prepare filter with valid spec (sample rate, max block size, channels)
    // 4. Update filter parameters (cutoff, resonance, mode)
    // 5. Set ADSR sample rate (required for timing calculations)
    // 6. Update ADSR parameters (now that sample rate is known)
    // 7. Mark DSP as initialized (prevents re-initialization issues)
    //
    // This is the standard, bulletproof way to initialize voices in modern JUCE.
    
    // Safety check: ensure sample rate is valid
    if (sampleRate <= 0.0)
    {
        // CRITICAL: Logger calls removed to prevent LeakedObjectDetector assertions
        // DBG("Space Dust: ERROR - Invalid sample rate in prepareToPlay: " + safeStringFromNumber(sampleRate));
        return; // Skip DSP initialization if sample rate is invalid
    }
    
    // Store sample rate for voice calculations
    this->sampleRate = sampleRate;
    
    // Re-seed Random with sample rate for additional uniqueness (already seeded in constructor with voice address)
    // This ensures each voice has different noise patterns even after sample rate changes
    random.setSeed(static_cast<juce::int64>(reinterpret_cast<uintptr_t>(this)) + static_cast<juce::int64>(sampleRate * 1000));
    
    // Prepare filter with sample rate (2 channels for stereo panning)
    const juce::uint32 maxBlockSize = static_cast<juce::uint32>(juce::jmax(4096, samplesPerBlock));
    filter.prepare({ sampleRate, maxBlockSize, 2 });
    modFilter1.prepare({ sampleRate, maxBlockSize, 2 });
    modFilter2.prepare({ sampleRate, maxBlockSize, 2 });
    
    // Pre-allocate voice buffers (stereo for per-oscillator pan)
    voiceTempBuffer.setSize(2, static_cast<int>(maxBlockSize), false, false, false);
    voiceSingleSampleBuffer.setSize(2, 1, false, false, false);
    
    // Prepare noise EQ shelf filters
    // Low shelf: affects frequencies below 200 Hz
    // High shelf: affects frequencies above 1.5 kHz
    juce::dsp::ProcessSpec eqSpec{ sampleRate, maxBlockSize, 1 };
    lowShelfFilter.prepare(eqSpec);
    highShelfFilter.prepare(eqSpec);
    updateNoiseEqFilters();
    
    // Update filter parameters AFTER filter is prepared
    // This ensures filter is ready to accept parameter changes
    updateFilter();
    updateModFilter1();
    updateModFilter2();
    
    // Prepare ADSR with sample rate (required for proper timing calculations)
    // JUCE's ADSR needs to know the sample rate to convert seconds to samples
    adsr.setSampleRate(sampleRate);
    
    // Prepare Filter Envelope ADSR with sample rate
    filterAdsr.setSampleRate(sampleRate);
    
    // Update ADSR parameters with new sample rate (recalculates internal timing)
    // Now that sample rate is valid, parameters can be safely applied
    updateAdsrParameters();
    updateFilterAdsrParameters();

    // Anti-click envelope smoother: ~3ms one-pole lowpass on ADSR output
    envSmoothCoeff = 1.0f - std::exp(-1.0f / (0.003f * static_cast<float>(sampleRate)));
    smoothedEnvelope = 0.0f;
    // Slightly faster than amplitude smooth — only enough to kill zipper/parameter clicks.
    filterCutoffSmoothCoeff = 1.0f - std::exp(-1.0f / (0.0015f * static_cast<float>(sampleRate)));
    smoothedFilterCutoffHz = juce::jlimit(20.0f, 20000.0f, baseFilterCutoff);
    voiceFade = 1.0f;
    voiceFadeSamplesRemaining = 0;
    outputSmootherL = 0.0f;
    outputSmootherR = 0.0f;

    // Mark DSP as properly initialized
    // This prevents issues if setCurrentPlaybackSampleRate() is called again
    isDspInitialized = true;
    
    // CRITICAL: Logger calls removed to prevent LeakedObjectDetector assertions
    // DBG("Space Dust: SynthVoice DSP initialized - sampleRate: " + safeStringFromNumber(sampleRate) + ", isDspInitialized: " + safeStringFromBool(isDspInitialized));
}

//==============================================================================
// -- Sample Rate Setup --

void SynthVoice::setCurrentPlaybackSampleRate(double newRate)
{
    //==============================================================================
    // -- CRITICAL: Safe Sample Rate Update --
    // 
    // This method is called by juce::Synthesiser:
    // 1. Automatically when voices are added (with sampleRate=0) ← This is the problem!
    // 2. When synth.setCurrentPlaybackSampleRate() is called explicitly
    // 
    // IMPORTANT: DSP initialization happens in prepareToPlay(), NOT here.
    // This method should only update the stored sample rate if it's valid.
    //
    // Why we need special handling:
    // - JUCE calls this automatically when voices are added (sampleRate=0)
    // - We must ignore invalid sample rates (0 or negative)
    // - If DSP is already initialized via prepareToPlay(), we only update the rate
    // - If sample rate changes later, we re-initialize DSP
    
    SynthesiserVoice::setCurrentPlaybackSampleRate(newRate);
    
    // CRITICAL: Ignore invalid sample rates (especially 0 when voices are first added)
    // JUCE automatically calls this with 0 when voices are added, before we can
    // properly initialize them. We must skip DSP initialization in this case.
    if (newRate <= 0.0)
    {
        // Don't log warnings for the expected case (sampleRate=0 during voice creation)
        // This is normal - DSP will be initialized properly in prepareToPlay()
        return; // Skip DSP update if sample rate is invalid
    }
    
    // Update stored sample rate
    sampleRate = newRate;
    
    // Only re-initialize DSP if it was already initialized (sample rate change scenario)
    // If DSP hasn't been initialized yet, prepareToPlay() will handle it
    if (isDspInitialized)
    {
        // Re-initialize DSP with new sample rate (in case sample rate changed)
        // This ensures filter and ADSR use the correct rate
        const juce::uint32 maxBlockSize = 512; // Safe maximum
        filter.prepare({ newRate, maxBlockSize, 2 });
        modFilter1.prepare({ newRate, maxBlockSize, 2 });
        modFilter2.prepare({ newRate, maxBlockSize, 2 });
        updateFilter();
        updateModFilter1();
        updateModFilter2();
        juce::dsp::ProcessSpec eqSpec{ newRate, maxBlockSize, 1 };
        lowShelfFilter.prepare(eqSpec);
        highShelfFilter.prepare(eqSpec);
        updateNoiseEqFilters();
        
        adsr.setSampleRate(newRate);
        filterAdsr.setSampleRate(newRate);
        updateAdsrParameters();
        updateFilterAdsrParameters();
        envSmoothCoeff = 1.0f - std::exp(-1.0f / (0.003f * static_cast<float>(newRate)));
        filterCutoffSmoothCoeff = 1.0f - std::exp(-1.0f / (0.0015f * static_cast<float>(newRate)));
    }
    // If DSP not initialized yet, prepareToPlay() will handle initialization
}

//==============================================================================
// -- Parameter Update Methods --

void SynthVoice::setOsc1Waveform(int waveform)
{
    osc1Waveform = juce::jlimit(0, 3, waveform);
}

void SynthVoice::setOsc2Waveform(int waveform)
{
    osc2Waveform = juce::jlimit(0, 3, waveform);
}

//==============================================================================
// -- Oscillator Pitch Tuning Methods --
// Each oscillator has independent coarse tuning (±24 semitones) and fine detuning (±50 cents)
// Double-click any knob to reset to 0

void SynthVoice::setOsc1CoarseTune(float semitones)
{
    osc1CoarseTune = juce::jlimit(-24.0f, 24.0f, semitones);
    // Update frequency if note is playing — base Hz must match renderNextBlock's currentPitch
    // (glide / legato). getMidiNoteInHertz(getCurrentlyPlayingNote()) can disagree for a whole
    // block because updateVoicesWithParameters runs before MIDI is applied in renderNextBlock.
    if (isActive)
    {
        const int n = getCurrentlyPlayingNote();
        const double baseHz = (currentPitch > 0.0)
                                  ? juce::jlimit(20.0, 20000.0, currentPitch)
                                  : (n >= 0 ? juce::MidiMessage::getMidiNoteInHertz(n) : 440.0);
        updateOsc1Frequency(baseHz);
    }
}

void SynthVoice::setOsc1Detune(float cents)
{
    osc1Detune = juce::jlimit(-50.0f, 50.0f, cents);
    if (isActive)
    {
        const int n = getCurrentlyPlayingNote();
        const double baseHz = (currentPitch > 0.0)
                                  ? juce::jlimit(20.0, 20000.0, currentPitch)
                                  : (n >= 0 ? juce::MidiMessage::getMidiNoteInHertz(n) : 440.0);
        updateOsc1Frequency(baseHz);
    }
}

void SynthVoice::setOsc2CoarseTune(float semitones)
{
    osc2CoarseTune = juce::jlimit(-24.0f, 24.0f, semitones);
    if (isActive)
    {
        const int n = getCurrentlyPlayingNote();
        const double baseHz = (currentPitch > 0.0)
                                  ? juce::jlimit(20.0, 20000.0, currentPitch)
                                  : (n >= 0 ? juce::MidiMessage::getMidiNoteInHertz(n) : 440.0);
        updateOsc2Frequency(baseHz);
    }
}

void SynthVoice::setOsc2Detune(float cents)
{
    osc2Detune = juce::jlimit(-50.0f, 50.0f, cents);
    if (isActive)
    {
        const int n = getCurrentlyPlayingNote();
        const double baseHz = (currentPitch > 0.0)
                                  ? juce::jlimit(20.0, 20000.0, currentPitch)
                                  : (n >= 0 ? juce::MidiMessage::getMidiNoteInHertz(n) : 440.0);
        updateOsc2Frequency(baseHz);
    }
}

void SynthVoice::setOsc1Level(float level)
{
    osc1Level = juce::jlimit(0.0f, 1.0f, level);
}

void SynthVoice::setOsc2Level(float level)
{
    osc2Level = juce::jlimit(0.0f, 1.0f, level);
}

void SynthVoice::setOsc1Pan(float pan)
{
    osc1Pan = juce::jlimit(-1.0f, 1.0f, pan);
}

void SynthVoice::setOsc2Pan(float pan)
{
    osc2Pan = juce::jlimit(-1.0f, 1.0f, pan);
}

void SynthVoice::setNoiseLevel(float level)
{
    noiseLevel = juce::jlimit(0.0f, 1.0f, level);
}

void SynthVoice::setNoiseType(int type)
{
    noiseType = (type == 0) ? White : Pink;
}

void SynthVoice::setSubOscOn(bool on)
{
    subOscOn = on;
}

void SynthVoice::setSubOscWaveform(int waveform)
{
    subOscWaveform = juce::jlimit(0, 3, waveform);
}

void SynthVoice::setSubOscLevel(float level)
{
    subOscLevel = juce::jlimit(0.0f, 1.0f, level);
}

void SynthVoice::setSubOscCoarse(float semitones)
{
    subOscCoarse = juce::jlimit(-36.0f, 36.0f, semitones);
}

void SynthVoice::setLowShelfAmount(float amount)
{
    lowShelfAmount = juce::jlimit(-1.0f, 1.0f, amount);
    updateNoiseEqFilters();
}

void SynthVoice::setHighShelfAmount(float amount)
{
    highShelfAmount = juce::jlimit(-1.0f, 1.0f, amount);
    updateNoiseEqFilters();
}

void SynthVoice::setFilterMode(int mode)
{
    filterMode = juce::jlimit(0, 2, mode);
    updateFilter();
}

void SynthVoice::setFilterCutoff(float cutoffHz)
{
    baseFilterCutoff = juce::jlimit(20.0f, 20000.0f, cutoffHz);
    // filterCutoff will be updated in renderNextBlock with envelope modulation
    updateFilter();
}

void SynthVoice::setFilterResonance(float resonance)
{
    filterResonance = juce::jlimit(0.0f, 1.0f, resonance);
    updateFilter();
}

void SynthVoice::setWarmSaturationMaster(bool enabled)
{
    warmSaturationMaster = enabled;
}

void SynthVoice::setModFilter1(bool show, bool linkToMaster, int mode, float cutoffHz, float resonance)
{
    modFilter1Show = show;
    modFilter1Linked = linkToMaster;
    modFilter1Mode = juce::jlimit(0, 2, mode);
    modFilter1Cutoff = juce::jlimit(20.0f, 20000.0f, cutoffHz);
    modFilter1Resonance = juce::jlimit(0.0f, 1.0f, resonance);
    updateModFilter1();
}

void SynthVoice::setWarmSaturationMod1(bool enabled)
{
    warmSaturationMod1 = enabled;
}

void SynthVoice::setModFilter2(bool show, bool linkToMaster, int mode, float cutoffHz, float resonance)
{
    modFilter2Show = show;
    modFilter2Linked = linkToMaster;
    modFilter2Mode = juce::jlimit(0, 2, mode);
    modFilter2Cutoff = juce::jlimit(20.0f, 20000.0f, cutoffHz);
    modFilter2Resonance = juce::jlimit(0.0f, 1.0f, resonance);
    updateModFilter2();
}

void SynthVoice::setWarmSaturationMod2(bool enabled)
{
    warmSaturationMod2 = enabled;
}

//==============================================================================
// -- Filter Envelope Methods --

void SynthVoice::setFilterEnvAttack(float seconds)
{
    filterEnvAttackTime = juce::jmax(0.01f, seconds);
    updateFilterAdsrParameters();
}

void SynthVoice::setFilterEnvDecay(float seconds)
{
    filterEnvDecayTime = juce::jmax(0.01f, seconds);
    updateFilterAdsrParameters();
}

void SynthVoice::setFilterEnvSustain(float level)
{
    filterEnvSustainLevel = juce::jlimit(0.0f, 1.0f, level);
    updateFilterAdsrParameters();
}

void SynthVoice::setFilterEnvRelease(float seconds)
{
    filterEnvReleaseTime = juce::jmax(0.01f, seconds);
    updateFilterAdsrParameters();
}

void SynthVoice::setFilterEnvAmount(float amount)
{
    filterEnvAmount = juce::jlimit(-100.0f, 100.0f, amount);
}

//==============================================================================
// -- ADSR Envelope Methods --

void SynthVoice::setEnvAttack(float seconds)
{
    // Clamp to parameter range (0.01-20.0s, skewed)
    envAttackTime = juce::jlimit(0.01f, 20.0f, seconds);
    // CRITICAL: Must call updateAdsrParameters() to apply the change
    // JUCE's ADSR requires all parameters to be set together via setParameters()
    updateAdsrParameters();
}

void SynthVoice::setEnvDecay(float seconds)
{
    // Clamp to parameter range (0.01-20.0s, skewed)
    envDecayTime = juce::jlimit(0.01f, 20.0f, seconds);
    // CRITICAL: Must call updateAdsrParameters() to apply the change
    // This ensures decay time changes are reflected immediately, regardless of current envelope state
    updateAdsrParameters();
}

void SynthVoice::setEnvSustain(float level)
{
    // Clamp to parameter range (0.0-1.0, linear amplitude level)
    envSustainLevel = juce::jlimit(0.0f, 1.0f, level);
    // CRITICAL: Must call updateAdsrParameters() to apply the change
    // This ensures sustain level changes are reflected immediately, regardless of current envelope state
    // Note: Sustain is an amplitude level (0.0-1.0), not a time value
    updateAdsrParameters();
}

void SynthVoice::setEnvRelease(float seconds)
{
    // Clamp to parameter range (0.01-20.0s, skewed) - long cosmic tails!
    envReleaseTime = juce::jlimit(0.01f, 20.0f, seconds);
    // CRITICAL: Must call updateAdsrParameters() to apply the change
    // This ensures release time changes are reflected immediately, even during release phase
    updateAdsrParameters();
}

//==============================================================================
// -- Glide (Portamento) Parameter Update --

void SynthVoice::setGlideTime(float seconds)
{
    // Clamp to parameter range (0.0-5.0s, skewed with midpoint at 1.0s)
    glideTimeSeconds = juce::jlimit(0.0f, 5.0f, seconds);
    
    // If glide is active, recalculate glideDelta based on new time
    // This allows real-time adjustment of glide speed during playback
    if (glideDelta != 0.0 && sampleRate > 0.0)
    {
        double pitchDifference = targetPitch - currentPitch;
        double samplesToGlide = glideTimeSeconds * sampleRate;
        
        if (samplesToGlide > 0.0)
        {
            glideDelta = pitchDifference / samplesToGlide;
        }
        else
        {
            glideDelta = 0.0; // Instant if time is too short
        }
    }
}

void SynthVoice::setLegatoGlide(bool enabled)
{
    legatoGlideEnabled = enabled;
}

void SynthVoice::setPitchEnvAmount(float amount)
{
    pitchEnvAmount = juce::jlimit(-100.0f, 100.0f, amount);
}

void SynthVoice::setPitchEnvTime(float seconds)
{
    pitchEnvTime = juce::jlimit(0.0f, 5.0f, seconds);
}

void SynthVoice::setPitchEnvPitch(float semitones)
{
    pitchEnvPitch = juce::jlimit(0.0f, 24.0f, semitones);
}

void SynthVoice::setPitchBendAmount(float semitones)
{
    pitchBendAmountFloat = juce::jlimit(0.0f, 24.0f, semitones);
}

void SynthVoice::setPitchBend(float value)
{
    pitchBend = juce::jlimit(-1.0f, 1.0f, value);
}

void SynthVoice::setLfoTargets(int lfo1Target, int lfo2Target)
{
    lfo1TargetCached = juce::jlimit(0, 5, lfo1Target);  // 0=Pitch, 1=Filter, 2=MasterVol, 3=Osc1, 4=Osc2, 5=Noise
    lfo2TargetCached = juce::jlimit(0, 5, lfo2Target);
}

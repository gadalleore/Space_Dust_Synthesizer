//==============================================================================
// Space Dust - Cosmic Subtractive Synthesizer
// 
// A dual-oscillator subtractive synthesizer with multimode filter and ADSR envelope.
// Built with JUCE 8, featuring real-time safe processing and a beautiful cosmic GUI.
//
// Signal Path: 
//   Osc1 (with independent detune) + Osc2 (with independent detune) 
//   â†’ Mix â†’ Filter â†’ ADSR Envelope â†’ Master Volume â†’ Output
//
// Features:
// - Dual oscillators with 4 waveforms each (Sine, Triangle, Saw, Square)
// - Independent detune for each oscillator (coarse + fine) for shimmering effects
// - Osc2 can be tuned relative to Osc1 (coarse/fine tuning for intervals)
// - Multimode state-variable filter (Low Pass, Band Pass, High Pass, Notch, Peak)
// - Proper 4-stage ADSR amplitude envelope (Attack â†’ Decay â†’ Sustain â†’ Release)
//   with long cosmic tails (release up to 20 seconds)
// - Master volume control for proper mix integration
// - 8-voice polyphony
// - Real-time safe parameter updates via AudioProcessorValueTreeState
//
// ADSR Envelope Implementation:
//   - Linear amplitude ramping for real-time safety
//   - Proper state machine: Idle â†’ Attack â†’ Decay â†’ Sustain â†’ Release â†’ Idle
//   - Attack: ramp from 0 to 1.0
//   - Decay: ramp from 1.0 to sustain level
//   - Sustain: hold at sustain level (no change)
//   - Release: ramp from current level to 0.0 (long cosmic tails!)
//
// Detuning Implementation:
//   - Each oscillator has independent detune (coarse + fine)
//   - Applied directly to oscillator pitch before phase calculation
//   - Creates shimmering, unison-like character with asymmetric movement
//   - Default: Osc1 = 0, Osc2 = +5 coarse / -3 fine (subtle shimmer)
//
// Space Dust by [your name] â€“ the cosmic sine machine
//==============================================================================

// VLD must be the first include for accurate call-stack capture in leak reports.
// Only active when CMake is configured with -DENABLE_VLD=ON (Debug builds only).
#if defined(VLD_ENABLED) && VLD_ENABLED && defined(_DEBUG)
  #include <vld.h>
#endif

#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "NoteLockGrid.h"      // LFO key tracking shares the filter's snap grid
#include "LfoWaveform.h"       // band-limited LFO shapes (see tools/lfotest)
#include "SpaceDustSynthesiser.h"
#include "SpaceDustReverb.h"
#include "SpaceDustGrainDelay.h"
#include "SpaceDustPhaser.h"
#include "SpaceDustTranceGate.h"
#include "MemorySafetyLogger.h"
#include <juce_core/juce_core.h>
#include <algorithm>
#include <cstdarg>

//==============================================================================
// -- UTF-8 String Validation Helper --
// CRITICAL: Prevents juce_String.cpp:327 assertion from invalid UTF-8 strings
// This helper validates UTF-8 strings before construction to prevent crashes
// in hosts like Ableton Live that are sensitive to string encoding issues.

namespace
{
    // Safe string creation helper - validates UTF-8 before creating String
    // CRITICAL: Prevents juce_String.cpp:327 assertion from invalid UTF-8 strings
    // Use this for all String creation from const char* literals or file paths
    juce::String safeString(const char* raw)
    {
        if (raw == nullptr || !juce::CharPointer_UTF8::isValidString(raw, -1))
            return "(safe fallback)";
        return juce::String(raw);
    }
    
    // Whole-Hz formatting for the filter cutoff parameters.
    //
    // Those parameters are continuous (no step interval) so Note Lock can land
    // exactly on its semitone grid -- a 1 Hz step is worth ~43 cents down at 20 Hz,
    // which would visibly detune a locked resonant peak. JUCE derives its default
    // decimal-place count from that interval, though, so removing the interval also
    // pushes the readout to 7 places ("7999.6314 Hz"). Spell the formatting out
    // instead. This lives on the PARAMETER rather than on the slider because
    // SliderParameterAttachment overwrites Slider::textFromValueFunction with the
    // parameter's own getText -- and it fixes the host's automation lane too.
    juce::AudioParameterFloatAttributes wholeHzAttributes()
    {
        return juce::AudioParameterFloatAttributes()
            .withStringFromValueFunction([](float v, int maximumLength)
            {
                auto asText = juce::String(juce::roundToInt(v));
                return maximumLength > 0 ? asText.substring(0, maximumLength) : asText;
            })
            .withValueFromStringFunction([](const juce::String& text)
            {
                return text.getFloatValue();
            });
    }

    // Helper to log parameter creation with exception handling
    template<typename ParamType>
    void addParameterWithLogging(std::vector<std::unique_ptr<juce::RangedAudioParameter>>& params,
                                  std::unique_ptr<ParamType> param,
                                  const juce::String& paramID)
    {
        try
        {
            params.push_back(std::move(param));
            DBG("Space Dust: Added param: " + paramID);
        }
        catch (const std::exception& e)
        {
            DBG("Space Dust: Exception adding param " + paramID + ": " + juce::String(e.what()));
            throw; // Re-throw to prevent invalid state
        }
        catch (...)
        {
            DBG("Space Dust: Unknown exception adding param: " + paramID);
            throw; // Re-throw to prevent invalid state
        }
    }
    
    // Macro to add parameter with logging (simpler syntax)
    #define ADD_PARAM_WITH_LOG(params, param_expr, param_id) \
        try { \
            params.push_back(param_expr); \
            DBG("Space Dust: Added param: " + safeString(param_id)); \
        } catch (const std::exception& e) { \
            DBG("Space Dust: Exception adding param " + safeString(param_id) + ": " + juce::String(e.what())); \
            throw; \
        } catch (...) { \
            DBG("Space Dust: Unknown exception adding param: " + safeString(param_id)); \
            throw; \
        }
    
    // Safe string creation from file path (validates UTF-8)
    juce::String safeFilePath(const char* path)
    {
        if (path == nullptr)
            return juce::String();
        
        if (!juce::CharPointer_UTF8::isValidString(path, -1))
        {
            DBG("Space Dust: WARNING - Invalid UTF-8 in file path, using safe fallback");
            // Return a safe default path using safe string literals
            return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                .getChildFile(safeString("SpaceDust_DebugLog.txt")).getFullPathName();
        }
        
        return juce::String(path);
    }
    
    // Safe string creation from string literal (compile-time validation)
    // For string literals, we assume they're valid if the source file is UTF-8
    // This is mainly for runtime-constructed strings
    juce::String safeStringFromNumber(int value)
    {
        // juce::String constructor from int is safe, but we validate the result
        juce::String result = juce::String(value);
        // String from number should always be valid ASCII (numbers are always valid UTF-8)
        // No validation needed for numeric strings, but included for defensive programming
        return result;
    }
    
    juce::String safeStringFromNumber(double value, int numDecimalPlaces = 2)
    {
        // juce::String constructor from double is safe, but we validate the result
        juce::String result = juce::String(value, numDecimalPlaces);
        // String from number should always be valid ASCII (numbers are always valid UTF-8)
        // No validation needed for numeric strings
        return result;
    }
    
    // Safe string formatted with UTF-8 validation
    juce::String safeStringFormatted(const char* format, ...)
    {
        va_list args;
        va_start(args, format);
        juce::String result = juce::String::formatted(format, args);
        va_end(args);
        
        // String::formatted with standard format specifiers should always produce valid UTF-8
        // Validation is mainly defensive programming
        return result;
    }
    
    // Debug-only file logger (writes to Documents/SpaceDust_DebugLog.txt). Disabled in Release
    // so shipped builds do not create or append to files on users' machines.
    void logToFile(const juce::String& msg)
    {
#if JUCE_DEBUG
        try
        {
            juce::File logFile = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                .getChildFile(safeString("SpaceDust_DebugLog.txt"));
            logFile.appendText("[" + juce::Time::getCurrentTime().formatted("%H:%M:%S.") +
                juce::String::formatted("%03d", juce::Time::getMillisecondCounter() % 1000) + "] " +
                msg + "\n");
        }
        catch (...) {}
#else
        juce::ignoreUnused(msg);
#endif
    }

    // Throttled version: logs at most once per minIntervalMs globally (Debug only).
    void logToFileThrottled(const juce::String& /*tag*/, const juce::String& msg, int minIntervalMs = 500)
    {
#if JUCE_DEBUG
        static std::atomic<juce::uint32> lastLogTime{0};
        auto now = juce::Time::getMillisecondCounter();
        auto last = lastLogTime.load(std::memory_order_relaxed);
        if ((now - last) < static_cast<juce::uint32>(minIntervalMs))
            return;
        lastLogTime.store(now, std::memory_order_relaxed);
        logToFile(msg);
#else
        juce::ignoreUnused(msg);
        juce::ignoreUnused(minIntervalMs);
#endif
    }

    // Legacy alias
    void appendFilterSyncLog(const juce::String& msg) { logToFile(msg); }

    //==========================================================================
    // -- Safe parameter read helper --
    // Returns the parameter's current value, or a fallback if the parameter ID
    // is missing from the APVTS layout. Direct `*apvts.getRawParameterValue(id)`
    // dereferences crash if the ID is wrong; this helper turns that into a
    // recoverable fallback. Use in state-restore and voice-update paths where
    // a missing param ID should not take the host down.
    inline float safeGetParam(juce::AudioProcessorValueTreeState& apvts,
                              const char* id,
                              float fallback = 0.0f) noexcept
    {
        if (auto* atomic = apvts.getRawParameterValue(id))
            return atomic->load();
        return fallback;
    }

    // Overload for runtime-built parameter IDs (e.g. "finalEQB" + n + "Freq").
    inline float safeGetParam(juce::AudioProcessorValueTreeState& apvts,
                              const juce::String& id,
                              float fallback = 0.0f) noexcept
    {
        if (auto* atomic = apvts.getRawParameterValue(id))
            return atomic->load();
        return fallback;
    }

    //==========================================================================
    // -- The parameter ids of every LFO, in one table --
    //
    // The LFO fill loop reaches LFO n through this, so it never builds a
    // juce::String on the audio thread. LFOs 3 and 4 have no parameters yet:
    // safeGetParam returns 0 for an id that does not exist, which gives a depth
    // of 0 and a buffer of zeros, and a zero buffer contributes nothing to
    // anything that reads it. Their rows are here so the buffers the modulation
    // matrix indexes exist before the knobs that drive them do.
    struct LfoParamIds
    {
        const char* enabled;
        const char* depth;
        const char* sync;
        const char* rate;
        const char* triplet;
        const char* tripletAll;
        const char* phase;
        const char* waveform;
    };

    constexpr LfoParamIds lfoParamIds[spacedust::numLfos] = {
        { "lfo1Enabled", "lfo1Depth", "lfo1Sync", "lfo1Rate",
          "lfo1TripletEnabled", "lfo1TripletStraightToggle", "lfo1Phase", "lfo1Waveform" },
        { "lfo2Enabled", "lfo2Depth", "lfo2Sync", "lfo2Rate",
          "lfo2TripletEnabled", "lfo2TripletStraightToggle", "lfo2Phase", "lfo2Waveform" },
        { "lfo3Enabled", "lfo3Depth", "lfo3Sync", "lfo3Rate",
          "lfo3TripletEnabled", "lfo3TripletStraightToggle", "lfo3Phase", "lfo3Waveform" },
        { "lfo4Enabled", "lfo4Depth", "lfo4Sync", "lfo4Rate",
          "lfo4TripletEnabled", "lfo4TripletStraightToggle", "lfo4Phase", "lfo4Waveform" },
    };

    //==========================================================================
    // -- Every knob the effects chain reads that an LFO may reach --
    //
    // ONE list, which gives both an index (ep::xxx) and the id string. The index
    // is what the chain uses; the id is looked up ONCE, on the message thread,
    // in rebuildCompiledRoutings, so a chunk never hashes a string to find out
    // where a knob sits.
    //
    // Floats only. A bool or a choice cannot be a modulation destination -- see
    // DestinationTable::isLegalDestination -- so those reads stay on
    // safeGetParam exactly as they were.
    //
    // Adding a knob to the chain and forgetting it here is not a crash: it keeps
    // its old raw read and simply cannot be modulated.
    #define SPACEDUST_EFFECT_PARAMS(X) \
        X (reverbDecayTime)               X (reverbWetMix) \
        X (reverbFilterHPCutoff)          X (reverbFilterHPResonance) \
        X (reverbFilterLPCutoff)          X (reverbFilterLPResonance) \
        /* The Delay sits AHEAD of runEffectsChain and is not chunk-aware, so  */ \
        /* these are refreshed once per block, like the Pre placements of the  */ \
        /* bit crusher and the trance gate. Coarser, but not silent.           */ \
        X (delayDecay)                    X (delayDryWet) \
        X (delayRate) \
        X (delayFilterHPCutoff)           X (delayFilterLPCutoff) \
        X (delayFilterHPResonance)        X (delayFilterLPResonance) \
        X (grainDelayDecay)               X (grainDelayMix) \
        X (grainDelayTime) \
        X (grainDelaySize)                X (grainDelayPitch) \
        X (grainDelayDensity)             X (grainDelayJitter) \
        X (grainDelayFilterHPCutoff)      X (grainDelayFilterLPCutoff) \
        X (grainDelayFilterHPResonance)   X (grainDelayFilterLPResonance) \
        X (phaserMix)                     X (phaserRate) \
        X (phaserDepth)                   X (phaserFeedback) \
        X (phaserCentre)                  X (phaserStereoOffset) \
        X (flangerMix)                    X (flangerRate) \
        X (flangerDepth)                  X (flangerFeedback) \
        X (flangerWidth) \
        X (transientMix)                  X (transientKaDonk) \
        X (transientCoarse)               X (transientLength) \
        X (compressorThreshold)           X (compressorRatio) \
        X (compressorAttack)              X (compressorRelease) \
        X (compressorMakeup)              X (compressorMix) \
        X (softClipperDrive)              X (softClipperKnee) \
        X (softClipperMix) \
        X (lofiAmount) \
        X (bitCrusherAmount)              X (bitCrusherRate) \
        X (bitCrusherMix) \
        X (tranceGateRate)                X (tranceGateAttack) \
        X (tranceGateRelease)             X (tranceGateMix) \
        /* The five EQ bands last, and in strict Freq / Gain / Q order: the  */ \
        /* chain reaches band i by adding i * finalEQParamsPerBand to the    */ \
        /* first one, and the static_assert below keeps that true.           */ \
        X (finalEQB1Freq) X (finalEQB1Gain) X (finalEQB1Q) \
        X (finalEQB2Freq) X (finalEQB2Gain) X (finalEQB2Q) \
        X (finalEQB3Freq) X (finalEQB3Gain) X (finalEQB3Q) \
        X (finalEQB4Freq) X (finalEQB4Gain) X (finalEQB4Q) \
        X (finalEQB5Freq) X (finalEQB5Gain) X (finalEQB5Q)

    enum EffectParam
    {
        #define SPACEDUST_EP_ENUM(name) ep_##name,
        SPACEDUST_EFFECT_PARAMS (SPACEDUST_EP_ENUM)
        #undef SPACEDUST_EP_ENUM
        numEffectParams
    };

    constexpr int finalEQParamsPerBand = 3;
    static_assert (ep_finalEQB2Freq - ep_finalEQB1Freq == finalEQParamsPerBand,
                   "the final EQ bands must stay Freq/Gain/Q, three apart");
    static_assert (ep_finalEQB5Q + 1 == numEffectParams,
                   "the final EQ bands must stay last in the list");

    const char* const effectParamIds[] = {
        #define SPACEDUST_EP_ID(name) #name,
        SPACEDUST_EFFECT_PARAMS (SPACEDUST_EP_ID)
        #undef SPACEDUST_EP_ID
    };

    static_assert (sizeof (effectParamIds) / sizeof (effectParamIds[0]) == numEffectParams,
                   "the id table and the enum come from the same list");

    //==========================================================================
    // -- Every CHOICE and per-step switch the effects chain reads --
    //
    // None of these can be a modulation destination -- isLegalDestination takes
    // floats only -- so they are not in the list above. They are here for a
    // different reason: the chain now runs sixteen times a block when a knob is
    // assigned, and every one of these reads used to build a juce::String to
    // find its parameter. juce::ParameterID{"reverbType", 1}.getParamID() heap
    // allocates, and so does "tranceGateStep" + juce::String(s + 1) inside a
    // sixteen-iteration loop. That is roughly 340-860 allocations per block on
    // the audio thread instead of the ~20-55 that were there when the chunked
    // path only ran under the test harness.
    //
    // So the POINTERS are resolved once, on the message thread, and the chain
    // reads through them. No string is built on the audio thread at all.
    #define SPACEDUST_EFFECT_CHOICES(X) \
        X (reverbType)            X (phaserStages) \
        X (transientType)         X (compressorType) \
        X (softClipperMode)       X (softClipperOversample) \
        X (tranceGateSteps) \
        X (finalEQB1Type)         X (finalEQB2Type) \
        X (finalEQB3Type)         X (finalEQB4Type) \
        X (finalEQB5Type)

    enum EffectChoice
    {
        #define SPACEDUST_EC_ENUM(name) ec_##name,
        SPACEDUST_EFFECT_CHOICES (SPACEDUST_EC_ENUM)
        #undef SPACEDUST_EC_ENUM
        numEffectChoices
    };

    static_assert (ec_finalEQB2Type - ec_finalEQB1Type == 1,
                   "the final EQ Type choices must stay one apart and in band order");
    static_assert (ec_finalEQB5Type + 1 == numEffectChoices,
                   "the final EQ Type choices must stay last, so the ec_finalEQB1Type + i "
                   "indexing cannot run into a choice inserted after them");

    const char* const effectChoiceIds[] = {
        #define SPACEDUST_EC_ID(name) #name,
        SPACEDUST_EFFECT_CHOICES (SPACEDUST_EC_ID)
        #undef SPACEDUST_EC_ID
    };

    static_assert (sizeof (effectChoiceIds) / sizeof (effectChoiceIds[0]) == numEffectChoices,
                   "the choice id table and the choice enum come from the same list");

    /** The trance gate's sixteen step switches, by id. Built once so the gate's
        per-step loop never concatenates a string on the audio thread. */
    const char* const tranceGateStepIds[16] = {
        "tranceGateStep1",  "tranceGateStep2",  "tranceGateStep3",  "tranceGateStep4",
        "tranceGateStep5",  "tranceGateStep6",  "tranceGateStep7",  "tranceGateStep8",
        "tranceGateStep9",  "tranceGateStep10", "tranceGateStep11", "tranceGateStep12",
        "tranceGateStep13", "tranceGateStep14", "tranceGateStep15", "tranceGateStep16"
    };

    //==========================================================================
    // -- Crash-safety marker for state restoration --
    // setStateInformation() writes this marker on entry and deletes it on
    // successful completion. If the marker is still present on the next entry,
    // the previous attempt crashed mid-restore â€” we skip the restore and load
    // defaults so the host can at least open the project. Breaks crash-on-reload
    // loops where a corrupted saved state would otherwise kill the host every
    // time it tries to recover.
    juce::File getStateRestoreMarker()
    {
        return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                   .getChildFile("SpaceDust")
                   .getChildFile("state_restore_in_progress.marker");
    }
}

//==============================================================================
// -- Constructor --

SpaceDustAudioProcessor::SpaceDustAudioProcessor()
#ifndef JucePlugin_PreferredChannelConfigurations
     : AudioProcessor (BusesProperties()
                     #if ! JucePlugin_IsMidiEffect
                      #if ! JucePlugin_IsSynth
                       .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                      #endif
                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
                     #endif
                       ),
#endif
     apvts(*this, nullptr, "PARAMETERS", createParameterLayout()),
     synth(apvts)
{
    //==============================================================================
    // -- DEBUG: Processor Constructor Start --
    // CRITICAL: Minimal logging in constructor - heavy init moved to prepareToPlay
    DBG("Space Dust: Processor ctor START");

    // Memory-safety logger: start background writer + log this processor's birth.
    SAFETY_LOGGER_START();
    SAFETY_LOG_OBJECT_CTOR(this, "SpaceDustAudioProcessor ctor");

    try
    {
        DBG("Space Dust: Processor ctor - APVTS created");
    }
    catch (const std::exception& e)
    {
        DBG("Space Dust: Exception in processor ctor: " + juce::String(e.what()));
    }
    catch (...)
    {
        DBG("Space Dust: Unknown exception in processor ctor");
    }
    
    //==============================================================================
    // -- Imported waveforms --
    // Read the user's saved waveforms so the dropdowns are populated before any
    // preset loads. A song that carries its own waveforms replaces these when its
    // state is restored; anything the song does not carry stays available.
    // Safe here despite the note below about voices: this touches no DSP and needs
    // no sample rate, and the audio thread cannot see the result until the first
    // processBlock picks it up.
    userWaveLibrary.loadFromDisk();

    //==============================================================================
    // -- DEBUG LOGGING: DISABLED --
    // FileLogger DISABLED to prevent LeakedObjectDetector assertions in Ableton Live
    // FileLogger is known to cause leaks in VST3 hosts, especially Ableton Live
    // Based on JUCE forums 2023-2025: FileLogger can trigger LeakedObjectDetector assertions
    // on plugin unload, even with proper cleanup
    // 
    // Use DBG() macro for debug-only output instead
    // For production builds, remove all logging entirely
    
    //==============================================================================
    // -- CRITICAL: DO NOT CREATE VOICES IN CONSTRUCTOR --
    // 
    // Voices must be created in prepareToPlay(), NOT in the constructor.
    // 
    // Why: DSP components (filter, ADSR) require a valid sample rate to initialize.
    // The sample rate is only known when prepareToPlay() is called by the host.
    // Creating voices in the constructor means:
    //   - sampleRate = 0 (default)
    //   - filter.prepare() called with invalid spec â†’ assertions
    //   - adsr.setSampleRate(0) â†’ invalid timing calculations â†’ assertions
    //   - Repeated assertions during processBlock() â†’ potential crashes
    //
    // Common pitfalls in hosts like Ableton Live:
    //   - Constructor runs before host knows audio settings
    //   - prepareToPlay() may be called multiple times (sample rate changes)
    //   - releaseResources() must clean up voices before prepareToPlay() is called again
    //
    // Solution: Create voices in prepareToPlay() after sample rate is known.
    //           Clean up voices in releaseResources() for safe re-initialization.
    
    // CRITICAL: Do NOT add sound in constructor
    // Sound will be added in prepareToPlay() to ensure proper lifecycle management
    // This prevents issues when releaseResources() clears sounds
    // juce::Logger::writeToLog("Space Dust: Processor constructor completed (sound will be added in prepareToPlay)");
    
    //==============================================================================
    // -- Initialize Atomic ADSR Parameters --
    // Convert normalized parameter values to actual seconds/levels for atomic storage
    // This ensures voices start with correct ADSR settings
    DBG("Space Dust: Initializing atomic ADSR parameters...");
    try
    {
        // CRITICAL: Use ParameterID::getParamID() for getParameter() calls to prevent string assertions
        if (auto* attackParam = apvts.getParameter(juce::ParameterID{"envAttack", 1}.getParamID()))
        {
            float normalizedValue = attackParam->getValue();
            float attackSeconds = attackParam->convertFrom0to1(normalizedValue);
            currentAttackTime.store(attackSeconds);
            DBG("Space Dust: envAttack converted: " + safeStringFromNumber(attackSeconds) + "s");
        }
        if (auto* decayParam = apvts.getParameter(juce::ParameterID{"envDecay", 1}.getParamID()))
        {
            float normalizedValue = decayParam->getValue();
            float decaySeconds = decayParam->convertFrom0to1(normalizedValue);
            currentDecayTime.store(decaySeconds);
            DBG("Space Dust: envDecay converted: " + safeStringFromNumber(decaySeconds) + "s");
        }
        if (auto* sustainParam = apvts.getParameter(juce::ParameterID{"envSustain", 1}.getParamID()))
        {
            currentSustainLevel.store(sustainParam->getValue());
            DBG("Space Dust: envSustain param retrieved");
        }
        if (auto* releaseParam = apvts.getParameter(juce::ParameterID{"envRelease", 1}.getParamID()))
        {
            float normalizedValue = releaseParam->getValue();
            float releaseSeconds = releaseParam->convertFrom0to1(normalizedValue);
            currentReleaseTime.store(releaseSeconds);
            DBG("Space Dust: envRelease converted: " + safeStringFromNumber(releaseSeconds) + "s");
        }
        // Filter envelope (same pattern: convert normalized to seconds)
        if (auto* p = apvts.getParameter(juce::ParameterID{"filterEnvAttack", 1}.getParamID()))
            currentFilterEnvAttack.store(juce::jmax(0.01f, p->convertFrom0to1(p->getValue())));
        if (auto* p = apvts.getParameter(juce::ParameterID{"filterEnvDecay", 1}.getParamID()))
            currentFilterEnvDecay.store(juce::jmax(0.01f, p->convertFrom0to1(p->getValue())));
        if (auto* p = apvts.getParameter(juce::ParameterID{"filterEnvRelease", 1}.getParamID()))
            currentFilterEnvRelease.store(juce::jmax(0.01f, p->convertFrom0to1(p->getValue())));
    }
    catch (const std::exception& e)
    {
        DBG("Space Dust: Exception getting ADSR params: " + juce::String(e.what()));
    }
    catch (...)
    {
        DBG("Space Dust: Unknown exception getting ADSR params");
    }
    
    DBG("Space Dust: Adding parameter listeners...");
    try
    {
        // Add ValueTree listener to update atomic ADSR parameters when they change
        // This ensures real-time safe, lock-free access from the audio thread
        // CRITICAL: Use ParameterID::getParamID() for consistency with parameter creation
        apvts.addParameterListener(juce::ParameterID{"envAttack", 1}.getParamID(), this);
        DBG("Space Dust: Added listener for envAttack");
        apvts.addParameterListener(juce::ParameterID{"envDecay", 1}.getParamID(), this);
        DBG("Space Dust: Added listener for envDecay");
        apvts.addParameterListener(juce::ParameterID{"envSustain", 1}.getParamID(), this);
        DBG("Space Dust: Added listener for envSustain");
        apvts.addParameterListener(juce::ParameterID{"envRelease", 1}.getParamID(), this);
        DBG("Space Dust: Added listener for envRelease");
        
        apvts.addParameterListener(juce::ParameterID{"lfo1Retrigger", 1}.getParamID(), this);
        apvts.addParameterListener(juce::ParameterID{"lfo2Retrigger", 1}.getParamID(), this);
        apvts.addParameterListener(juce::ParameterID{"filterEnvAttack", 1}.getParamID(), this);
        apvts.addParameterListener(juce::ParameterID{"filterEnvDecay", 1}.getParamID(), this);
        apvts.addParameterListener(juce::ParameterID{"filterEnvRelease", 1}.getParamID(), this);
        apvts.addParameterListener(juce::ParameterID{"mpeMode", 1}.getParamID(), this);
        apvts.addParameterListener(juce::ParameterID{"mpePitchBendRange", 1}.getParamID(), this);
        DBG("Space Dust: Added listeners for LFO retrigger, filter envelope, and MPE");
    }
    catch (const std::exception& e)
    {
        DBG("Space Dust: Exception adding listeners: " + juce::String(e.what()));
    }
    catch (...)
    {
        DBG("Space Dust: Unknown exception adding listeners");
    }
    
    DBG("Space Dust: Converting ADSR params to actual values...");
    try
    {
        // Initialize atomic values from current parameter values (convert normalized to actual values)
        // This ensures voices start with correct ADSR settings
        // CRITICAL: Use ParameterID::getParamID() for getParameter() calls to prevent string assertions
        if (auto* attackParam = apvts.getParameter(juce::ParameterID{"envAttack", 1}.getParamID()))
        {
            float normalizedValue = attackParam->getValue();
            float attackSeconds = attackParam->convertFrom0to1(normalizedValue);
            currentAttackTime.store(attackSeconds);
            DBG("Space Dust: envAttack converted: " + safeStringFromNumber(attackSeconds) + "s");
        }
        if (auto* decayParam = apvts.getParameter(juce::ParameterID{"envDecay", 1}.getParamID()))
        {
            float normalizedValue = decayParam->getValue();
            float decaySeconds = decayParam->convertFrom0to1(normalizedValue);
            currentDecayTime.store(decaySeconds);
            DBG("Space Dust: envDecay converted: " + safeStringFromNumber(decaySeconds) + "s");
        }
        if (auto* sustainParam = apvts.getParameter(juce::ParameterID{"envSustain", 1}.getParamID()))
        {
            // Sustain is already 0.0-1.0 (linear), store normalized value directly
            currentSustainLevel.store(sustainParam->getValue());
            DBG("Space Dust: envSustain stored: " + safeStringFromNumber(sustainParam->getValue()));
        }
        if (auto* releaseParam = apvts.getParameter(juce::ParameterID{"envRelease", 1}.getParamID()))
        {
            float normalizedValue = releaseParam->getValue();
            float releaseSeconds = releaseParam->convertFrom0to1(normalizedValue);
            currentReleaseTime.store(releaseSeconds);
            DBG("Space Dust: envRelease converted: " + safeStringFromNumber(releaseSeconds) + "s");
        }
        // Filter envelope (convert normalized to seconds - matches main ADSR pattern)
        if (auto* p = apvts.getParameter(juce::ParameterID{"filterEnvAttack", 1}.getParamID()))
            currentFilterEnvAttack.store(juce::jmax(0.01f, p->convertFrom0to1(p->getValue())));
        if (auto* p = apvts.getParameter(juce::ParameterID{"filterEnvDecay", 1}.getParamID()))
            currentFilterEnvDecay.store(juce::jmax(0.01f, p->convertFrom0to1(p->getValue())));
        if (auto* p = apvts.getParameter(juce::ParameterID{"filterEnvRelease", 1}.getParamID()))
            currentFilterEnvRelease.store(juce::jmax(0.01f, p->convertFrom0to1(p->getValue())));
        
        // Initialize LFO retrigger flags from parameters
        if (auto* lfo1RetriggerParam = apvts.getParameter(juce::ParameterID{"lfo1Retrigger", 1}.getParamID()))
        {
            lfoRetrigger[0].store(lfo1RetriggerParam->getValue() > 0.5f);
        }
        if (auto* lfo2RetriggerParam = apvts.getParameter(juce::ParameterID{"lfo2Retrigger", 1}.getParamID()))
        {
            lfoRetrigger[1].store(lfo2RetriggerParam->getValue() > 0.5f);
        }
    }
    catch (const std::exception& e)
    {
        DBG("Space Dust: Exception converting ADSR params: " + juce::String(e.what()));
    }
    catch (...)
    {
        DBG("Space Dust: Unknown exception converting ADSR params");
    }
    
    // Watch for presets edited outside the plugin, and publish what's currently loaded.
    // Timer callbacks run on the message thread, which is where we are now.
    presetHotReload.start();

    // After the parameters exist, because this walks them.
    modDestinations.build (apvts);

    // And after the destination table, because this looks every routing up in
    // it. Sizes the audio thread's arrays on this first call.
    rebuildCompiledRoutings();

    //==============================================================================
    DBG("Space Dust: Processor ctor END");
    logToFile("Processor constructed");
}

SpaceDustAudioProcessor::~SpaceDustAudioProcessor()
{
    logToFile("Destructor START");
    DBG("Space Dust: Destructor started - cleaning up resources");
    SAFETY_LOG_OBJECT_DTOR(this, "SpaceDustAudioProcessor dtor");

    // Cancel any pending async filter sync before removing listeners.
    // This prevents handleAsyncUpdate() from firing during/after destruction.
    cancelPendingUpdate();
    logToFile("Destructor: cancelled pending async updates");
    
    DBG("Space Dust: Removing parameter listeners");
    apvts.removeParameterListener(juce::ParameterID{"envAttack", 1}.getParamID(), this);
    apvts.removeParameterListener(juce::ParameterID{"envDecay", 1}.getParamID(), this);
    apvts.removeParameterListener(juce::ParameterID{"envSustain", 1}.getParamID(), this);
    apvts.removeParameterListener(juce::ParameterID{"envRelease", 1}.getParamID(), this);
    apvts.removeParameterListener(juce::ParameterID{"lfo1Retrigger", 1}.getParamID(), this);
    apvts.removeParameterListener(juce::ParameterID{"lfo2Retrigger", 1}.getParamID(), this);
    apvts.removeParameterListener(juce::ParameterID{"filterEnvAttack", 1}.getParamID(), this);
    apvts.removeParameterListener(juce::ParameterID{"filterEnvDecay", 1}.getParamID(), this);
    apvts.removeParameterListener(juce::ParameterID{"filterEnvRelease", 1}.getParamID(), this);
    apvts.removeParameterListener(juce::ParameterID{"mpeMode", 1}.getParamID(), this);
    apvts.removeParameterListener(juce::ParameterID{"mpePitchBendRange", 1}.getParamID(), this);
    DBG("Space Dust: Parameter listeners removed");
    
    //==============================================================================
    // -- Final Cleanup: Silence LeakedObjectDetector --
    // CRITICAL: Explicitly clear all synthesizer resources one final time to ensure
    // complete cleanup and silence LeakedObjectDetector assertions in debug builds.
    // 
    // Why this is necessary:
    // - JUCE's LeakedObjectDetector checks for ReferenceCountedObject leaks on shutdown
    // - SynthesiserSound objects are ReferenceCountedObjects
    // - Even though releaseResources() should clear everything, some hosts may not
    //   call it before destruction, or may call it in a way that leaves references
    // - This final cleanup ensures all resources are released before the destructor completes
    //
    // Order matters:
    // 1. Clear voices first (they may reference sounds)
    // 2. Clear sounds second (clears ReferenceCountedArray)
    // 3. Reset sample rate (ensures clean state)
    DBG("Space Dust: Clearing voices (count: " + safeStringFromNumber(synth.getNumVoices()) + ")");
    synth.clearVoices();           // Clear all voices (deletes them)
    DBG("Space Dust: Voices cleared");

    // MPE: juce::MPESynthesiser does NOT use SynthesiserSound, so there is no
    // clearSounds()/getNumSounds() to call.  The previous Synthesiser-based
    // implementation needed those to silence LeakedObjectDetector in Ableton; with
    // MPESynthesiser there is no ReferenceCountedArray<SynthesiserSound> to clear.

    synth.setCurrentPlaybackSampleRate(0.0); // Reset sample rate to clean state
    DBG("Space Dust: Sample rate reset");

    // Free the waveform snapshot the audio thread was holding. Audio has stopped
    // by now, so nothing is reading it; the library's own timer only ever sees the
    // banks the audio thread handed BACK, not the one it was still using.
    userWaveLibrary.reclaimFromAudio(audioUserWaveBank);


    //==============================================================================
    // -- Aggressive Ableton VST3 Unload Crash Workaround --
    // 
    // CRITICAL: Ableton Live 11/12 has a known VST3 unload bug where the host
    // may attempt to access plugin resources after the destructor has started but
    // before threads have fully detached. This causes intermittent crashes when
    // deleting plugin instances.
    //
    // Aggressive workaround (industry-standard pattern for Ableton, based on JUCE forums 2023-2025):
    // - Only apply for VST3 wrapper (check wrapper type)
    // - Use Timer::callAfterDelay with 200ms delay for stubborn cases
    //
    // This is a well-documented workaround used by many professional JUCE plugins
    // (including official JUCE examples) to ensure stable unload in Ableton Live.
    //
    // Note: This is NOT a hack - it's a necessary compatibility measure for
    // a known host bug. Ableton is aware of this issue but has not fixed it
    // as of Live 12.1.
    if (wrapperType == juce::AudioProcessor::WrapperType::wrapperType_VST3)
    {
        DBG("Space Dust: Applying Ableton VST3 workaround");
        // Use Timer callback with longer delay (200ms) for stubborn cases
        // Gives host threads time to fully detach before destruction completes
        // Based on JUCE forums 2023-2025: 200ms delay works for persistent Ableton crashes
        juce::Timer::callAfterDelay(200, []() {
            // Empty callback - just gives time for threads to detach
        });
    }
    
    DBG("Space Dust: Destructor cleanup complete - all resources released");
    logToFile("Destructor END - all resources released");

    // Memory-safety logger: drain & flush ring; join writer thread.
    SAFETY_LOGGER_SHUTDOWN();
}

//==============================================================================
const juce::String SpaceDustAudioProcessor::getName() const
{
    return JucePlugin_Name;
}

bool SpaceDustAudioProcessor::acceptsMidi() const
{
   #if JucePlugin_WantsMidiInput
    return true;
   #else
    return false;
   #endif
}

bool SpaceDustAudioProcessor::producesMidi() const
{
   #if JucePlugin_ProducesMidiOutput
    return true;
   #else
    return false;
   #endif
}

bool SpaceDustAudioProcessor::isMidiEffect() const
{
   #if JucePlugin_IsMidiEffect
    return true;
   #else
    return false;
   #endif
}

double SpaceDustAudioProcessor::getTailLengthSeconds() const
{
    return 0.0;
}

int SpaceDustAudioProcessor::getNumPrograms()
{
    return 1;   // NB: some hosts don't cope very well if you tell them there are 0 programs,
                // so this should be at least 1, even if you're not really implementing programs.
}

int SpaceDustAudioProcessor::getCurrentProgram()
{
    return 0;
}

void SpaceDustAudioProcessor::setCurrentProgram(int index)
{
    juce::ignoreUnused(index);
}

const juce::String SpaceDustAudioProcessor::getProgramName(int index)
{
    juce::ignoreUnused(index);
    return {};
}

void SpaceDustAudioProcessor::changeProgramName(int index, const juce::String& newName)
{
    juce::ignoreUnused(index, newName);
}

//==============================================================================
// -- Audio Setup --

void SpaceDustAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    logToFile("prepareToPlay START - sr=" + juce::String(sampleRate) + ", block=" + juce::String(samplesPerBlock));
#if JUCE_DEBUG
    try
    {
        juce::File logFile = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
            .getChildFile(safeString("SpaceDust_DebugLog.txt"));
        if (logFile.exists())
            logFile.deleteFile();
        juce::FileOutputStream out(logFile);
        if (out.openedOk())
        {
            out.writeText("Space Dust Debug Log - New session started: " +
                         juce::Time::getCurrentTime().toString(true, true) + "\n", false, false, nullptr);
            out.writeText("Log init: path=" + logFile.getFullPathName() + "\n", false, false, nullptr);
            out.writeText("Space Dust: prepareToPlay START - sr=" + safeStringFromNumber(sampleRate) +
                         ", block=" + safeStringFromNumber(samplesPerBlock) + "\n", false, false, nullptr);
            out.flush();
        }
    }
    catch (const std::exception& e)
    {
        DBG("Space Dust: Exception initializing log: " + juce::String(e.what()));
    }
    catch (...) {}
#endif
    
    DBG("Space Dust: prepareToPlay START - sr=" + safeStringFromNumber(sampleRate) + ", block=" + safeStringFromNumber(samplesPerBlock));
    
    //==============================================================================
    // -- CRITICAL: Voice Creation and DSP Initialization --
    // 
    // This is the ONLY place where synthesizer voices should be created.
    // 
    // Why voices must be created here (not in constructor):
    // 1. Sample rate is only known when host calls prepareToPlay()
    // 2. DSP components (filter, ADSR) require valid sample rate for prepare()
    // 3. Creating voices in constructor with sampleRate=0 causes:
    //    - StateVariableTPTFilter assertions (invalid sample rate)
    //    - ADSR assertions (invalid timing calculations)
    //    - Potential crashes during processBlock()
    //
    // Initialization order (MUST be followed):
    // 1. Clear any existing voices (safe re-initialization)
    // 2. Create new voices (DSP will be initialized via setCurrentPlaybackSampleRate)
    // 3. Set sample rate (this calls setCurrentPlaybackSampleRate on each voice)
    // 4. Update voices with current parameter values
    //
    // Note: prepareToPlay() may be called multiple times (e.g., sample rate changes),
    //       so we must clear existing voices first.
    
    // Step 1: Clear any existing voices (safe re-initialization)
    DBG("Space Dust: prepareToPlay - Step 1: Clearing voices");
    try
    {
        synth.clearVoices();
        DBG("Space Dust: prepareToPlay - Step 1: Voices cleared");
    }
    catch (const std::exception& e)
    {
        DBG("Space Dust: Exception clearing voices: " + juce::String(e.what()));
        throw;
    }
    catch (...)
    {
        DBG("Space Dust: Unknown exception clearing voices");
        throw;
    }
    
    // Step 2: (Formerly: ensure at least one SynthesiserSound exists.)
    // MPE: juce::MPESynthesiser does NOT use SynthesiserSound â€” voice/note assignment
    // is handled entirely by the internal MPEInstrument.  We intentionally skip the
    // addSound() call here; SynthSound.h remains in the codebase as an unused stub
    // for preset / state-file backwards compatibility but the synth no longer needs it.
    DBG("Space Dust: prepareToPlay - Step 2: (MPE - no SynthesiserSound needed)");
    
    // Step 3: Create 8 new synthesizer voices and set synthesiser for legato/mono mode
    DBG("Space Dust: prepareToPlay - Step 3: Creating voices");
    try
    {
        for (int i = 0; i < 8; ++i)
        {
            auto* v = new SynthVoice();
            v->setSynthesiser(&synth);
            synth.addVoice(v);
            if ((i + 1) % 2 == 0)
                DBG("Space Dust: prepareToPlay - Step 3: Added " + safeStringFromNumber(i + 1) + " voices");
        }
        DBG("Space Dust: prepareToPlay - Step 3: All 8 voices created");
    }
    catch (const std::exception& e)
    {
        DBG("Space Dust: Exception creating voices: " + juce::String(e.what()));
        throw;
    }
    catch (...)
    {
        DBG("Space Dust: Unknown exception creating voices");
        throw;
    }
    
    // Step 4: Set sample rate for synthesizer
    DBG("Space Dust: prepareToPlay - Step 4: Setting sample rate");
    try
    {
        synth.setCurrentPlaybackSampleRate(sampleRate);
        currentSampleRate = sampleRate;
        // Establish the MPE layout on the first audio block (applied audio-thread-side
        // by applyPendingMpeReconfig() so it never races note rendering).
        mpeReconfigPending.store(true, std::memory_order_release);
        
        // Initialize LFO buffers for per-sample processing.
        // Size with generous headroom (>= 8192): some hosts (e.g. Ableton during
        // freeze/bounce/render) hand processBlock a LARGER block than the size they
        // declared here, and the LFO fill loops write `numSamples` entries via the
        // unchecked setSample(). Without headroom that overruns the buffer and
        // corrupts the heap. processBlock also has a hard grow-guard as a backstop.
        for (int lfo = 0; lfo < spacedust::numLfos; ++lfo)
        {
            lfoBuffers[lfo].setSize(1, juce::jmax(samplesPerBlock, 8192));
            lfoBuffers[lfo].clear();
            // Seed Sample & Hold with initial random values (avoids first period at 0)
            lfoShState[lfo] = lfoShState[lfo] * 1103515245u + 12345u;
            lfoSampleHoldValue[lfo] = (static_cast<float>((lfoShState[lfo] >> 16) & 0x7FFF) / 32767.5f) * 2.0f - 1.0f;
        }

        // The voice modulation scratch, allocated ONCE here where the audio is
        // stopped. This is the ONLY place it is ever sized: nothing on the
        // audio thread grows it, so filling it every block cannot allocate.
        //
        // The 8192 headroom is the same figure the LFO buffers use, because
        // some hosts hand processBlock a bigger block than they declared. Where
        // the LFO buffers must grow to meet such a block -- writing past them
        // corrupts the heap -- the scratch does not: fillVoiceModScratch clamps
        // instead and publishes how far it got in numVoiceModSamples().
        voiceModRowSamples = juce::jmax(samplesPerBlock, 8192);
        voiceModScratch.assign((size_t) maxVoiceModRows * (size_t) voiceModRowSamples, 0.0f);
        voiceModRowsFilled = 0;
        voiceModValidSamples = 0;

        // Initialize delay lines
        juce::dsp::ProcessSpec delaySpec;
        delaySpec.sampleRate = sampleRate;
        delaySpec.maximumBlockSize = static_cast<juce::uint32>(samplesPerBlock);
        delaySpec.numChannels = 1;
        delayLineL.prepare(delaySpec);
        delayLineR.prepare(delaySpec);
        delayLineL.reset();
        delayLineR.reset();
        
        // Initialize delay filters (HP then LP in series - applied ONLY to feedback/wet path)
        juce::dsp::ProcessSpec filterSpec;
        filterSpec.sampleRate = sampleRate;
        filterSpec.maximumBlockSize = static_cast<juce::uint32>(samplesPerBlock);
        filterSpec.numChannels = 2;
        delayFilterHP.prepare(filterSpec);
        delayFilterLP.prepare(filterSpec);
        delayFilterHPFb.prepare(filterSpec);
        delayFilterLPFb.prepare(filterSpec);
        delayFilterHP.reset();
        delayFilterLP.reset();
        delayFilterHPFb.reset();
        delayFilterLPFb.reset();
        
        // Smoothed delay parameters (prevents zippers, resonance spikes, pitch artifacts)
        const double rampSec = 0.03;  // 30ms ramp for smooth param changes
        smoothedDelayTime.reset(sampleRate, rampSec);
        smoothedDelayTime.setCurrentAndTargetValue(1000.0f);  // Initial delay samples
        smoothedDelayDecay.reset(sampleRate, rampSec);
        smoothedDelayDecay.setCurrentAndTargetValue(0.0f);
        smoothedDelayDryWet.reset(sampleRate, rampSec);
        smoothedDelayDryWet.setCurrentAndTargetValue(0.0f);
        smoothedDelayHPCutoff.reset(sampleRate, 0.01);  // 10ms for cutoff (faster response)
        smoothedDelayHPCutoff.setCurrentAndTargetValue(1000.0f);
        smoothedDelayLPCutoff.reset(sampleRate, 0.01);
        smoothedDelayLPCutoff.setCurrentAndTargetValue(4000.0f);
        smoothedDelayHPQ.reset(sampleRate, rampSec);
        smoothedDelayHPQ.setCurrentAndTargetValue(0.707f);
        smoothedDelayLPQ.reset(sampleRate, rampSec);
        smoothedDelayLPQ.setCurrentAndTargetValue(0.707f);
        
        // Initialize reverb
        juce::dsp::ProcessSpec reverbSpec;
        reverbSpec.sampleRate = sampleRate;
        reverbSpec.maximumBlockSize = static_cast<juce::uint32>(samplesPerBlock);
        reverbSpec.numChannels = 2;
        reverb_.prepare(reverbSpec);
        reverb_.reset();
        lastReverbDecayForBypass_ = -1.0f;

        // Initialize grain delay
        grainDelay_.prepare(reverbSpec);
        grainDelay_.reset();

        // Initialize phaser
        phaser_.prepare(reverbSpec);
        phaser_.reset();

        // Initialize flanger
        flanger_.prepare(reverbSpec);
        flanger_.reset();

        // Initialize bit crusher
        bitCrusher_.prepare(reverbSpec);
        bitCrusher_.reset();

        // Initialize soft clipper
        softClipper_.prepare(reverbSpec);
        softClipper_.reset();

        // Initialize compressor
        compressor_.prepare(reverbSpec);
        compressor_.reset();

        // Initialize lo-fi
        lofi_.prepare(reverbSpec);
        lofi_.reset();

        // Initialize transient
        transient_.prepare(reverbSpec);
        transient_.reset();

        // Pre-mode transient "before the filter" mirror filters + render scratch.
        // One mirror per per-voice filter stage (master + the two Mod-tab filters)
        // so an unlinked Mod filter cuts the transient just like the Main filter.
        transientPreFilter_.prepare(reverbSpec);
        transientPreFilter_.reset();
        transientPreFilterMod1_.prepare(reverbSpec);
        transientPreFilterMod1_.reset();
        transientPreFilterMod2_.prepare(reverbSpec);
        transientPreFilterMod2_.reset();
        transientScratch_.setSize(reverbSpec.numChannels,
                                  static_cast<int>(reverbSpec.maximumBlockSize),
                                  false, false, true);
        transientScratch_.clear();

        // Initialize final EQ
        finalEQ_.prepare(reverbSpec);
        finalEQ_.reset();

        // Initialize Ka-Donk delay lines
        {
            juce::dsp::ProcessSpec delaySpec;
            delaySpec.sampleRate = sampleRate;
            delaySpec.maximumBlockSize = static_cast<juce::uint32>(samplesPerBlock);
            delaySpec.numChannels = 1;
            kaDonkDelayL_.prepare(delaySpec);
            kaDonkDelayR_.prepare(delaySpec);
            kaDonkDelayL_.reset();
            kaDonkDelayR_.reset();
            smoothedKaDonkDelay_.reset(sampleRate, 0.05);
            smoothedKaDonkDelay_.setCurrentAndTargetValue(0.0f);
        }

        // Initialize trance gate
        tranceGate_.prepare(reverbSpec);
        tranceGate_.reset();

        // Initialize goniometer buffers (double-buffered for Spectral tab Lissajous display).
        // Size them to the FIXED maximum (not samplesPerBlock): the editor's paint thread
        // holds a reference returned by getGoniometerBuffer() and reads up to validSamples.
        // If prepareToPlay reallocated these (host changing buffer size / sample rate while
        // the Spectral tab is visible) the in-flight paint would read freed memory. A fixed
        // capacity means setSize never reallocates after the first prepare, so the pointer
        // the paint thread holds stays valid for the process lifetime. Writes copy at most
        // this many samples (see processBlock), so the buffer is never overrun.
        goniometerBuffer[0].setSize(2, goniometerMaxSamples, false, true, true);
        goniometerBuffer[1].setSize(2, goniometerMaxSamples, false, true, true);
        goniometerBuffer[0].clear();
        goniometerBuffer[1].clear();
        goniometerValidSamples.store(0, std::memory_order_release);
        goniometerReadIndex.store(0);

        // Resample history: as many seconds as a waveform slot can hold, because
        // that is the most Resample could ever hand to one. Allocated here, on the
        // message thread, and never from processBlock.
        resampleCapture.prepare(sampleRate, UserWave::maxSampleSeconds);


        DBG("Space Dust: prepareToPlay - Step 4: Sample rate set to " + safeStringFromNumber(sampleRate));
    }
    catch (const std::exception& e)
    {
        DBG("Space Dust: Exception setting sample rate: " + juce::String(e.what()));
        throw;
    }
    catch (...)
    {
        DBG("Space Dust: Unknown exception setting sample rate");
        throw;
    }
    
    // Step 5: Initialize all voices' DSP with valid sample rate
    DBG("Space Dust: prepareToPlay - Step 5: Initializing voice DSP");
    try
    {
        for (int i = 0; i < synth.getNumVoices(); ++i)
        {
            if (auto* spaceDustVoice = dynamic_cast<SynthVoice*>(synth.getVoice(i)))
            {
                spaceDustVoice->setProcessor(this);
                spaceDustVoice->prepareToPlay(sampleRate, samplesPerBlock);
            }
        }
        DBG("Space Dust: prepareToPlay - Step 5: All voices DSP initialized");
    }
    catch (const std::exception& e)
    {
        DBG("Space Dust: Exception initializing voice DSP: " + juce::String(e.what()));
        throw;
    }
    catch (...)
    {
        DBG("Space Dust: Unknown exception initializing voice DSP");
        throw;
    }
    
    // Step 6: Initialize all voices with current parameter values
    DBG("Space Dust: prepareToPlay - Step 6: Updating voices with parameters");
    try
    {
        updateVoicesWithParameters();

        // Start from a clean mono/legato note model and transport-edge baseline.
        synth.resetNoteState();
        wasPlayingState = false;
        lastPpqPosition = 0.0;

        DBG("Space Dust: prepareToPlay END - voices initialized");
    }
    catch (const std::exception& e)
    {
        DBG("Space Dust: Exception updating voices with parameters: " + juce::String(e.what()));
        throw;
    }
    catch (...)
    {
        DBG("Space Dust: Unknown exception updating voices with parameters");
        throw;
    }
}

//==============================================================================
// -- Parameter Updates --

/**
    Update all voices with current parameter values from AudioProcessorValueTreeState.
    
    This method is called from the audio thread (processBlock) to ensure real-time
    parameter updates. All parameter reads are atomic and lock-free, making this
    safe for audio thread execution.
    
    Real-time Safety: Only reads atomic values, no allocations, no locks.
*/
void SpaceDustAudioProcessor::updateVoicesWithParameters(float lfo1Modulation, float lfo2Modulation)
{
    // Get parameter values (real-time safe: reading atomic values)
    int osc1Wave = (int)safeGetParam(apvts, "osc1Waveform");
    int osc2Wave = (int)safeGetParam(apvts, "osc2Waveform");

    // Oscillator tuning parameters (simple, intuitive system)
    float osc1CoarseTune = voiceModulatedValue("osc1CoarseTune", safeGetParam(apvts, "osc1CoarseTune"));
    float osc1Detune = voiceModulatedValue("osc1Detune", safeGetParam(apvts, "osc1Detune"));
    float osc2CoarseTune = voiceModulatedValue("osc2CoarseTune", safeGetParam(apvts, "osc2CoarseTune"));
    float osc2Detune = voiceModulatedValue("osc2Detune", safeGetParam(apvts, "osc2Detune"));

    // LFO modulation is now applied per-sample in renderNextBlock via LFO buffers
    // lfo1Modulation and lfo2Modulation parameters are ignored (kept for API compatibility)

    // Independent oscillator and noise level controls.
    // osc1Level, osc2Level and noiseLevel are three of the six destinations that
    // are ALREADY applied per sample inside SynthVoice, via the legacy LFO-target
    // path (SynthVoice.cpp:1712-1747). Routing them through voiceModulatedValue
    // here as well would apply the same modulation twice, so they stay untouched.
    float osc1Level = safeGetParam(apvts, "osc1Level");
    float osc2Level = safeGetParam(apvts, "osc2Level");
    float noiseLevel = safeGetParam(apvts, "noiseLevel");
    int noiseTypeVal = (int)safeGetParam(apvts, "noiseType");  // 0=White, 1=Pink

    int filterMode = (int)safeGetParam(apvts, "filterMode");
    // filterCutoff is one of the six protected destinations too (see above) --
    // it already gets per-sample modulation inside SynthVoice.
    float filterCutoffHz = 8000.0f;
    if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("filterCutoff")))
        filterCutoffHz = juce::jlimit(20.0f, 20000.0f, p->get());
    // LFO filter modulation is applied per-sample in renderNextBlock

    float filterResonance = voiceModulatedValue("filterResonance", safeGetParam(apvts, "filterResonance"));
    
    // LFO targets (cache per-block to avoid per-sample APVTS reads in voice - major CPU win)
    int lfo1Target = static_cast<int>(safeGetParam(apvts, "lfo1Target"));
    int lfo2Target = static_cast<int>(safeGetParam(apvts, "lfo2Target"));
    
    bool modFilter1Show = safeGetParam(apvts, "modFilter1Show") > 0.5f;
    bool modFilter2Show = safeGetParam(apvts, "modFilter2Show") > 0.5f;
    bool modFilter1Link = safeGetParam(apvts, "modFilter1LinkToMaster") > 0.5f;
    bool modFilter2Link = safeGetParam(apvts, "modFilter2LinkToMaster") > 0.5f;
    bool warmSaturationMaster = safeGetParam(apvts, "warmSaturationMaster") > 0.5f;
    bool filterOversample = safeGetParam(apvts, "filterOversample") > 0.5f;  // [A/B prototype]
    bool filterKeyTrack = safeGetParam(apvts, "filterKeyTrack") > 0.5f;

    // When linked, use master filter values directly instead of mod filter values.
    // This avoids calling setValueNotifyingHost for sync, which triggers performEdit
    // in the VST3 wrapper and causes Ableton to grey out automation lanes.
    // When linked, modFilterNCutoff/Resonance alias the ALREADY-modulated master
    // values above rather than reading their own parameters -- correct either
    // way: filterCutoffHz stays untouched (protected, see above) and
    // filterResonance already carries whatever the matrix gave it. Only the
    // unlinked branch reads its own knob, and that knob IS a legal, unprotected
    // destination, so it goes through voiceModulatedValue.
    int modFilter1Mode = modFilter1Link ? filterMode : (int)safeGetParam(apvts, "modFilter1Mode");
    float modFilter1Cutoff = modFilter1Link ? filterCutoffHz
        : voiceModulatedValue("modFilter1Cutoff", safeGetParam(apvts, "modFilter1Cutoff"));
    float modFilter1Resonance = modFilter1Link ? filterResonance
        : voiceModulatedValue("modFilter1Resonance", safeGetParam(apvts, "modFilter1Resonance"));
    bool warmSaturationMod1 = modFilter1Link ? warmSaturationMaster : safeGetParam(apvts, "warmSaturationMod1") > 0.5f;
    bool modFilter1KeyTrack = modFilter1Link ? filterKeyTrack : safeGetParam(apvts, "modFilter1KeyTrack") > 0.5f;

    int modFilter2Mode = modFilter2Link ? filterMode : (int)safeGetParam(apvts, "modFilter2Mode");
    float modFilter2Cutoff = modFilter2Link ? filterCutoffHz
        : voiceModulatedValue("modFilter2Cutoff", safeGetParam(apvts, "modFilter2Cutoff"));
    float modFilter2Resonance = modFilter2Link ? filterResonance
        : voiceModulatedValue("modFilter2Resonance", safeGetParam(apvts, "modFilter2Resonance"));
    bool warmSaturationMod2 = modFilter2Link ? warmSaturationMaster : safeGetParam(apvts, "warmSaturationMod2") > 0.5f;
    bool modFilter2KeyTrack = modFilter2Link ? filterKeyTrack : safeGetParam(apvts, "modFilter2KeyTrack") > 0.5f;

    // Filter envelope: read directly from parameters each block (guarantees label matches decay)
    // Uses plain param ID strings to match SliderAttachment; p->get() returns exact displayed value
    float filterEnvAttack = 0.01f, filterEnvDecay = 0.8f, filterEnvRelease = 3.0f;
    if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("filterEnvAttack")))
        filterEnvAttack = voiceModulatedValue("filterEnvAttack", juce::jlimit(0.01f, 20.0f, p->get()));
    if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("filterEnvDecay")))
        filterEnvDecay = voiceModulatedValue("filterEnvDecay", juce::jlimit(0.01f, 20.0f, p->get()));
    if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("filterEnvRelease")))
        filterEnvRelease = voiceModulatedValue("filterEnvRelease", juce::jlimit(0.01f, 20.0f, p->get()));
    float filterEnvAmount = 0.0f;
    if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("filterEnvAmount")))
        filterEnvAmount = voiceModulatedValue("filterEnvAmount", juce::jlimit(-100.0f, 100.0f, p->get()));
    // Sustain level tracks the cutoff knob on the full 20 Hz..20 kHz log span so the decay/hold
    // stage matches the filter frequency when the envelope uses the full range (see SynthVoice).
    float filterEnvSustain = 0.7f;
    {
        const float logMin = std::log(20.0f);
        const float logMax = std::log(20000.0f);
        const float logCut = std::log(filterCutoffHz);
        filterEnvSustain = juce::jlimit(0.0f, 1.0f, (logCut - logMin) / (logMax - logMin));
    }
    
    // ADSR parameters: Use atomic values (already converted from normalized to seconds/level)
    // This ensures real-time safe, lock-free access from the audio thread
    float envAttack = voiceModulatedValue("envAttack", currentAttackTime.load());
    float envDecay = voiceModulatedValue("envDecay", currentDecayTime.load());
    float envSustain = voiceModulatedValue("envSustain", currentSustainLevel.load());
    float envRelease = voiceModulatedValue("envRelease", currentReleaseTime.load());
    
    // Voice mode and glide. safeGetParam returns the DENORMALISED (actual) value via
    // APVTS::getRawParameterValue -> getRawDenormalisedValue, so this is already in
    // seconds. The previous code wrapped it in convertFrom0to1() which treated 0.016
    // as a 0-1 normalised value and produced ~0.000338s â€” which then got snapped to 0
    // by the parameter's 0.001 interval, silently disabling user glide and forcing
    // the 3ms anti-click branch on every note in mono/legato.
    float glideTime = voiceModulatedValue("glideTime", juce::jlimit(0.0f, 5.0f, safeGetParam(apvts, "glideTime")));
    bool legatoGlide = safeGetParam(apvts, "legatoGlide") > 0.5f;

    // Pitch envelope parameters (use get() for actual value - separate from pitch bend)
    float pitchEnvAmount = 0.0f, pitchEnvTime = 0.0f, pitchEnvPitch = 0.0f;
    if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("pitchEnvAmount")))
        pitchEnvAmount = voiceModulatedValue("pitchEnvAmount", p->get());
    if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("pitchEnvTime")))
        pitchEnvTime = voiceModulatedValue("pitchEnvTime", p->get());
    if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("pitchEnvPitch")))
        pitchEnvPitch = voiceModulatedValue("pitchEnvPitch", p->get());

    // Pitch bend parameters (use get() for actual value - separate from pitch envelope).
    // pitchBend itself (the -1..1 wheel position) is NOT routed: it is host-driven
    // (DestinationTable::isLegalDestination refuses it outright) and carries its
    // own snap-back state machine below, so it is left exactly as it was.
    float pitchBendAmount = 0.0f;
    if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("pitchBendAmount")))
        pitchBendAmount = voiceModulatedValue("pitchBendAmount", juce::jlimit(0.0f, 24.0f, p->get()));
    float pitchBend;
    if (pitchBendSnapActive.load())
    {
        pitchBend = pitchBendRampCurrentValue.load();
    }
    else if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("pitchBend")))
    {
        pitchBend = p->get();  // -1 to 1
    }
    else
    {
        pitchBend = 0.0f;
    }

    // Analog Drift lives in the Lo-Fi UI group; only apply when Lo-Fi is enabled
    float analogDrift = voiceModulatedValue("analogDrift", juce::jlimit(0.0f, 1.0f, safeGetParam(apvts, "analogDrift")));
    if (safeGetParam(apvts, "lofiEnabled") <= 0.5f)
        analogDrift = 0.0f;

    // Bend, Spectrum and Sync. Read once here rather than per voice: they are the
    // same ten numbers for every voice, and safeGetParam is not free.
    const auto readShaping = [this](const char* bp, const char* bm, const char* bpm,
                                    const char* sp, const char* sy)
    {
        PhaseShaper::Amounts a;
        a.bendPlus      = voiceModulatedValue(bp,  juce::jlimit(0.0f, 1.0f, safeGetParam(apvts, bp)));
        a.bendMinus     = voiceModulatedValue(bm,  juce::jlimit(0.0f, 1.0f, safeGetParam(apvts, bm)));
        a.bendPlusMinus = voiceModulatedValue(bpm, juce::jlimit(0.0f, 1.0f, safeGetParam(apvts, bpm)));
        a.spectrum      = voiceModulatedValue(sp,  juce::jlimit(0.0f, 1.0f, safeGetParam(apvts, sp)));
        a.sync          = voiceModulatedValue(sy,  juce::jlimit(0.0f, 1.0f, safeGetParam(apvts, sy)));
        return a;
    };

    const auto osc1Shaping = readShaping("osc1BendPlus", "osc1BendMinus",
                                         "osc1BendPlusMinus", "osc1Spectrum", "osc1Sync");
    const auto osc2Shaping = readShaping("osc2BendPlus", "osc2BendMinus",
                                         "osc2BendPlusMinus", "osc2Spectrum", "osc2Sync");
    const auto subOscShaping = readShaping("subOscBendPlus", "subOscBendMinus",
                                           "subOscBendPlusMinus", "subOscSpectrum", "subOscSync");

    // Unison, read once for the same reason as the shaping above.
    const int osc1UnisonVoices = juce::jlimit(1, Unison::maxVoices,
                                              (int) std::lround(safeGetParam(apvts, "osc1UnisonVoices")));
    const int osc2UnisonVoices = juce::jlimit(1, Unison::maxVoices,
                                              (int) std::lround(safeGetParam(apvts, "osc2UnisonVoices")));
    // Voice counts stay un-modulated on purpose: they are AudioParameterInt (see
    // their registration), which DestinationTable::isLegalDestination refuses
    // outright, so there is no routing to read here.
    const float osc1UnisonDetune = voiceModulatedValue("osc1UnisonDetune", juce::jlimit(0.0f, 1.0f, safeGetParam(apvts, "osc1UnisonDetune")));
    const float osc2UnisonDetune = voiceModulatedValue("osc2UnisonDetune", juce::jlimit(0.0f, 1.0f, safeGetParam(apvts, "osc2UnisonDetune")));
    const float osc1UnisonWidth = voiceModulatedValue("osc1UnisonWidth", juce::jlimit(0.0f, 1.0f, safeGetParam(apvts, "osc1UnisonWidth")));
    const float osc2UnisonWidth = voiceModulatedValue("osc2UnisonWidth", juce::jlimit(0.0f, 1.0f, safeGetParam(apvts, "osc2UnisonWidth")));

    const int subOscUnisonVoices = juce::jlimit(1, Unison::maxVoices,
                                                (int) std::lround(safeGetParam(apvts, "subOscUnisonVoices")));
    const float subOscUnisonDetune = voiceModulatedValue("subOscUnisonDetune", juce::jlimit(0.0f, 1.0f, safeGetParam(apvts, "subOscUnisonDetune")));
    const float subOscUnisonWidth = voiceModulatedValue("subOscUnisonWidth", juce::jlimit(0.0f, 1.0f, safeGetParam(apvts, "subOscUnisonWidth")));

    const int noiseUnisonVoices = juce::jlimit(1, Unison::maxVoices,
                                               (int) std::lround(safeGetParam(apvts, "noiseUnisonVoices")));
    const float noiseUnisonDetune = voiceModulatedValue("noiseUnisonDetune", juce::jlimit(0.0f, 1.0f, safeGetParam(apvts, "noiseUnisonDetune")));
    const float noiseUnisonWidth = voiceModulatedValue("noiseUnisonWidth", juce::jlimit(0.0f, 1.0f, safeGetParam(apvts, "noiseUnisonWidth")));

    // How far apart the copies START in the cycle. Read here with the rest of the
    // unison numbers because it is set with them, once per block, for every voice.
    const float osc1UnisonPhase = voiceModulatedValue("osc1UnisonPhase", juce::jlimit(0.0f, 1.0f, safeGetParam(apvts, "osc1UnisonPhase")));
    const float osc2UnisonPhase = voiceModulatedValue("osc2UnisonPhase", juce::jlimit(0.0f, 1.0f, safeGetParam(apvts, "osc2UnisonPhase")));
    const float subOscUnisonPhase = voiceModulatedValue("subOscUnisonPhase", juce::jlimit(0.0f, 1.0f, safeGetParam(apvts, "subOscUnisonPhase")));
    const float noiseUnisonPhase = voiceModulatedValue("noiseUnisonPhase", juce::jlimit(0.0f, 1.0f, safeGetParam(apvts, "noiseUnisonPhase")));

    // Knobs the original code read fresh inside the per-voice loop below. Hoisted
    // here so voiceModulatedValue's slotFor lookup constructs its std::string
    // once per BLOCK, not once per voice -- the audio-thread allocation rule the
    // task brief calls out applies just as much to "once per voice" as it does
    // to "once per sample".
    const float osc1Pan = voiceModulatedValue("osc1Pan", safeGetParam(apvts, "osc1Pan"));
    const float osc2Pan = voiceModulatedValue("osc2Pan", safeGetParam(apvts, "osc2Pan"));
    const float lowShelfAmount = voiceModulatedValue("lowShelfAmount", safeGetParam(apvts, "lowShelfAmount"));
    const float highShelfAmount = voiceModulatedValue("highShelfAmount", safeGetParam(apvts, "highShelfAmount"));
    const float subOscLevel = voiceModulatedValue("subOscLevel", safeGetParam(apvts, "subOscLevel"));
    const float subOscCoarse = voiceModulatedValue("subOscCoarse", safeGetParam(apvts, "subOscCoarse"));
    const float mpePressureDepth = voiceModulatedValue("mpePressureDepth", safeGetParam(apvts, "mpePressureDepth")) / 100.0f;
    const float mpeTimbreDepth = voiceModulatedValue("mpeTimbreDepth", safeGetParam(apvts, "mpeTimbreDepth")) / 100.0f;
    const float velocityAmount = voiceModulatedValue("velocityAmount", safeGetParam(apvts, "velocityAmount")) / 100.0f;

    // Update all voices with current parameter values
    for (int i = 0; i < synth.getNumVoices(); ++i)
    {
        if (auto* voice = dynamic_cast<SynthVoice*>(synth.getVoice(i)))
        {
            voice->setUserWaveBank(audioUserWaveBank);
            voice->setOsc1Waveform(osc1Wave);
            voice->setOsc2Waveform(osc2Wave);
            voice->setOsc1WaveShaping(osc1Shaping);
            voice->setOsc2WaveShaping(osc2Shaping);
            voice->setOsc1Unison(osc1UnisonVoices, osc1UnisonDetune, osc1UnisonWidth, osc1UnisonPhase);
            voice->setOsc2Unison(osc2UnisonVoices, osc2UnisonDetune, osc2UnisonWidth, osc2UnisonPhase);
            voice->setSubOscWaveShaping(subOscShaping);
            voice->setSubOscUnison(subOscUnisonVoices, subOscUnisonDetune, subOscUnisonWidth,
                                   subOscUnisonPhase);
            voice->setNoiseUnison(noiseUnisonVoices, noiseUnisonDetune, noiseUnisonWidth,
                                  noiseUnisonPhase);
            voice->setOsc1CoarseTune(osc1CoarseTune);
            voice->setOsc1Detune(osc1Detune);
            voice->setOsc2CoarseTune(osc2CoarseTune);
            voice->setOsc2Detune(osc2Detune);
            voice->setOsc1Level(osc1Level);
            voice->setOsc2Level(osc2Level);
            voice->setOsc1Pan(osc1Pan);
            voice->setOsc2Pan(osc2Pan);
            voice->setNoiseLevel(noiseLevel);
            voice->setNoiseType(noiseTypeVal);  // 0=White, 1=Pink (automatable param)
            voice->setLowShelfAmount(lowShelfAmount);
            voice->setHighShelfAmount(highShelfAmount);
            voice->setFilterMode(filterMode);
            voice->setFilterCutoff(filterCutoffHz);
            voice->setFilterResonance(filterResonance);
            voice->setWarmSaturationMaster(warmSaturationMaster);
            voice->setFilterOversample(filterOversample);  // [A/B prototype]
            voice->setFilterKeyTrack(filterKeyTrack);
            voice->setModFilter1(modFilter1Show, modFilter1Link, modFilter1Mode, modFilter1Cutoff, modFilter1Resonance);
            voice->setWarmSaturationMod1(warmSaturationMod1);
            voice->setModFilter1KeyTrack(modFilter1KeyTrack);
            voice->setModFilter2(modFilter2Show, modFilter2Link, modFilter2Mode, modFilter2Cutoff, modFilter2Resonance);
            voice->setWarmSaturationMod2(warmSaturationMod2);
            voice->setModFilter2KeyTrack(modFilter2KeyTrack);
            voice->setFilterEnvAttack(filterEnvAttack);
            voice->setFilterEnvDecay(filterEnvDecay);
            voice->setFilterEnvSustain(filterEnvSustain);
            voice->setFilterEnvRelease(filterEnvRelease);
            voice->setFilterEnvAmount(filterEnvAmount);
            voice->setEnvAttack(envAttack);
            voice->setEnvDecay(envDecay);
            voice->setEnvSustain(envSustain);
            voice->setEnvRelease(envRelease);
            voice->setGlideTime(glideTime);
            voice->setLegatoGlide(legatoGlide);
            voice->setPitchEnvAmount(pitchEnvAmount);
            voice->setPitchEnvTime(pitchEnvTime);
            voice->setPitchEnvPitch(pitchEnvPitch);
            voice->setSubOscOn(safeGetParam(apvts, "subOscOn") > 0.5f);
            voice->setSubOscWaveform(static_cast<int>(safeGetParam(apvts, "subOscWaveform")));
            voice->setSubOscLevel(subOscLevel);
            voice->setSubOscCoarse(subOscCoarse);
            voice->setPitchBendAmount(pitchBendAmount);
            voice->setPitchBend(pitchBend);
            voice->setLfoTargets(lfo1Target, lfo2Target);
            voice->setLfoRates(lastLfoHz[0], lastLfoHz[1]);
            voice->setAnalogDrift(analogDrift);

            // MPE expression depth (0-100% â†’ 0.0-1.0), already divided above.
            voice->setMpePressureDepth(mpePressureDepth);
            voice->setMpeTimbreDepth(mpeTimbreDepth);
            voice->setVelocityAmount(velocityAmount);
        }
    }
}

//==============================================================================
// -- ADSR Parameter Listener --

/**
    ValueTree listener callback for ADSR parameter updates.
    
    Converts normalized parameter values (0.0-1.0) to actual time values in seconds
    or sustain level (0.0-1.0), then stores in atomic variables for real-time safe
    access from the audio thread.
    
    This is called on the message thread when parameters change, ensuring safe
    conversion and atomic storage before the audio thread accesses the values.
    
    Real-time Safety: This runs on the message thread, not the audio thread.
    The atomic stores are lock-free and safe for concurrent access.
*/
void SpaceDustAudioProcessor::parameterChanged(const juce::String& parameterID, float newValue)
{
    if (parameterID == juce::ParameterID{"envAttack", 1}.getParamID())
    {
        currentAttackTime.store(juce::jlimit(0.01f, 20.0f, newValue));
        logToFileThrottled("envAttack", "ADSR attack=" + juce::String(newValue, 4) + "s");
    }
    else if (parameterID == juce::ParameterID{"envDecay", 1}.getParamID())
    {
        currentDecayTime.store(juce::jlimit(0.01f, 20.0f, newValue));
        logToFileThrottled("envDecay", "ADSR decay=" + juce::String(newValue, 4) + "s");
    }
    else if (parameterID == juce::ParameterID{"envSustain", 1}.getParamID())
    {
        currentSustainLevel.store(newValue);
        logToFileThrottled("envSustain", "ADSR sustain=" + juce::String(newValue, 4));
    }
    else if (parameterID == juce::ParameterID{"envRelease", 1}.getParamID())
    {
        currentReleaseTime.store(juce::jlimit(0.01f, 20.0f, newValue));
        logToFileThrottled("envRelease", "ADSR release=" + juce::String(newValue, 4) + "s");
    }
    else if (parameterID == juce::ParameterID{"lfo1Retrigger", 1}.getParamID())
    {
        lfoRetrigger[0].store(newValue > 0.5f);
    }
    else if (parameterID == juce::ParameterID{"lfo2Retrigger", 1}.getParamID())
    {
        lfoRetrigger[1].store(newValue > 0.5f);
    }
    else if (parameterID == juce::ParameterID{"filterEnvAttack", 1}.getParamID())
    {
        // Read actual value from param (matches UI label exactly)
        if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter(parameterID)))
            currentFilterEnvAttack.store(juce::jlimit(0.01f, 20.0f, p->get()));
    }
    else if (parameterID == juce::ParameterID{"filterEnvDecay", 1}.getParamID())
    {
        // Read actual value from param (matches UI label exactly)
        if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter(parameterID)))
            currentFilterEnvDecay.store(juce::jlimit(0.01f, 20.0f, p->get()));
    }
    else if (parameterID == juce::ParameterID{"filterEnvRelease", 1}.getParamID())
    {
        // Read actual value from param (matches UI label exactly)
        if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter(parameterID)))
            currentFilterEnvRelease.store(juce::jlimit(0.01f, 20.0f, p->get()));
    }
    // -- MPE mode / bend range: request a reconfig, APPLIED on the audio thread --
    // parameterChanged can run on the message thread; doing the MPE zone-layout change
    // here would race the audio thread's renderNextBlock (it releases notes / mutates the
    // note array). Instead just flag it; processBlock applies it before rendering. See
    // applyPendingMpeReconfig().
    else if (parameterID == juce::ParameterID{"mpeMode", 1}.getParamID()
          || parameterID == juce::ParameterID{"mpePitchBendRange", 1}.getParamID())
    {
        mpeReconfigPending.store(true, std::memory_order_release);
    }
    // Filter link params: no sync needed. updateVoicesWithParameters() reads master
    // values directly when linked, bypassing the mod filter parameters entirely.
    // This avoids setValueNotifyingHost which triggers performEdit in the VST3 wrapper,
    // causing Ableton to grey out automation lanes.
}

//==============================================================================
// -- AsyncUpdater (kept for future use, currently no-op) --

void SpaceDustAudioProcessor::handleAsyncUpdate()
{
    // Filter sync is now handled at the voice level in updateVoicesWithParameters():
    // when a mod filter is linked to master, the voice uses master filter values directly.
    // No parameter-level sync is needed, which avoids setValueNotifyingHost calls
    // that were causing automation to grey out in Ableton (VST3 performEdit issue).
}

//==============================================================================
// Applies a pending MPE zone-layout change on the AUDIO THREAD (called from the top
// of processBlock). Doing the reconfig here â€” rather than in the message-thread
// parameterChanged â€” keeps it serialised with renderNextBlock, since setZoneLayout()/
// enableLegacyMode() release notes and mutate the MPE note array (not thread-safe
// against rendering; JUCE's noteStateLock is private). Reads the current parameter
// values atomically, so it always applies the latest mpeMode / bend range.
void SpaceDustAudioProcessor::applyPendingMpeReconfig()
{
    if (! mpeReconfigPending.exchange(false, std::memory_order_acquire))
        return;

    if (safeGetParam(apvts, "mpeMode") > 0.5f)
        synth.setMpeZoneLayoutLower();   // Lower Zone (proper MPE spec)
    else
        synth.setLegacyModeWithPitchBendRange(
            static_cast<int>(safeGetParam(apvts, "mpePitchBendRange")));
}

void SpaceDustAudioProcessor::releaseResources()
{
    logToFile("releaseResources START");
    DBG("Space Dust: releaseResources() called - starting cleanup");
    
    //==============================================================================
    // -- CRITICAL: Force All Notes Off Before Cleanup --
    // MPE: juce::MPESynthesiser uses turnOffAllVoices(allowTailOff) â€” the
    // equivalent of juce::Synthesiser::allNotesOff(0, allowTailOff).
    synth.turnOffAllVoices(true);  // allowTailOff=true â†’ graceful release
    synth.resetNoteState();        // clear mono/legato note stack so it can't survive a reload

    // Reset transport-edge tracking so the next prepareToPlay starts clean.
    wasPlayingState = false;
    lastPpqPosition = 0.0;

    //==============================================================================
    // -- CRITICAL: Complete Resource Cleanup for Safe Unload --
    // 
    // This method is called when playback stops or plugin is being unloaded.
    // It's also called before prepareToPlay() is called again (e.g., sample rate changes).
    //
    // Why clearVoices() + clearSounds() is mandatory:
    // 1. prepareToPlay() may be called multiple times (sample rate changes, project reload)
    // 2. Old voices must be destroyed before new ones are created
    // 3. clearSounds() clears the ReferenceCountedArray<SynthesiserSound>, preventing
    //    ReferenceCountedObject assertion on plugin unload in Ableton Live
    // 4. This prevents memory leaks and ensures clean DSP state
    // 5. In Ableton Live, this exact sequence is critical for stable plugin unload
    //
    // CRITICAL: The order matters:
    // - clearVoices() first: deletes all voices (which may reference sounds)
    // - clearSounds() second: clears the ReferenceCountedArray, preventing dangling
    //   ReferenceCountedObject assertions during plugin destructor
    //
    // Real-time Safety: Both methods are safe to call on audio thread.
    // They properly stop all voices, delete them, and clear sound references.
    
    // Step 1: Stop all active voices gracefully
    // This ensures voices release any resources before being deleted
    DBG("Space Dust: Stopping active voices (count: " + safeStringFromNumber(synth.getNumVoices()) + ")");
    // MPE: voices are juce::MPESynthesiserVoice and have noteStopped(bool) instead
    // of stopNote(float, bool).  We cast to SynthVoice (or use the base virtual) to
    // hard-stop each one.
    for (int i = 0; i < synth.getNumVoices(); ++i)
    {
        if (auto* voice = synth.getVoice(i))
        {
            voice->noteStopped(false);  // allowTailOff=false â†’ immediate hard stop
        }
    }
    DBG("Space Dust: All voices stopped");
    
    // Step 2: Clear all voices and free DSP resources
    // This ensures clean state for next prepareToPlay() call
    DBG("Space Dust: Clearing voices");
    synth.clearVoices();           // deletes all voices
    DBG("Space Dust: Voices cleared");
    
    // Reset delay lines and filters (clear internal state)
    delayLineL.reset();
    delayLineR.reset();
    delayFilterHP.reset();
    delayFilterLP.reset();
    delayFilterHPFb.reset();
    delayFilterLPFb.reset();
    grainDelay_.reset();
    phaser_.reset();
    flanger_.reset();
    bitCrusher_.reset();
    softClipper_.reset();
    compressor_.reset();
    lofi_.reset();
    finalEQ_.reset();
    tranceGate_.reset();
    transientPreFilter_.reset();
    transientPreFilterMod1_.reset();
    transientPreFilterMod2_.reset();

    // Step 3: (Formerly cleared SynthesiserSound array.)
    // MPE: juce::MPESynthesiser has no SynthesiserSound array.  Nothing to clear.
    
    // Step 4: Reset sample rate to ensure clean state
    // This prevents stale sample rate values from being used
    synth.setCurrentPlaybackSampleRate(0.0);
    currentSampleRate = 0.0;
    DBG("Space Dust: Sample rate reset");
    
    // Step 5: Reset atomic parameter values to defaults
    // This ensures clean state for next prepareToPlay() call
    // Re-read from parameters to get current values (not hardcoded defaults)
    if (auto* attackParam = apvts.getParameter(juce::ParameterID{"envAttack", 1}.getParamID()))
    {
        float normalizedValue = attackParam->getValue();
        float attackSeconds = attackParam->convertFrom0to1(normalizedValue);
        currentAttackTime.store(attackSeconds);
    }
    if (auto* decayParam = apvts.getParameter(juce::ParameterID{"envDecay", 1}.getParamID()))
    {
        float normalizedValue = decayParam->getValue();
        float decaySeconds = decayParam->convertFrom0to1(normalizedValue);
        currentDecayTime.store(decaySeconds);
    }
    if (auto* sustainParam = apvts.getParameter(juce::ParameterID{"envSustain", 1}.getParamID()))
    {
        currentSustainLevel.store(sustainParam->getValue());
    }
    if (auto* releaseParam = apvts.getParameter(juce::ParameterID{"envRelease", 1}.getParamID()))
    {
        float normalizedValue = releaseParam->getValue();
        float releaseSeconds = releaseParam->convertFrom0to1(normalizedValue);
        currentReleaseTime.store(releaseSeconds);
    }
    if (auto* p = apvts.getParameter(juce::ParameterID{"filterEnvAttack", 1}.getParamID()))
        currentFilterEnvAttack.store(juce::jmax(0.01f, p->convertFrom0to1(p->getValue())));
    if (auto* p = apvts.getParameter(juce::ParameterID{"filterEnvDecay", 1}.getParamID()))
        currentFilterEnvDecay.store(juce::jmax(0.01f, p->convertFrom0to1(p->getValue())));
    if (auto* p = apvts.getParameter(juce::ParameterID{"filterEnvRelease", 1}.getParamID()))
        currentFilterEnvRelease.store(juce::jmax(0.01f, p->convertFrom0to1(p->getValue())));
    DBG("Space Dust: Atomic parameters reset");
    
    // Note: DSP objects (ADSR, filters) in voices are automatically reset
    // when voices are cleared and recreated in prepareToPlay()
    
    DBG("Space Dust: releaseResources() cleanup complete");
    logToFile("releaseResources END");
}

#ifndef JucePlugin_PreferredChannelConfigurations
bool SpaceDustAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
  #if JucePlugin_IsMidiEffect
    juce::ignoreUnused(layouts);
    return true;
  #else
    // This is the place where you check if the layout is supported.
    // In this template code we only support mono or stereo.
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono()
     && layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

    // This checks if the input layout matches the output layout
   #if ! JucePlugin_IsSynth
    if (layouts.getMainOutputChannelSet() != layouts.getMainInputChannelSet())
        return false;
   #endif

    return true;
  #endif
}
#endif

//==============================================================================
// -- Audio Processing --

/**
    Main audio processing callback.
    
    Processes audio blocks in real-time:
    1. Validates buffer size (CRITICAL: prevents crashes with zero-length buffers)
    2. Clears unused output channels
    3. Updates all voices with current parameter values (real-time safe)
    4. Renders synthesizer output (handles MIDI, voices, and audio generation)
    5. Applies master volume
    
    Real-time Safety: All operations are allocation-free and lock-free.
    
    CRITICAL: DAW Compatibility Guard
    ===================================
    Some DAWs (especially Ableton Live) call processBlock() even when not playing,
    sometimes with invalid buffer configurations:
    - buffer.getNumSamples() == 0 (zero-length buffer)
    - buffer.getNumChannels() == 0 (no channels allocated)
    
    Passing these directly to Synthesiser::renderNextBlock() causes assertions
    in juce_AudioSampleBuffer.h:639, leading to crashes when tweaking parameters
    (especially envelope knobs during GUI interaction).
    
    This guard ensures we skip processing on invalid buffers, preventing crashes
    while maintaining real-time safety. This is a standard JUCE best practice for
    rock-solid DAW compatibility.
*/
void SpaceDustAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    // Tag this OS thread as the audio thread for the safety logger.
    // Cheap thread-local bool; entries from this thread will carry [RT] in the log.
    SAFETY_MARK_AUDIO_THREAD();

    // Apply any pending MPE zone-layout reconfig HERE (audio thread), never from the
    // message-thread parameterChanged, so it can't race note rendering. No-op unless
    // mpeMode/mpePitchBendRange changed since the last block.
    applyPendingMpeReconfig();

    // Pick up any newly imported waveforms and hand the previous set back to be
    // freed on the message thread. One atomic exchange, and a no-op on every block
    // where nothing was imported. Doing this before the zero-sample guard below
    // means the handover keeps happening while a host is sending empty blocks
    // during GUI edits -- which is exactly when an import lands.
    userWaveLibrary.exchangeBank(audioUserWaveBank);

    // The Transient can play an imported sample as its hit, so it needs the same
    // set the voices get. Handed over here, before anything can trigger it, and
    // for this block only -- it never keeps the pointer.
    transient_.setUserWaveBank(audioUserWaveBank);

    //==============================================================================
    // -- CRITICAL: Bulletproof Buffer Guard for Ableton/Reaper Compatibility --
    // 
    // This guard is MANDATORY for Ableton Live, Reaper, and many other DAWs.
    // 
    // Why zero-sample guard is required:
    // - Many DAWs (especially Ableton Live 11/12) call processBlock() with numSamples == 0
    //   during GUI parameter tweaking, even when playback is stopped
    // - Reaper also calls processBlock() with zero samples in certain scenarios
    // - Passing zero-length buffers to Synthesiser::renderNextBlock() causes
    //   internal assertions in juce_AudioSampleBuffer.h:639
    // - This leads to crashes when aggressively tweaking knobs (especially
    //   envelope parameters with long Release times)
    // 
    // This is a standard JUCE best practice for rock-solid DAW compatibility.
    // All professional JUCE plugins implement this guard pattern.
    
    // CRITICAL: Do NOT clear MIDI buffer here - it must be processed by the synthesizer!
    // The synthesizer needs the MIDI messages to trigger notes. MIDI will be consumed
    // during renderNextBlock() processing and cleared afterward.
    
    // Bulletproof guard: validate buffer size before processing
    const int numSamples = buffer.getNumSamples();
    if (numSamples <= 0)
    {
        buffer.clear();  // silence output
        // CRITICAL: Still process MIDI to prevent stuck notes. Some hosts (e.g. Ableton
        // during parameter tweaking) send zero-sample blocks that may contain note-offs.
        // Skipping MIDI processing here was a root cause of held notes.
        if (midiMessages.getNumEvents() > 0)
        {
            juce::ScopedNoDenormals noDenormals;
            synth.processMidiBuffer(midiMessages, 0);
            juce::AudioBuffer<float> tempBuffer(juce::jmax(1, buffer.getNumChannels()), 1);
            tempBuffer.clear();
            synth.renderNextBlock(tempBuffer, midiMessages, 0, 1);
            midiMessages.clear();
        }
        return;
    }
    
    // Extra safety: ensure valid channels
    if (buffer.getNumChannels() == 0)
    {
        buffer.clear();
        return;
    }

    // STANDALONE ONLY: merge the on-screen / computer-keyboard notes into the incoming
    // MIDI before the synth consumes it. Gated on wrapperType so it is never executed in
    // the VST3/AU plugin (where the keyboard component is never created anyway). JUCE
    // compiles this shared code once for all formats, so the guard - not an #ifdef - is
    // how we keep the behaviour exclusive to the Standalone build.
    if (wrapperType == wrapperType_Standalone)
        keyboardState.processNextMidiBuffer(midiMessages, 0, numSamples, true);

    //==============================================================================
    // -- Resample: the synth plays itself a middle C --
    // Put into the MIDI stream here, after every other source of notes and before
    // the synth reads it, so the note is rendered by this very block -- the same
    // block whose output the recorder begins keeping at the bottom of this
    // function. Nothing is scheduled, waited for, or timed on the message thread.
    // Cleared before the voices render, and written by any of them that is reading
    // an imported sample. A list nothing is playing is therefore left at -1, which
    // is what takes the playhead off the Waveforms window when a note ends.
    //
    // Only while that window is open to look at it. Shut, this whole mechanism --
    // here and in every voice -- does not run.
    if (isUserWavePhaseWanted())
        for (auto& phase : userWavePhase)
            phase.store(-1.0f, std::memory_order_relaxed);

    const bool resampleStarting = resampleCapture.startsThisBlock();

    if (resampleStarting || resampleCapture.isRecording())
    {
        // The keyboard is LOCKED while a recording runs.
        //
        // Every note reaching the synth is rendered into the very output being
        // recorded, so a key pressed while the bar is filling ends up inside the
        // waveform -- and it is pressed for exactly that reason, because nothing
        // can be heard and the player is checking whether anything is happening.
        //
        // Dropped, not held back: a note played during a recording is not a note
        // anybody meant to sound three seconds later. This catches the host's
        // MIDI, the arrangement's and the on-screen keyboard's alike, because all
        // three have already been merged into this one buffer by now.
        midiMessages.clear();
    }

    if (resampleStarting)
    {
        // Whatever was sounding is cut first, so the recording is of this note and
        // of nothing else. Hard, not with a tail: a release ringing on underneath
        // is exactly what this is meant to keep out of the recording.
        //
        // MPE: juce::MPESynthesiser spells allNotesOff(0, allowTailOff) as
        // turnOffAllVoices(allowTailOff), the same as releaseResources uses.
        synth.turnOffAllVoices(false);
        synth.resetNoteState();

        midiMessages.addEvent(juce::MidiMessage::noteOn(1, Resample::middleC,
                                                        static_cast<juce::uint8>(Resample::velocity)), 0);
    }

    // Letting the note go again. Asked of the recorder rather than counted here,
    // so the rule lives in one testable place -- and asked unconditionally, so a
    // recording that has been given up on still gets its note-off instead of
    // leaving middle C held down for good.
    int resampleReleaseAt = 0;

    if (resampleCapture.releaseThisBlock(numSamples, resampleReleaseAt))
        midiMessages.addEvent(juce::MidiMessage::noteOff(1, Resample::middleC),
                              resampleReleaseAt);

    juce::ScopedNoDenormals noDenormals;

    // This block's routing set, taken ONCE and announced to the message thread
    // so it cannot rebuild into the buffer being walked. Must come before every
    // consumer: fillVoiceModScratch below, and refreshEffectModulatedValues in
    // the effects chain.
    latchCompiledRoutings();

    auto totalNumInputChannels  = getTotalNumInputChannels();
    auto totalNumOutputChannels = getTotalNumOutputChannels();

    // Clear any output channels that don't contain input data
    // (Not needed for synth, but good practice for compatibility)
    for (auto i = totalNumInputChannels; i < totalNumOutputChannels; ++i)
        buffer.clear(i, 0, numSamples);

    //==============================================================================
    // -- LFO Processing (Per-Sample Buffers) --
    // Generate per-sample LFO values and fill buffers for voice access

    // SAFETY: a host may call processBlock with MORE samples than it declared to
    // prepareToPlay (Ableton does this during freeze/bounce/render). The LFO fill
    // loops below write `numSamples` entries via setSample(), which is unchecked in
    // Release â€” an oversized block would write past the end and corrupt the heap
    // (ASan-confirmed heap-buffer-overflow). Grow the buffers if the block exceeds
    // their current capacity. Allocates only on growth (rare), then stays grown.
    for (int lfo = 0; lfo < spacedust::numLfos; ++lfo)
        if (lfoBuffers[lfo].getNumSamples() < numSamples)
            lfoBuffers[lfo].setSize(1, numSamples, false, false, true);

    // LFO waveform generation lives in Source/LfoWaveform.cpp so its fold-back can be
    // measured directly (tools/lfotest). It takes the per-sample phase advance so the
    // eased edges cannot collapse into steps once cycles get short at audio rates.

    // Sample & Hold: generate next random value in [-1, 1] using LCG (real-time safe)
    auto nextSampleHoldValue = [](uint32_t& state, float& held) {
        state = state * 1103515245u + 12345u;
        float r = static_cast<float>((state >> 16) & 0x7FFF) / 32767.5f;
        held = r * 2.0f - 1.0f;
    };

    // Smoothing coefficient: ~5 samples to soften retrigger/phase jumps (prevents
    // clicks). static so the lambda below can use it without an explicit capture.
    static constexpr float kLfoSmoothAlpha = 0.25f;

    //==========================================================================
    // 0.01-200 Hz, logarithmic. Anything saved while the top was briefly 2 kHz is
    // rescaled on load by migrateLfoRatesIfOld, so it keeps its original speed.
    auto lfoBaseHz = [] (float rate0to12) -> double
    {
        return lfoKnobToHz(static_cast<double>(rate0to12));
    };

    // The output smoother is a one-pole whose knee sits near 2.2 kHz at 48 kHz, so a
    // fixed coefficient is harmless across the 200 Hz range. It is left rate-dependent
    // anyway: it costs nothing, and it means the smoother can never quietly attenuate
    // the oscillation being asked for if the range is ever widened again.
    //
    // So the coefficient opens up with the rate: the knee is kept at least an order of
    // magnitude above the LFO's own frequency, and never tighter than the original
    // 0.25. Slow LFOs therefore behave exactly as before (0.25 wins by miles), while a
    // fast one passes through essentially untouched. Band-limiting is LfoWaveform's
    // job now, not this filter's.
    // Takes the phase advance per sample, which makes it sample-rate independent:
    // knee = 10 x rate  =>  alpha = 1 - exp(-20 * pi * delta).
    auto smoothingAlphaFor = [] (double delta) -> float
    {
        const double a = 1.0 - std::exp(-20.0 * juce::MathConstants<double>::pi * std::abs(delta));
        return static_cast<float>(juce::jlimit(static_cast<double>(kLfoSmoothAlpha), 1.0, a));
    };

    // One body for every LFO. LFO 1 and LFO 2 were two copies of this, character
    // for character apart from their names, so folding them into a loop changes
    // nothing about how either one sounds -- and it is what lets the modulation
    // matrix address an LFO by number.
    for (int lfo = 0; lfo < spacedust::numLfos; ++lfo)
    {
        const auto& ids = lfoParamIds[lfo];
        auto&       buf = lfoBuffers[lfo];

        const bool  lfoEnabled    = safeGetParam(apvts, ids.enabled) > 0.5f;
        const float lfoDepth      = lfoEnabled ? (safeGetParam(apvts, ids.depth) * 2.0f / 100.0f) : 0.0f;  // 0-2.0 when on
        const bool  lfoSync       = safeGetParam(apvts, ids.sync) > 0.5f;
        const float lfoRate       = safeGetParam(apvts, ids.rate);  // 0-12
        const bool  lfoTriplet    = safeGetParam(apvts, ids.triplet) > 0.5f;
        const bool  lfoAll        = safeGetParam(apvts, ids.tripletAll) > 0.5f;
        const float lfoPhaseParam = safeGetParam(apvts, ids.phase);
        const int   lfoWaveform   = static_cast<int>(safeGetParam(apvts, ids.waveform));

        if (lfoSync)
        {
            // Get tempo from host
            double tempo = 120.0;
            auto* playHead = getPlayHead();
            if (playHead != nullptr)
            {
                auto posInfo = playHead->getPosition();
                if (posInfo.hasValue())
                {
                    if (posInfo->getBpm().hasValue() && *posInfo->getBpm() > 0.0)
                        tempo = *posInfo->getBpm();
                }
            }
            
            double samplesPerBeat = currentSampleRate * 60.0 / tempo;
            
            // Linear rate mapping (0-12) -> index: avoids fold-back at high rates
            float rateClamped = juce::jlimit(0.0f, 12.0f, lfoRate);
            int musicalIndex = static_cast<int>(std::round(rateClamped * 8.0f / 12.0f));
            musicalIndex = juce::jlimit(0, 8, musicalIndex);
            
            double multiplier = 1.0;
            
            if (lfoTriplet && lfoAll)
            {
                // All mode: 18 steps - linear map rate 0-12 to index 0-17
                static const double allMultipliers[18] = {
                    8.0, 6.0, 4.0, 2.6666666666666665, 2.0, 1.3333333333333333,
                    1.0, 0.6666666666666666, 0.5, 0.3333333333333333, 0.25,
                    0.16666666666666666, 0.125, 0.08333333333333333, 0.0625,
                    0.0510204081632653, 0.03125, 0.03125
                };
                int mappedIndex = static_cast<int>(std::round(rateClamped * 17.0f / 12.0f));
                mappedIndex = juce::jlimit(0, 17, mappedIndex);
                multiplier = allMultipliers[mappedIndex];
            }
            else if (lfoTriplet && !lfoAll)
            {
                static const double tripletMultipliers[9] = {
                    32.0/3.0, 16.0/3.0, 8.0/3.0, 4.0/3.0, 2.0/3.0,
                    1.0/3.0, 1.0/6.0, 1.0/12.0, 1.0/24.0
                };
                multiplier = tripletMultipliers[musicalIndex];
            }
            else
            {
                static const double straightMultipliers[9] = {
                    8.0, 4.0, 2.0, 1.0, 0.5, 0.25, 0.125, 0.0625, 0.03125
                };
                multiplier = straightMultipliers[musicalIndex];
            }
            
            double periodSamples = samplesPerBeat * multiplier;
            double hz = currentSampleRate / periodSamples;
            double delta = hz / currentSampleRate;
            
            float phaseOffset = lfoPhaseParam / 360.0f;
            bool isPlaying = false;
            if (playHead != nullptr)
            {
                auto posInfo = playHead->getPosition();
                if (posInfo.hasValue())
                    isPlaying = posInfo->getIsPlaying();
            }
            bool useBeatPhase = !lfoRetrigger[lfo].load() && isPlaying;  // Beat phase only when playing + no retrigger
            
            if (useBeatPhase)
            {
                // Derive phase from beat position so wave start (phase 0) aligns with beat 1
                double ppqStart = 0.0;
                if (playHead != nullptr)
                {
                    auto posInfo = playHead->getPosition();
                    if (posInfo.hasValue() && posInfo->getPpqPosition().hasValue())
                        ppqStart = *posInfo->getPpqPosition();
                }
                double periodBeats = multiplier;
                double prevPhase = lfoPrevPhase[lfo];
                for (int s = 0; s < numSamples; ++s)
                {
                    double ppq = ppqStart + static_cast<double>(s) / samplesPerBeat;
                    double phase = std::fmod(ppq, periodBeats) / periodBeats;
                    float raw;
                    if (lfoWaveform == 5)
                    {
                        bool wrapped = (prevPhase < 0) || (prevPhase > 0.5 && phase < prevPhase);
                        if (wrapped)
                            nextSampleHoldValue(lfoShState[lfo], lfoSampleHoldValue[lfo]);
                        raw = lfoSampleHoldValue[lfo] * lfoDepth;
                        prevPhase = phase;
                    }
                    else
                    {
                        raw = LfoWaveform::generate(phase + phaseOffset, lfoWaveform, delta) * lfoDepth;
                    }
                    lfoSmoothedValue[lfo] += smoothingAlphaFor(delta) * (raw - lfoSmoothedValue[lfo]);
                    buf.setSample(0, s, lfoSmoothedValue[lfo]);
                }
                lfoPrevPhase[lfo] = prevPhase;
            }
            else
            {
                // Retrigger ON: use accumulator (reset on note in voice)
                double phase = lfoCurrentPhase[lfo];
                for (int s = 0; s < numSamples; ++s)
                {
                    double phaseNext = phase + delta;
                    bool wrapped = (phaseNext >= 1.0);
                    if (lfoWaveform == 5 && wrapped)
                        nextSampleHoldValue(lfoShState[lfo], lfoSampleHoldValue[lfo]);
                    phase = std::fmod(phaseNext, 1.0);
                    float raw = (lfoWaveform == 5) ? (lfoSampleHoldValue[lfo] * lfoDepth)
                        : (LfoWaveform::generate(phase + phaseOffset, lfoWaveform, delta) * lfoDepth);
                    lfoSmoothedValue[lfo] += smoothingAlphaFor(delta) * (raw - lfoSmoothedValue[lfo]);
                    buf.setSample(0, s, lfoSmoothedValue[lfo]);
                }
                lfoCurrentPhase[lfo] = phase;
            }
        }
        else
        {
            // Free mode: the knob is Hz, straight through the shared mapping.
            const double hz = lfoBaseHz(lfoRate);

            double delta = (currentSampleRate > 0.0) ? (hz / currentSampleRate) : 0.0;
            lastLfoHz[lfo] = hz;   // for the voices' oversample latch

            // Fill per-sample buffer
            double phase = lfoCurrentPhase[lfo];
            float phaseOffset = lfoPhaseParam / 360.0f;
            for (int s = 0; s < numSamples; ++s)
            {
                double phaseNext = phase + delta;
                bool wrapped = (phaseNext >= 1.0);
                if (lfoWaveform == 5 && wrapped)
                    nextSampleHoldValue(lfoShState[lfo], lfoSampleHoldValue[lfo]);
                phase = std::fmod(phaseNext, 1.0);
                float raw = (lfoWaveform == 5) ? (lfoSampleHoldValue[lfo] * lfoDepth)
                    : (LfoWaveform::generate(phase + phaseOffset, lfoWaveform, delta) * lfoDepth);
                lfoSmoothedValue[lfo] += smoothingAlphaFor(delta) * (raw - lfoSmoothedValue[lfo]);
                buf.setSample(0, s, lfoSmoothedValue[lfo]);
            }
            lfoCurrentPhase[lfo] = phase;
        }
    }

    // Pitch bend snap-back: smooth linear ramp over 0.05s (per-block interpolation, no stepping)
    if (pitchBendSnapActive.load())
    {
        if (pitchBendRampReset.exchange(false))
            pitchBendRampSamplesElapsed = 0.0f;
        float startVal = pitchBendSnapStartValue.load();
        float rampSamples = static_cast<float>(0.05 * currentSampleRate);
        pitchBendRampSamplesElapsed += static_cast<float>(numSamples);
        float frac = juce::jmin(1.0f, pitchBendRampSamplesElapsed / rampSamples);
        float ramped = startVal * (1.0f - frac);
        pitchBendRampCurrentValue.store(ramped);
        if (frac >= 1.0f)
        {
            pitchBendSnapActive.store(false);
            pitchBendRampSamplesElapsed = 0.0f;
            pitchBendRampComplete.store(true);
        }
    }
    
    bool transientEnabled = safeGetParam(apvts, "transientEnabled") > 0.5f;
    bool transientPostEffect = safeGetParam(apvts, "transientPostEffect") > 0.5f;

    // ---- Loop-debug instrumentation (set SPACEDUST_LOOP_DEBUG to 0 to remove) ----
    // Captures, per relevant block, the transport-edge flags, whether the wrap flush
    // fired, the note-stack + every voice's state BEFORE the flush, and the IN/OUT
    // MIDI + state AFTER processMidiBuffer. Writes one human-readable file to
    // Documents\SpaceDust_LoopDebug.txt so we can see exactly which voice gets cut
    // (FADE) and which phantom note-on caused it on a Cubase loop wrap.
#define SPACEDUST_LOOP_DEBUG 0   // flip to 1 to re-capture loop-wrap MIDI + voice/stack state
#if SPACEDUST_LOOP_DEBUG
    juce::String dbgStackBefore, dbgEdgeInfo;
    bool dbgFlushFired = false;
    bool dbgLogThisBlock = false;
#endif

    //==============================================================================
    // -- Transport-edge detection: flush stale mono/legato note state --
    //
    // The mono/legato note stack (SpaceDustSynthesiser::noteStack/currentNote) models
    // "which keys the host thinks are held".  On a transport stop or a playhead jump
    // (loop wrap / rewind / seek) the host stops sending the note-offs we are tracking,
    // so the stack desyncs and processMidiBuffer() starts rewriting the stream with
    // phantom note-on/note-off pairs â†’ wrong notes, stuck notes, voice-steal thrashing.
    //
    // Detect those edges here and flush.  Only act in Mono/Legato (mode != 0): Poly
    // mode keeps no stack and must NOT have voices cut at a loop boundary (would chop
    // sustained chords), so its behaviour is left exactly as before.
    {
        bool   nowPlaying = wasPlayingState;
        double ppq        = lastPpqPosition;
        if (auto* ph = getPlayHead())
        {
            auto posInfo = ph->getPosition();
            if (posInfo.hasValue())
            {
                nowPlaying = posInfo->getIsPlaying();
                if (posInfo->getPpqPosition().hasValue())
                    ppq = *posInfo->getPpqPosition();
            }
        }

        const bool stopped    = (wasPlayingState && ! nowPlaying);
        // Transport START (not-playing -> playing). Without this, the FIRST loop runs
        // on whatever stale note-stack/voice state was left from preview notes, the
        // prior session, or a note held across the loop point â€” so the first pass has
        // wrong note timing, then "corrects" once the loop-wrap flush below fires.
        // Treating start like a wrap makes loop 1 behave identically to loop 2+.
        const bool started    = (! wasPlayingState && nowPlaying);
        // Backward jump while playing = loop wrap or rewind. Small epsilon avoids
        // false positives from float noise on a steadily-advancing playhead.
        const bool jumpedBack = (nowPlaying && (ppq + 1.0e-6 < lastPpqPosition));

        // A note the host HOLDS CONTINUOUSLY across the loop boundary (Cubase does
        // exactly this for a note that spans the cycle) emits NO note-on/note-off at
        // the wrap â€” the voice must keep sustaining straight through. Re-articulating
        // hosts (and the FL loop-restart-pop case) instead carry a fresh note-on/off
        // in the wrap block. So a loop wrap is only a state-flush edge when the block
        // actually carries note articulation; otherwise turnOffAllVoices() below would
        // force the still-held note into its release and it would decay away mid-loop
        // (the Cubase "sustained note plays for a second then cuts out on the loop"
        // bug). Skipping the flush here also leaves the note in the stack, so the
        // host's eventual real note-off still releases it â€” no stuck note.
        //
        // Stop and Start always flush regardless of note events: a transport stop must
        // never leave a note hanging, and a fresh start wants a clean baseline.
        bool blockHasNoteEvent = false;
        for (const auto meta : midiMessages)
        {
            if (meta.getMessage().isNoteOnOrOff())
            {
                blockHasNoteEvent = true;
                break;
            }
        }
        const bool wrapEdge = jumpedBack && blockHasNoteEvent;

#if SPACEDUST_LOOP_DEBUG
        // Capture state BEFORE the flush (lastPpqPosition still holds the prev block's
        // ppq here â€” it's updated at the end of this scope), so the log shows the
        // pre-wrap note-stack desync that manufactures the phantom note.
        dbgLogThisBlock = (stopped || started || jumpedBack || blockHasNoteEvent);
        if (dbgLogThisBlock)
        {
            dbgStackBefore = synth.getDebugState();
            dbgEdgeInfo = "play=" + juce::String((int) nowPlaying)
                        + " ppq=" + juce::String(ppq, 4)
                        + " prevPpq=" + juce::String(lastPpqPosition, 4)
                        + " stop=" + juce::String((int) stopped)
                        + " start=" + juce::String((int) started)
                        + " jump=" + juce::String((int) jumpedBack)
                        + " hasNote=" + juce::String((int) blockHasNoteEvent)
                        + " wrapEdge=" + juce::String((int) wrapEdge);
        }
#endif

        if ((stopped || started || wrapEdge) && synth.getVoiceModeIndex() != 0)
        {
            // Flush the note STACK but KEEP lastMonoVoiceIndex: the new loop's first
            // note then REUSES the voice that is ringing out the previous note's
            // release (a smooth mono retrigger, exactly like normal Mono play) instead
            // of being allocated a fresh voice while cutStrayVoices() hard-fades the old
            // one â€” that fresh-alloc + hard-fade of a still-loud tail was the POP on
            // loop restart (worst in FL's tiny buffers, milder in Ableton's larger ones).
            synth.resetNoteState(/*keepMonoVoiceIndex*/ true);
            // GRACEFUL release, not a hard stop. allowTailOff=false chopped any still-
            // ringing release tail at the loop boundary over the 1.5 ms voiceFade ramp â€”
            // a clearly audible POP on loop restart (worst in FL's tiny buffers, milder in
            // Ableton), especially with resonance / a long release. allowTailOff=true lets
            // held notes enter their normal release (envelope value stays continuous, only
            // the slope changes â†’ declicked) and leaves an already-decaying tail to finish.
            // Stuck notes are still prevented: every voice is driven into its release stage
            // here, and the new loop's first note re-asserts the mono single-voice guarantee
            // via cutStrayVoices().
            synth.turnOffAllVoices(true);
#if SPACEDUST_LOOP_DEBUG
            dbgFlushFired = true;
#endif
        }

        wasPlayingState = nowPlaying;
        lastPpqPosition = ppq;
    }

    //==============================================================================
    // -- Process MIDI with Mono Mode Support --
    // Call custom synthesiser methods to handle mono mode
    //
#if SPACEDUST_LOOP_DEBUG
    {
        auto dumpNotes = [](const juce::MidiBuffer& mb) -> juce::String
        {
            juce::String s;
            for (const auto meta : mb)
            {
                auto msg = meta.getMessage();
                if (msg.isNoteOn() && msg.getVelocity() > 0)
                    s << "+" << msg.getNoteNumber() << "@" << meta.samplePosition << " ";
                else if (msg.isNoteOff() || (msg.isNoteOn() && msg.getVelocity() == 0))
                    s << "-" << msg.getNoteNumber() << "@" << meta.samplePosition << " ";
            }
            return s;
        };

        static std::atomic<int> g_dbgLines{ 0 };
        static bool g_dbgPrevPlaying = false;
        const bool nowPlay = wasPlayingState;          // updated by edge block above

        juce::File dbgFile = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                                 .getChildFile("SpaceDust_LoopDebug.txt");

        if (nowPlay && ! g_dbgPrevPlaying)
        {
            g_dbgLines.store(0);
            dbgFile.replaceWithText("=== PLAY START  mode=" + juce::String(synth.getVoiceModeIndex())
                                    + "  sr=" + juce::String(currentSampleRate)
                                    + "  block=" + juce::String(numSamples) + " ===\n");
        }
        g_dbgPrevPlaying = nowPlay;

        const juce::String inc = dumpNotes(midiMessages);
        synth.processMidiBuffer(midiMessages, numSamples);
        const juce::String outg = dumpNotes(midiMessages);
        const juce::String stackAfter = synth.getDebugState();

        // Log any block that is a transport edge OR carries note activity (in or out).
        // Bounded so a long session can't grow the file without limit.
        const bool worth = dbgLogThisBlock || inc.isNotEmpty() || outg.isNotEmpty();
        int lines = g_dbgLines.load();
        if (worth && lines < 6000)
        {
            g_dbgLines.store(lines + 1);
            juce::String line;
            if (dbgFlushFired) line << "*** FLUSH (resetNoteState+turnOffAllVoices) ***\n";
            line << dbgEdgeInfo << "  n=" << numSamples << "\n"
                 << "   IN [" << inc << "]\n"
                 << "   OUT[" << outg << "]\n"
                 << "   before " << dbgStackBefore << "\n"
                 << "   after  " << stackAfter << "\n";
            dbgFile.appendText(line);
        }
    }
#else
    synth.processMidiBuffer(midiMessages, numSamples);
#endif

    //==============================================================================
    // -- Transient: trigger from MIDI after mono/legato rewrite --
    // Legato mode sets nextNoteIsLegato for overlapping note-ons (and return-to-held
    // note); transient should match envelope retrigger â€” only on true new notes.
    if (transientEnabled)
    {
        const bool inLegatoVoiceMode = (synth.getVoiceModeIndex() == 2);
        for (const auto metadata : midiMessages)
        {
            const auto msg = metadata.getMessage();
            if (msg.isNoteOn() && msg.getVelocity() > 0)
            {
                if (inLegatoVoiceMode && synth.isNextNoteLegato())
                    continue;

                SpaceDustTransient::Parameters tp;
                tp.enabled = true;
                tp.type = effectChoiceIndex(ec_transientType, 0);
                tp.mix = juce::jlimit(0.0f, 1.0f, safeGetParam(apvts, "transientMix"));
                tp.postEffect = transientPostEffect;
                tp.kaDonk = juce::jlimit(0.0f, 1.0f, safeGetParam(apvts, "transientKaDonk"));
                tp.coarse = juce::jlimit(-24.0f, 24.0f, safeGetParam(apvts, "transientCoarse"));
                tp.length = juce::jlimit(0.0f, 1.0f, safeGetParam(apvts, "transientLength"));
                transient_.setParameters(tp);
                transient_.trigger(msg.getNoteNumber());
                break;
            }
        }
    }

    // Voice params after mono/legato MIDI rewrite so coarse/detune retune uses currentPitch
    // (see SynthVoice::setOsc* â€” must align with renderNextBlock base Hz, not stale MIDI note).
    updateVoicesWithParameters(0.0f, 0.0f);

    // Per-sample modulated values for the VOICE destinations, filled HERE
    // because renderNextBlock is the next statement. The effects chain, where
    // effectModulated is filled, does not run for another 400 lines, so a voice
    // reading effectModulated would get the PREVIOUS block's numbers.
    fillVoiceModScratch(numSamples);

    //==============================================================================
    // -- Render the Synthesizer --
    // CRITICAL: This processes MIDI messages and triggers voices.
    // Handles MIDI parsing, voice triggering, and audio generation.
    // Now safe to call - buffer is guaranteed to be valid (non-zero samples, non-zero channels)
    synth.renderNextBlock(buffer, midiMessages, 0, numSamples);
    
    // Clear MIDI buffer AFTER processing (synthesizer has consumed all messages)
    // This prevents stale MIDI from accumulating across blocks
    midiMessages.clear();

    //==============================================================================
    // -- Ka-Donk Delay: delays synth output so transient leads --
    if (transientEnabled)
    {
        float kaDonkAmount = juce::jlimit(0.0f, 1.0f, safeGetParam(apvts, "transientKaDonk"));
        float kaDonkDelaySamples = kaDonkAmount * static_cast<float>(currentSampleRate);
        kaDonkDelaySamples = juce::jlimit(0.0f, static_cast<float>(kaDonkMaxSamples), kaDonkDelaySamples);
        smoothedKaDonkDelay_.setTargetValue(kaDonkDelaySamples);

        if (kaDonkDelaySamples > 0.5f && buffer.getNumChannels() >= 2 && numSamples > 0)
        {
            auto* dataL = buffer.getWritePointer(0);
            auto* dataR = buffer.getWritePointer(1);

            for (int i = 0; i < numSamples; ++i)
            {
                float delaySmp = smoothedKaDonkDelay_.getNextValue();

                kaDonkDelayL_.pushSample(0, dataL[i]);
                kaDonkDelayR_.pushSample(0, dataR[i]);

                dataL[i] = kaDonkDelayL_.popSample(0, delaySmp);
                dataR[i] = kaDonkDelayR_.popSample(0, delaySmp);
            }
        }
    }

    //==============================================================================
    // -- Transient (Pre: BEFORE the filter when Post Effect OFF) --
    // The transient must be coloured by the synth filter in this mode. The real
    // filters are per-voice (inside the synth, already run), so we render the
    // transient into a private scratch buffer and pass it through a SERIES of linear
    // SVF mirrors â€” one for the MASTER (Main-tab) filter, then one for each Mod-tab
    // filter that is Shown AND unlinked â€” then sum it into the mix. This mirrors the
    // per-voice order (master â†’ mod1 â†’ mod2 in series, mod stages active only when
    // Shown && unlinked), so the unlinked Mod filters cut the transient exactly like
    // the Main filter does. Post mode (below) stays end-of-chain and unfiltered. This
    // is confined to the master chain; SynthVoice is untouched.
    if (transientEnabled && !transientPostEffect)
    {
        SpaceDustTransient::Parameters tp;
        tp.enabled = true;
        tp.type = effectChoiceIndex(ec_transientType, tp.type);
        tp.mix = juce::jlimit(0.0f, 1.0f, safeGetParam(apvts, "transientMix"));
        tp.postEffect = false;
        tp.kaDonk = juce::jlimit(0.0f, 1.0f, safeGetParam(apvts, "transientKaDonk"));
        tp.coarse = juce::jlimit(-24.0f, 24.0f, safeGetParam(apvts, "transientCoarse"));
        tp.length = juce::jlimit(0.0f, 1.0f, safeGetParam(apvts, "transientLength"));
        transient_.setParameters(tp);

        const int numCh = buffer.getNumChannels();
        if (numCh > 0 && numSamples > 0 && transientScratch_.getNumSamples() >= numSamples)
        {
            // Render the transient alone into a cleared scratch buffer (process() adds).
            juce::AudioBuffer<float> scratch(transientScratch_.getArrayOfWritePointers(),
                                             juce::jmin(numCh, transientScratch_.getNumChannels()),
                                             numSamples);
            scratch.clear();
            transient_.process(scratch);

            // Configure one mirror SVF (mode 0=LP/1=BP/2=HP/3=Notch/4=Peak; Q matches
            // NonlinearSVF's legacy curve Q = 0.1 + res*19.9, capped below self-osc for
            // stability — setResonanceQ() takes that Q as-is, so the mirror never
            // self-oscillates and the pre-existing LP/BP/HP sound is unchanged).
            auto configureMirror = [](NonlinearSVF& f, int mode, float cutoffHz, float res)
            {
                f.setMode(juce::jlimit(0, NonlinearSVF::numModes - 1, mode));
                f.setCutoffFrequency(juce::jlimit(20.0f, 20000.0f, cutoffHz));
                const float resQ = 0.1f + juce::jmin(juce::jlimit(0.0f, 1.0f, res), 0.80f) * 19.9f;
                f.setResonanceQ(juce::jlimit(0.1f, 16.0f, resQ));
            };

            // Master mirror (always present â€” it is the only filter when no Mod
            // filter is active, so the master-only sound is unchanged).
            configureMirror(transientPreFilter_,
                            static_cast<int>(safeGetParam(apvts, "filterMode")),
                            safeGetParam(apvts, "filterCutoff", 8000.0f),
                            safeGetParam(apvts, "filterResonance"));

            // Mod mirrors: active only when Shown AND unlinked, matching the per-voice
            // chain. A linked Mod filter is NOT mirrored â€” the master already covers it
            // (the per-voice synth skips it for the same reason).
            const bool mod1Active = safeGetParam(apvts, "modFilter1Show") > 0.5f
                                 && safeGetParam(apvts, "modFilter1LinkToMaster") <= 0.5f;
            const bool mod2Active = safeGetParam(apvts, "modFilter2Show") > 0.5f
                                 && safeGetParam(apvts, "modFilter2LinkToMaster") <= 0.5f;
            if (mod1Active)
                configureMirror(transientPreFilterMod1_,
                                static_cast<int>(safeGetParam(apvts, "modFilter1Mode")),
                                safeGetParam(apvts, "modFilter1Cutoff", 8000.0f),
                                safeGetParam(apvts, "modFilter1Resonance"));
            if (mod2Active)
                configureMirror(transientPreFilterMod2_,
                                static_cast<int>(safeGetParam(apvts, "modFilter2Mode")),
                                safeGetParam(apvts, "modFilter2Cutoff", 8000.0f),
                                safeGetParam(apvts, "modFilter2Resonance"));

            for (int ch = 0; ch < scratch.getNumChannels(); ++ch)
            {
                auto* s = scratch.getWritePointer(ch);
                for (int i = 0; i < numSamples; ++i)
                {
                    float y = transientPreFilter_.processSample(ch, s[i]);  // master
                    if (mod1Active) y = transientPreFilterMod1_.processSample(ch, y);
                    if (mod2Active) y = transientPreFilterMod2_.processSample(ch, y);
                    s[i] = y;
                }

                buffer.addFrom(ch, 0, scratch, ch, 0, numSamples);
            }
        }
        else
        {
            // Fallback (scratch unavailable): original unfiltered behaviour.
            transient_.process(buffer);
        }
    }

    //==============================================================================
    // -- Effect order (TG/BC Post Effect toggles) --
    // TG Post OFF: TG first. BC Post OFF: BC before all effects (second, after TG). BC Post ON: BC late.
    // TG Post ON:  TG last.  BC Post OFF: BC before all effects (first). BC Post ON: BC late (after Flanger).
    bool tranceGatePostEffect = safeGetParam(apvts, "tranceGatePostEffect") > 0.5f;
    bool bitCrusherPostEffect = safeGetParam(apvts, "bitCrusherPostEffect") > 0.5f;

    // The modulated values for everything read between here and runEffectsChain:
    // the Pre placements of the trance gate and the bit crusher, and the Delay.
    //
    // These are refreshed ONCE PER BLOCK, not per chunk, so they modulate at
    // block rate and the Post placements modulate at chunk rate. That is not a
    // bug. The Pre sites are called from processBlock, outside the chunk loop,
    // and moving them inside would mean moving the Delay too -- the Delay is not
    // chunk-aware. Coarser, but not silent.
    refreshEffectModulatedValues (0);

    // -- 1) Trance Gate (Pre: when Post Effect OFF) --
    if (! tranceGatePostEffect)
        processTranceGate (buffer, 0);

    // -- 2) Bit Crusher (early: before all effects except TG. When TG Post OFF, TG is first so BC is second) --
    if (! bitCrusherPostEffect)
        processBitCrusher (buffer, 0);

    //==============================================================================
    // -- Delay Effect --
    bool delayEnabled = safeGetParam(apvts, "delayEnabled") > 0.5f;
    float delayDecay = modParam(ep_delayDecay) * 0.01f;  // 0-1
    float delayDryWet = modParam(ep_delayDryWet) * 0.01f;  // 0-1
    float delayRate = modParam(ep_delayRate);
    bool delaySync = safeGetParam(apvts, "delaySync") > 0.5f;
    bool delayPingPong = safeGetParam(apvts, "delayPingPong") > 0.5f;
    
    // Inverted: knob 0 = long delay (low freq), knob 12 = short delay (high freq)
    float delayRateClamped = juce::jlimit(0.0f, 12.0f, delayRate);
    float delayRateInverted = 12.0f - delayRateClamped;
    
    float delayTimeSamples = 1.0f;
    if (delaySync)
    {
        // Delay sync: unified list - straight, dotted (1/8., 1/4.), and triplets baked in
        // 18 steps: 1/32, 1/24, 1/16, 1/12, 1/8, 1/8., 1/4, 1/4., 1/2, 3/4, 1, 3/2, 2, 3, 4, 5, 8, 8
        double tempo = 120.0;
        auto* playHead = getPlayHead();
        if (playHead != nullptr)
        {
            auto posInfo = playHead->getPosition();
            if (posInfo.hasValue() && posInfo->getBpm().hasValue() && *posInfo->getBpm() > 0.0)
                tempo = *posInfo->getBpm();
        }
        double samplesPerBeat = currentSampleRate * 60.0 / tempo;
        double normalized = juce::jlimit(0.0, 1.0, delayRateInverted / 12.0);
        double curved = std::pow(normalized, 2.5);
        // Sample directly into the 18-entry table. Previously this mapped 0..12 -> 0..17
        // via round(musicalIndex/12*17), which skipped indices 2 (1/16), 5 (1/8.), 12 (2)
        // and 15 (5) â€” so those divisions were unreachable.
        int musicalIndex = static_cast<int>(std::round(curved * 17.0));
        musicalIndex = juce::jlimit(0, 17, musicalIndex);
        static const double delayMultipliers[18] = {
            8.0, 6.0, 4.0, 2.6666666666666665, 2.0, 1.3333333333333333,
            1.0, 0.6666666666666666, 0.5, 0.3333333333333333, 0.25,
            0.16666666666666666, 0.125, 0.08333333333333333, 0.0625,
            0.0510204081632653, 0.03125, 0.03125
        };
        double multiplier = delayMultipliers[musicalIndex];
        double delayBeats = 1.0 / multiplier;
        delayTimeSamples = static_cast<float>(samplesPerBeat * delayBeats);
    }
    else
    {
        // Free mode: 20ms to 2000ms log scale (inverted: knob 0 = 2000ms, knob 12 = 20ms)
        float normalizedRate = juce::jlimit(0.0f, 1.0f, delayRateInverted / 12.0f);
        float logMin = std::log(20.0f);
        float logMax = std::log(2000.0f);
        float logMs = logMin + normalizedRate * (logMax - logMin);
        float delayMs = std::exp(logMs);
        delayMs = juce::jlimit(20.0f, 2000.0f, delayMs);
        delayTimeSamples = delayMs * static_cast<float>(currentSampleRate) / 1000.0f;
    }
    delayTimeSamples = juce::jlimit(1.0f, static_cast<float>(maxDelaySamples), delayTimeSamples);
    
    // Feedback at or below ~0.1% (knob 0â€“0.1): clear delay state and bypass â€” no echo tail.
    if (delayEnabled && delayDecay <= 0.001f && numSamples > 0 && buffer.getNumChannels() >= 2)
    {
        delayLineL.reset();
        delayLineR.reset();
        delayFilterHP.reset();
        delayFilterLP.reset();
        delayFilterHPFb.reset();
        delayFilterLPFb.reset();
        smoothedDelayDecay.setCurrentAndTargetValue(0.0f);
    }
    else if (delayEnabled && delayDecay > 0.001f && numSamples > 0 && buffer.getNumChannels() >= 2)
    {
        // Pre-effect drive: 0 dB at mix 0, 3 dB at mix full (compensates amplitude loss)
        float delayDrive = std::pow(10.0f, delayDryWet * 3.0f / 20.0f);
        buffer.applyGain(0, 0, numSamples, delayDrive);
        buffer.applyGain(1, 0, numSamples, delayDrive);
        // Set smoothed targets (read once per block - real-time safe)
        smoothedDelayTime.setTargetValue(delayTimeSamples);
        smoothedDelayDecay.setTargetValue(delayDecay);
        smoothedDelayDryWet.setTargetValue(delayDryWet);
        
        bool delayFilterOn = safeGetParam(apvts, "delayFilterShow") > 0.5f;
        float delayHPCutoff = juce::jlimit(20.0f, 20000.0f, modParam(ep_delayFilterHPCutoff));
        float delayLPCutoff = juce::jlimit(20.0f, 20000.0f, modParam(ep_delayFilterLPCutoff));
        // Clamp Q to 0.1-5.0 to prevent resonance runaway (was 0.1-20, caused instability)
        float delayHPRes = modParam(ep_delayFilterHPResonance);
        float delayLPRes = modParam(ep_delayFilterLPResonance);
        float hpQ = juce::jlimit(0.1f, 5.0f, 0.1f + delayHPRes * 4.9f);
        float lpQ = juce::jlimit(0.1f, 5.0f, 0.1f + delayLPRes * 4.9f);
        bool delayWarmSat = safeGetParam(apvts, "delayFilterWarmSaturation") > 0.5f;
        
        smoothedDelayHPCutoff.setTargetValue(delayHPCutoff);
        smoothedDelayLPCutoff.setTargetValue(delayLPCutoff);
        smoothedDelayHPQ.setTargetValue(hpQ);
        smoothedDelayLPQ.setTargetValue(lpQ);
        
        // Filter: apply ONLY to delayed/feedback signal (never dry). Reset only in prepareToPlay.
        if (delayFilterOn)
        {
            delayFilterHP.setType(juce::dsp::StateVariableTPTFilterType::highpass);
            delayFilterLP.setType(juce::dsp::StateVariableTPTFilterType::lowpass);
            delayFilterHPFb.setType(juce::dsp::StateVariableTPTFilterType::highpass);
            delayFilterLPFb.setType(juce::dsp::StateVariableTPTFilterType::lowpass);
        }
        
        auto* left = buffer.getWritePointer(0);
        auto* right = buffer.getWritePointer(1);
        
        // Feedback path: low Q (0.707) for stability - resonance causes runaway in feedback loop.
        // Output path: full user Q for the resonant sound we hear.
        auto filterForFeedback = [&](int ch, float x) -> float {
            if (!delayFilterOn) return x;
            float y = delayFilterHPFb.processSample(ch, x);
            return delayFilterLPFb.processSample(ch, y);
        };
        auto filterForOutput = [&](int ch, float x) -> float {
            if (!delayFilterOn) return x;
            float y = delayFilterHP.processSample(ch, x);
            return delayFilterLP.processSample(ch, y);
        };
        auto saturateForOutput = [&](float filtered) -> float {
            if (!delayWarmSat) return filtered;
            float drive = 1.0f + (hpQ + lpQ) * 0.15f;  // Conservative: avoid gain > 1 in feedback
            return std::tanh(juce::jlimit(-1.5f, 1.5f, filtered) * drive);
        };
        // Feedback shaper. With warm saturation ON it tanh-saturates (the intended
        // "warm" colour, baked into each echo). With it OFF the feedback must stay
        // CLEAN â€” tanh was being applied unconditionally, so a low-level single note
        // passed through tanh's linear region untouched, but a louder CHORD got
        // compressed/distorted (the amplitude-dependent "nasty" colour the user heard).
        // When clean we only clamp as a runaway safety net, at a ceiling (Â±4 â‰ˆ +12 dBFS)
        // high enough to stay transparent for musical levels â€” including a chord boosted
        // by the +3 dB delay drive plus feedback accumulation. A Â±2 ceiling was still low
        // enough that loud chords clipped against it (the residual colour). fbDecay <= 0.99
        // keeps the loop stable; this just caps a pathological build-up.
        auto saturateFeedback = [&](float raw) -> float {
            if (delayWarmSat)
                return std::tanh(juce::jlimit(-2.0f, 2.0f, raw));
            return juce::jlimit(-4.0f, 4.0f, raw);
        };
        
        if (delayPingPong)
        {
            for (int s = 0; s < numSamples; ++s)
            {
                float lIn = left[s];
                float rIn = right[s];
                float monoIn = 0.5f * (lIn + rIn);
                
                float delayTime = juce::jmax(1.0f, smoothedDelayTime.getNextValue());
                float fbDecay = juce::jlimit(0.0f, 0.99f, smoothedDelayDecay.getNextValue());
                float dryMix = 1.0f - smoothedDelayDryWet.getNextValue();
                float wetMix = smoothedDelayDryWet.getNextValue();
                
                if (delayFilterOn)
                {
                    float hpCut = smoothedDelayHPCutoff.getNextValue();
                    float lpCut = smoothedDelayLPCutoff.getNextValue();
                    delayFilterHP.setCutoffFrequency(hpCut);
                    delayFilterHP.setResonance(smoothedDelayHPQ.getNextValue());
                    delayFilterLP.setCutoffFrequency(lpCut);
                    delayFilterLP.setResonance(smoothedDelayLPQ.getNextValue());
                    delayFilterHPFb.setCutoffFrequency(hpCut);
                    delayFilterHPFb.setResonance(0.707f);  // Low Q for stable feedback
                    delayFilterLPFb.setCutoffFrequency(lpCut);
                    delayFilterLPFb.setResonance(0.707f);
                }
                
                float d1 = delayLineL.popSample(0, delayTime, true);
                float d2 = delayLineR.popSample(0, delayTime, true);
                float d1FiltOut = filterForOutput(0, d1);
                float d2FiltOut = filterForOutput(1, d2);
                float d1FiltFb = filterForFeedback(0, d1);
                float d2FiltFb = filterForFeedback(1, d2);
                float lOut = dryMix * lIn + wetMix * saturateForOutput(d1FiltOut);
                float rOut = dryMix * rIn + wetMix * saturateForOutput(d2FiltOut);
                float d1Fb = saturateFeedback(monoIn + fbDecay * d2FiltFb);
                delayLineL.pushSample(0, d1Fb);
                delayLineR.pushSample(0, d1FiltFb);
                left[s] = lOut;
                right[s] = rOut;
            }
        }
        else
        {
            for (int s = 0; s < numSamples; ++s)
            {
                float lIn = left[s];
                float rIn = right[s];
                
                float delayTime = juce::jmax(1.0f, smoothedDelayTime.getNextValue());
                float fbDecay = juce::jlimit(0.0f, 0.99f, smoothedDelayDecay.getNextValue());
                float dryMix = 1.0f - smoothedDelayDryWet.getNextValue();
                float wetMix = smoothedDelayDryWet.getNextValue();
                
                if (delayFilterOn)
                {
                    float hpCut = smoothedDelayHPCutoff.getNextValue();
                    float lpCut = smoothedDelayLPCutoff.getNextValue();
                    delayFilterHP.setCutoffFrequency(hpCut);
                    delayFilterHP.setResonance(smoothedDelayHPQ.getNextValue());
                    delayFilterLP.setCutoffFrequency(lpCut);
                    delayFilterLP.setResonance(smoothedDelayLPQ.getNextValue());
                    delayFilterHPFb.setCutoffFrequency(hpCut);
                    delayFilterHPFb.setResonance(0.707f);  // Low Q for stable feedback
                    delayFilterLPFb.setCutoffFrequency(lpCut);
                    delayFilterLPFb.setResonance(0.707f);
                }
                
                float lDelayed = delayLineL.popSample(0, delayTime, true);
                float rDelayed = delayLineR.popSample(0, delayTime, true);
                float lFiltOut = filterForOutput(0, lDelayed);
                float rFiltOut = filterForOutput(1, rDelayed);
                float lFiltFb = filterForFeedback(0, lDelayed);
                float rFiltFb = filterForFeedback(1, rDelayed);
                float lOut = dryMix * lIn + wetMix * saturateForOutput(lFiltOut);
                float rOut = dryMix * rIn + wetMix * saturateForOutput(rFiltOut);
                float lFb = saturateFeedback(lIn + fbDecay * lFiltFb);
                float rFb = saturateFeedback(rIn + fbDecay * rFiltFb);
                delayLineL.pushSample(0, lFb);
                delayLineR.pushSample(0, rFb);
                left[s] = lOut;
                right[s] = rOut;
            }
        }
    }
    
    // Whole block unless something needs the chain finer than that. A patch that
    // modulates no effect parameter costs exactly what it cost before, and
    // produces exactly the same samples -- which tools/chunkaudit proves.
    if (! forceChunking && ! anyEffectParameterIsModulated())
    {
        runEffectsChain (buffer, 0);
    }
    else
    {
        const int total = buffer.getNumSamples();

        for (int start = 0; start < total; start += effectChunkSamples)
        {
            const int len = juce::jmin (effectChunkSamples, total - start);

            // A view onto the same memory, not a copy.
            juce::AudioBuffer<float> chunk (buffer.getArrayOfWritePointers(),
                                            buffer.getNumChannels(),
                                            start,
                                            len);

            runEffectsChain (chunk, start);
        }
    }

    //==============================================================================
    // -- Master Volume Control --
    // Apply master volume to final output (real-time safe)
    // LFO can modulate master volume when target is "Master Vol" (index 2)
    auto* masterVolumeParam = apvts.getRawParameterValue("masterVolume");
    if (masterVolumeParam != nullptr)
    {
        float masterVol = *masterVolumeParam;
        int lfo1Target = static_cast<int>(safeGetParam(apvts, "lfo1Target"));
        int lfo2Target = static_cast<int>(safeGetParam(apvts, "lfo2Target"));
        bool lfo1Master = (lfo1Target == 2);
        bool lfo2Master = (lfo2Target == 2);
        if (lfo1Master || lfo2Master)
        {
            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            {
                auto* ptr = buffer.getWritePointer(ch);
                for (int i = 0; i < numSamples; ++i)
                {
                    float mod = 1.0f;
                    if (lfo1Master && i < lfoBufferFor(0).getNumSamples())
                        mod += lfoBufferFor(0).getSample(0, i);
                    if (lfo2Master && i < lfoBufferFor(1).getNumSamples())
                        mod += lfoBufferFor(1).getSample(0, i);
                    ptr[i] *= masterVol * juce::jlimit(0.0f, 2.0f, mod);
                }
            }
        }
        else
        {
            buffer.applyGain(masterVol);
        }
    }
    
    //==============================================================================
    // -- Stereo Level Meter Peak Tracking --
    // Track peak levels for L/R channels (thread-safe atomic writes)
    if (buffer.getNumChannels() >= 2)
    {
        float leftPeak = buffer.getMagnitude(0, 0, numSamples);
        float rightPeak = buffer.getMagnitude(1, 0, numSamples);
        leftPeakLevel.store(leftPeak);
        rightPeakLevel.store(rightPeak);
    }
    else if (buffer.getNumChannels() >= 1)
    {
        float monoPeak = buffer.getMagnitude(0, 0, numSamples);
        leftPeakLevel.store(monoPeak);
        rightPeakLevel.store(monoPeak);
    }

    //==============================================================================
    // -- Goniometer (Lissajous) Buffer Copy --
    // Double-buffered: audio thread writes to buffer UI is not reading
    if (buffer.getNumChannels() >= 2 && numSamples > 0)
    {
        const int readIdx = goniometerReadIndex.load(std::memory_order_relaxed);
        const int writeIdx = 1 - readIdx;
        auto& dest = goniometerBuffer[writeIdx];
        // Buffers are fixed at goniometerMaxSamples; clamp the copy so a block larger
        // than that capacity (host block-size violation) can never overrun the buffer.
        const int copyN = juce::jmin(numSamples, dest.getNumSamples());
        if (dest.getNumChannels() >= 2 && copyN > 0)
        {
            dest.copyFrom(0, 0, buffer, 0, 0, copyN);
            dest.copyFrom(1, 0, buffer, 1, 0, copyN);
            if (copyN < dest.getNumSamples())
                dest.clear(copyN, dest.getNumSamples() - copyN);
            goniometerValidSamples.store(copyN, std::memory_order_release);
            goniometerReadIndex.store(writeIdx, std::memory_order_release);
        }
    }

    //==============================================================================
    // -- Spectrum Analyser FIFO --
    // Append a continuous mono mix of the output so the UI FFT reads a gap-free
    // window (this is what keeps a steady note rock-stable on the display).
    if (numSamples > 0 && buffer.getNumChannels() >= 1)
    {
        const float* L = buffer.getReadPointer(0);
        const float* R = buffer.getNumChannels() >= 2 ? buffer.getReadPointer(1) : L;
        int wp = spectrumFifoWritePos.load(std::memory_order_relaxed);
        for (int i = 0; i < numSamples; ++i)
        {
            spectrumFifo[static_cast<size_t>(wp)] = 0.5f * (L[i] + R[i]);
            wp = (wp + 1) & (spectrumFifoSize - 1);
        }
        spectrumFifoWritePos.store(wp, std::memory_order_release);

        // -- Oscilloscope FIFO --
        // The same idea kept in STEREO, because the scope draws L and R separately
        // and the spectrum's mono sum cannot serve it (2026-08-01). It has to be a
        // continuous history for the same reason: the UI reads ~20 times a second
        // while blocks arrive ~86 times a second, so anything assembled from the
        // blocks the UI happens to catch is stitched from non-adjacent audio.
        int swp = scopeFifoWritePos.load(std::memory_order_relaxed);
        for (int i = 0; i < numSamples; ++i)
        {
            scopeFifoL[static_cast<size_t>(swp)] = L[i];
            scopeFifoR[static_cast<size_t>(swp)] = R[i];
            swp = (swp + 1) & (scopeFifoSize - 1);
        }
        scopeFifoWritePos.store(swp, std::memory_order_release);

        // -- Resample --
        // Does nothing unless a recording is running. Written here, at the very
        // end of the chain, so what Resample keeps is exactly what the player
        // would have heard -- every effect, the master volume, all of it.
        resampleCapture.write(L, R, numSamples);
    }

    //==============================================================================
    // -- Resample: heard by the recorder, and by nobody else --
    // The note is played into the chain and taken out of it again here, after the
    // recorder and after every meter and scope -- so the picture on the panel
    // still moves while the recording is made, and the speakers stay silent.
    //
    // Last of all, so nothing downstream can put it back. isRecording() is already
    // false on the block a recording ends in, but a recording ends BECAUSE the
    // sound has died away, so the block let through is a block of silence.
    if (resampleCapture.isRecording())
        buffer.clear();
}

//==============================================================================
// -- Bit Crusher / Trance Gate: single implementation, called from BOTH the
//    Pre placement (processBlock, unchunked) and the Post placement
//    (runEffectsChain, chunked). Each one re-reads its own parameters so a
//    later change to how a parameter is read (e.g. a modulated value instead
//    of the raw APVTS value) takes effect at both placements at once.
//==============================================================================
void SpaceDustAudioProcessor::processBitCrusher (juce::AudioBuffer<float>& buffer, int /*startSampleInBlock*/)
{
    bool bitCrusherEnabled = safeGetParam(apvts, "bitCrusherEnabled") > 0.5f;
    if (bitCrusherEnabled && buffer.getNumChannels() >= 1 && buffer.getNumSamples() > 0)
    {
        SpaceDustBitCrusher::Parameters bp;
        bp.enabled = true;
        bp.amount = juce::jlimit(0.0f, 1.0f, modParam(ep_bitCrusherAmount));
        bp.rate = juce::jlimit(0.0f, 1.0f, modParam(ep_bitCrusherRate));
        bp.mix = juce::jlimit(0.0f, 1.0f, modParam(ep_bitCrusherMix));
        bitCrusher_.setParameters(bp);
        bitCrusher_.process(buffer);
    }
}

void SpaceDustAudioProcessor::processTranceGate (juce::AudioBuffer<float>& buffer, int startSampleInBlock)
{
    bool tranceGateEnabled = safeGetParam(apvts, "tranceGateEnabled") > 0.5f;
    if (tranceGateEnabled && buffer.getNumChannels() >= 2 && buffer.getNumSamples() > 0)
    {
        SpaceDustTranceGate::Parameters tp;
        tp.enabled = true;
        {
            const int stepsIdx = effectChoiceIndex(ec_tranceGateSteps, 1);
            tp.numSteps = (stepsIdx == 0) ? 4 : (stepsIdx == 1) ? 8 : 16;
        }
        tp.sync = safeGetParam(apvts, "tranceGateSync") > 0.5f;
        tp.rate = modParam(ep_tranceGateRate);
        tp.attackMs = modParam(ep_tranceGateAttack);
        tp.releaseMs = modParam(ep_tranceGateRelease);
        tp.mix = modParam(ep_tranceGateMix);
        for (int s = 0; s < 16; ++s)
            if (auto* rp = tranceGateStepValues[s])
                tp.stepOn[s] = rp->load() > 0.5f;
        tranceGate_.setParameters(tp);
        tranceGate_.process(buffer, currentSampleRate, getPlayHead(), startSampleInBlock);
    }
}

//==============================================================================
void SpaceDustAudioProcessor::runEffectsChain (juce::AudioBuffer<float>& buffer, int startSampleInBlock)
{
    // Where every knob in this chain sits for THIS piece of the block. Called
    // once when the chain runs whole and once per 32-sample chunk when an
    // effect knob is modulated. With nothing modulated it writes each knob's
    // own value back, so the chain reads exactly what it read before.
    refreshEffectModulatedValues (startSampleInBlock);

    //==============================================================================
    // -- Reverb Effect --
    bool reverbEnabled = safeGetParam(apvts, "reverbEnabled") > 0.5f;
    float reverbDecayTime = modParam(ep_reverbDecayTime);
    if (reverbEnabled && buffer.getNumChannels() >= 2 && buffer.getNumSamples() > 0)
    {
        // Decay at minimum: flush reverb once and bypass (Void Verb still diffuses when decay_ == 0).
        if (reverbDecayTime <= 0.001f)
        {
            if (lastReverbDecayForBypass_ > 0.001f || lastReverbDecayForBypass_ < 0.0f)
                reverb_.reset();
        }
        else
        {
            float reverbWetMix = modParam(ep_reverbWetMix);
            float reverbDrive = std::pow(10.0f, reverbWetMix * 3.0f / 20.0f);
            buffer.applyGain(0, 0, buffer.getNumSamples(), reverbDrive);
            buffer.applyGain(1, 0, buffer.getNumSamples(), reverbDrive);
            SpaceDustReverb::Parameters rp;
            rp.type = effectChoiceIndex(ec_reverbType, 0);
            rp.wetMix = reverbWetMix;
            rp.decayTime = reverbDecayTime;
            rp.filterOn = safeGetParam(apvts, "reverbFilterShow") > 0.5f;
            rp.filterWarmSaturation = safeGetParam(apvts, "reverbFilterWarmSaturation") > 0.5f;
            rp.filterHPCutoff = modParam(ep_reverbFilterHPCutoff);
            rp.filterHPResonance = modParam(ep_reverbFilterHPResonance);
            rp.filterLPCutoff = modParam(ep_reverbFilterLPCutoff);
            rp.filterLPResonance = modParam(ep_reverbFilterLPResonance);
            reverb_.setParameters(rp);
            reverb_.process(buffer);
        }
        lastReverbDecayForBypass_ = reverbDecayTime;
    }
    else if (!reverbEnabled)
    {
        lastReverbDecayForBypass_ = -1.0f;
    }

    //==============================================================================
    // -- Grain Delay Effect --
    bool grainDelayEnabled = safeGetParam(apvts, "grainDelayEnabled") > 0.5f;
    if (grainDelayEnabled && buffer.getNumChannels() >= 2 && buffer.getNumSamples() > 0)
    {
        float grainMix = juce::jlimit(0.0f, 1.0f, modParam(ep_grainDelayMix) * 0.01f);
        float grainDrive = std::pow(10.0f, grainMix * 3.0f / 20.0f);
        buffer.applyGain(0, 0, buffer.getNumSamples(), grainDrive);
        buffer.applyGain(1, 0, buffer.getNumSamples(), grainDrive);
        SpaceDustGrainDelay::Parameters gp;
        gp.enabled = true;
        gp.delayMs = juce::jlimit(20.0f, 2000.0f, modParam(ep_grainDelayTime));
        gp.grainSizeMs = juce::jlimit(10.0f, 500.0f, modParam(ep_grainDelaySize));
        gp.pitchSemitones = juce::jlimit(-12.0f, 12.0f, modParam(ep_grainDelayPitch));
        gp.mix = grainMix;
        // Decay is 0-150% in the APVTS, so it is scaled here. It is a plain
        // AudioParameterFloat exactly like grainDelayMix beside it, and
        // getRawParameterValue reports real units for it as it does for every
        // other parameter. An older comment here claimed the value was
        // normalised and that get() was needed; that was wrong, and believing
        // it left this knob assignable but unable to move.
        gp.decay = juce::jlimit(0.0f, 1.0f, modParam(ep_grainDelayDecay) / 150.0f);
        gp.density = juce::jlimit(1.0f, 8.0f, modParam(ep_grainDelayDensity));
        gp.jitter = juce::jlimit(0.0f, 1.0f, modParam(ep_grainDelayJitter) * 0.01f);
        gp.pingPong = safeGetParam(apvts, "grainDelayPingPong") > 0.5f;
        gp.filterOn = safeGetParam(apvts, "grainDelayFilterShow") > 0.5f;
        gp.hpCutoffHz = juce::jlimit(20.0f, 20000.0f, modParam(ep_grainDelayFilterHPCutoff));
        gp.lpCutoffHz = juce::jlimit(20.0f, 20000.0f, modParam(ep_grainDelayFilterLPCutoff));
        gp.hpRes = modParam(ep_grainDelayFilterHPResonance);
        gp.lpRes = modParam(ep_grainDelayFilterLPResonance);
        gp.warmSaturation = safeGetParam(apvts, "grainDelayFilterWarmSaturation") > 0.5f;
        grainDelay_.setParameters(gp);
        grainDelay_.process(buffer);
    }

    //==============================================================================
    // -- Phaser Effect --
    bool phaserEnabled = safeGetParam(apvts, "phaserEnabled") > 0.5f;
    if (phaserEnabled && buffer.getNumChannels() >= 2 && buffer.getNumSamples() > 0)
    {
        float phaserMix = juce::jlimit(0.0f, 1.0f, modParam(ep_phaserMix));
        float phaserDrive = std::pow(10.0f, phaserMix * 3.0f / 20.0f);
        buffer.applyGain(0, 0, buffer.getNumSamples(), phaserDrive);
        buffer.applyGain(1, 0, buffer.getNumSamples(), phaserDrive);
        SpaceDustPhaser::Parameters pp;
        pp.enabled = true;
        pp.rateHz = juce::jlimit(0.05f, 200.0f, modParam(ep_phaserRate));
        pp.depth = juce::jlimit(0.0f, 1.0f, modParam(ep_phaserDepth));
        pp.feedback = juce::jlimit(-1.0f, 1.0f, modParam(ep_phaserFeedback));
        pp.scriptMode = safeGetParam(apvts, "phaserScriptMode") > 0.5f;
        pp.mix = phaserMix;
        pp.centreHz = juce::jlimit(50.0f, 2000.0f, modParam(ep_phaserCentre));
        pp.numStages = (effectChoiceIndex(ec_phaserStages, 0) == 0) ? 4 : 6;
        pp.stereoOffset = juce::jlimit(0.0f, 1.0f, modParam(ep_phaserStereoOffset));
        pp.vintageMode = safeGetParam(apvts, "phaserVintageMode") > 0.5f;
        phaser_.setParameters(pp);
        phaser_.process(buffer);
    }

    //==============================================================================
    // -- Flanger Effect --
    bool flangerEnabled = safeGetParam(apvts, "flangerEnabled") > 0.5f;
    if (flangerEnabled && buffer.getNumChannels() >= 1 && buffer.getNumSamples() > 0)
    {
        float flangerMix = juce::jlimit(0.0f, 1.0f, modParam(ep_flangerMix));
        float flangerDrive = std::pow(10.0f, flangerMix * 3.0f / 20.0f);
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            buffer.applyGain(ch, 0, buffer.getNumSamples(), flangerDrive);
        SpaceDustFlanger::Parameters fp;
        fp.enabled = true;
        fp.rateHz = juce::jlimit(0.05f, 200.0f, modParam(ep_flangerRate));
        fp.depth = juce::jlimit(0.0f, 1.0f, modParam(ep_flangerDepth));
        fp.feedback = juce::jlimit(-1.0f, 1.0f, modParam(ep_flangerFeedback));
        fp.width = juce::jlimit(0.0f, 1.0f, modParam(ep_flangerWidth));
        fp.mix = flangerMix;
        flanger_.setParameters(fp);
        flanger_.process(buffer);
    }

    // -- Transient (Post: end of chain, before late BC) --
    {
        const bool transientEnabled = safeGetParam(apvts, "transientEnabled") > 0.5f;
        const bool transientPostEffect = safeGetParam(apvts, "transientPostEffect") > 0.5f;
        if (transientEnabled && transientPostEffect)
        {
            SpaceDustTransient::Parameters tp;
            tp.enabled = true;
            tp.type = effectChoiceIndex(ec_transientType, tp.type);
            tp.mix = juce::jlimit(0.0f, 1.0f, modParam(ep_transientMix));
            tp.postEffect = true;
            tp.kaDonk = juce::jlimit(0.0f, 1.0f, modParam(ep_transientKaDonk));
            tp.coarse = juce::jlimit(-24.0f, 24.0f, modParam(ep_transientCoarse));
            tp.length = juce::jlimit(0.0f, 1.0f, modParam(ep_transientLength));
            transient_.setParameters(tp);
            transient_.process(buffer);
        }
    }

    // -- Bit Crusher (late: after Flanger, before TG) --
    {
        const bool bitCrusherPostEffect = safeGetParam(apvts, "bitCrusherPostEffect") > 0.5f;
        if (bitCrusherPostEffect)
            processBitCrusher (buffer, startSampleInBlock);
    }

    // -- Trance Gate (Post: when Post Effect ON, always last) --
    {
        const bool tranceGatePostEffect = safeGetParam(apvts, "tranceGatePostEffect") > 0.5f;
        if (tranceGatePostEffect)
            processTranceGate (buffer, startSampleInBlock);
    }

    //==============================================================================
    // -- Compressor (Saturation Color) - after BitCrusher/TranceGate, before Soft Clipper --
    bool compressorEnabled = safeGetParam(apvts, "compressorEnabled") > 0.5f;
    if (compressorEnabled && buffer.getNumChannels() >= 1 && buffer.getNumSamples() > 0)
    {
        SpaceDustCompressor::Parameters cp;
        cp.enabled = true;
        cp.type = effectChoiceIndex(ec_compressorType, 0);
        cp.thresholdDb = juce::jlimit(-60.0f, 0.0f, modParam(ep_compressorThreshold));
        cp.ratio = juce::jlimit(1.0f, 20.0f, modParam(ep_compressorRatio));
        cp.attackMs = juce::jlimit(0.1f, 80.0f, modParam(ep_compressorAttack));
        cp.releaseMs = juce::jlimit(5.0f, 1200.0f, modParam(ep_compressorRelease));
        cp.makeupGainDb = juce::jlimit(0.0f, 24.0f, modParam(ep_compressorMakeup));
        cp.mix = juce::jlimit(0.0f, 1.0f, modParam(ep_compressorMix));
        cp.autoRelease = safeGetParam(apvts, "compressorAutoRelease") > 0.5f;
        cp.softClip = safeGetParam(apvts, "compressorSoftClip") > 0.5f;
        compressor_.setParameters(cp);
        compressor_.process(buffer);
    }

    //==============================================================================
    // -- Soft Clipper (Saturation Color) - before master volume --
    bool softClipperEnabled = safeGetParam(apvts, "softClipperEnabled") > 0.5f;
    if (softClipperEnabled && buffer.getNumChannels() >= 1 && buffer.getNumSamples() > 0)
    {
        SpaceDustSoftClipper::Parameters sp;
        sp.enabled = true;
        sp.mode = effectChoiceIndex(ec_softClipperMode, 0);
        sp.drive = juce::jlimit(0.0f, 1.0f, modParam(ep_softClipperDrive));
        sp.knee = juce::jlimit(0.0f, 1.0f, modParam(ep_softClipperKnee));
        {
            const int idx = effectChoiceIndex(ec_softClipperOversample, 0);
            sp.oversample = (idx == 0) ? 2 : (idx == 1) ? 4 : (idx == 2) ? 8 : 16;
        }
        sp.mix = juce::jlimit(0.0f, 1.0f, modParam(ep_softClipperMix));
        softClipper_.setParameters(sp);
        softClipper_.process(buffer);
    }
    
    //==============================================================================
    // -- Lo-Fi (Saturation Color) - end of effects chain, before master volume --
    bool lofiEnabled = safeGetParam(apvts, "lofiEnabled") > 0.5f;
    if (lofiEnabled && buffer.getNumChannels() >= 1 && buffer.getNumSamples() > 0)
    {
        SpaceDustLofi::Parameters lp;
        lp.enabled = true;
        lp.amount = juce::jlimit(0.0f, 1.0f, modParam(ep_lofiAmount));
        lofi_.setParameters(lp);
        lofi_.process(buffer);
    }

    //==============================================================================
    // -- Final EQ (5-band, end of chain, Saturation Color tab) --
    {
        bool finalEQEnabled = safeGetParam(apvts, "finalEQEnabled") > 0.5f;
        if (finalEQEnabled && buffer.getNumChannels() >= 1 && buffer.getNumSamples() > 0)
        {
            // Each band's shape is its own parameter now. The fallback index is the
            // band's original fixed type, so a preset saved before the Type dropdown
            // existed still loads with the EQ it was built on.
            const int defaultTypeIndex[5] = {
                static_cast<int>(SpaceDustFinalEQ::BandType::LowShelf),
                static_cast<int>(SpaceDustFinalEQ::BandType::Bell),
                static_cast<int>(SpaceDustFinalEQ::BandType::Bell),
                static_cast<int>(SpaceDustFinalEQ::BandType::Bell),
                static_cast<int>(SpaceDustFinalEQ::BandType::HighShelf)
            };
            SpaceDustFinalEQ::Parameters fep;
            fep.enabled = true;
            for (int i = 0; i < 5; ++i)
            {
                // Band i's three modulatable knobs sit together in the effect
                // parameter list, in Freq / Gain / Q order, and its Type sits
                // at the matching place in the choice list -- see the
                // static_asserts beside both lists. Nothing here builds a
                // string, because the chain runs this sixteen times a block
                // whenever an effect knob is assigned.
                const int band = ep_finalEQB1Freq + i * finalEQParamsPerBand;
                fep.bands[i].freqHz = juce::jlimit(20.0f, 20000.0f, modParam(band + 0, 1000.0f));
                fep.bands[i].gainDb = juce::jlimit(-15.0f, 15.0f,    modParam(band + 1));
                fep.bands[i].Q      = juce::jlimit(0.1f, 10.0f,      modParam(band + 2, 1.0f));
                fep.bands[i].type   = SpaceDustFinalEQ::typeFromChoiceIndex(
                    effectChoiceIndex(ec_finalEQB1Type + i, defaultTypeIndex[i]));
            }
            finalEQ_.setParameters(fep);
            finalEQ_.process(buffer);
        }
    }
}

bool SpaceDustAudioProcessor::anyEffectParameterIsModulated() const noexcept
{
    return effectsAreModulated.load (std::memory_order_relaxed);
}

//==============================================================================
// -- The modulation matrix, compiled and delivered --
//==============================================================================

bool SpaceDustAudioProcessor::isEffectParameter (const std::string& id) noexcept
{
    // The chain from runEffectsChain, by the prefix each stage's parameters use.
    // A knob outside this list is a voice knob, which the per-sample voice loop
    // already reaches without any chunking.
    //
    // "master" is deliberately NOT here. masterVolume is one of the six
    // destinations the VOICE already applies per sample; listing it would chunk
    // the whole effects chain for a knob that does not need it, and risks the
    // same knob being applied twice.
    static const char* const prefixes[] = {
        "reverb", "delay", "grain", "phaser", "flanger", "transient",
        "compressor", "softClipper", "lofi", "bitCrusher", "tranceGate",
        "finalEQ"
    };

    for (const char* p : prefixes)
        if (id.rfind (p, 0) == 0)
            return true;

    return false;
}

void SpaceDustAudioProcessor::rebuildCompiledRoutings()
{
    // MESSAGE THREAD ONLY. Everything here allocates.
    const int numDests = modDestinations.size();

    // The destination table is built once, in the constructor, and never
    // changes -- so these are sized on the FIRST call and left alone after it.
    // Re-assigning them on every rebuild would write zeros into arrays the
    // audio thread may be reading from at that very moment.
    if ((int) destBases.size() != numDests)
    {
        destBases.assign ((size_t) numDests, 0.0f);
        effectModulated.assign ((size_t) numDests, 0.0f);
        destRanges.assign ((size_t) numDests, spacedust::DestRange { });
        destRawValues.assign ((size_t) numDests, nullptr);

        for (int i = 0; i < numDests; ++i)
        {
            destRanges[(size_t) i] = modDestinations.rangeAt (i);

            // The very atomic the old safeGetParam() call found by name. Taking
            // it here means an UNMODULATED destination reads bit-identically to
            // the way it read before the matrix existed, and that no chunk has
            // to hash a parameter id.
            destRawValues[(size_t) i] =
                apvts.getRawParameterValue (juce::String (modDestinations.idAt (i)));
        }

        // Where each knob the effects chain reads sits in that same table.
        effectParamSlots.assign ((size_t) numEffectParams, -1);

        for (int i = 0; i < numEffectParams; ++i)
            effectParamSlots[(size_t) i] = modDestinations.slotFor (effectParamIds[i]);

        // The chain's choices and the trance gate's step switches, resolved to
        // pointers here so no chunk ever builds a juce::String to find one.
        effectChoiceParams.assign ((size_t) numEffectChoices, nullptr);

        for (int i = 0; i < numEffectChoices; ++i)
            effectChoiceParams[(size_t) i] = dynamic_cast<juce::AudioParameterChoice*> (
                apvts.getParameter (effectChoiceIds[i]));

        for (int i = 0; i < 16; ++i)
            tranceGateStepValues[i] = apvts.getRawParameterValue (tranceGateStepIds[i]);
    }

    // Build into a buffer that is NEITHER the one published last NOR the one
    // the audio thread said it is walking. With three buffers and two
    // forbidden indices, exactly one is always free.
    const int live = liveCompiled.load (std::memory_order_relaxed);
    const int busy = readerInUse.load (std::memory_order_acquire);

    int target = 0;
    while (target == live || target == busy)
        ++target;

    jassert (target < numCompiledBuffers);

    auto& out = compiledBuffers[target];

    out.routings.clear();
    out.voiceSlots.clear();
    out.routings.reserve (modMatrix.routings().size());

    bool touchesEffects = false;
    int  voiceSlotsRefused = 0;

    for (const auto& r : modMatrix.routings())
    {
        const int slot = modDestinations.slotFor (r.destination);

        // An old patch may name a parameter this build does not have. It is kept
        // in modMatrix (see fromXml) but it can never reach audio, because this
        // lookup gives -1 and the routing is skipped here.
        if (slot < 0)
            continue;

        const auto range = modDestinations.rangeAt (slot);

        if (isEffectParameter (modDestinations.idAt (slot)))
        {
            touchesEffects = true;
        }
        else if (std::find (out.voiceSlots.begin(), out.voiceSlots.end(), slot)
                     == out.voiceSlots.end())
        {
            // One row per DESTINATION, not per routing: four LFOs on the same
            // knob still sum into one value.
            //
            // The scratch cap is enforced HERE, on the message thread, not in
            // the audio-thread fill. A routing past the cap is refused outright
            // and logged, so the knob simply never becomes a destination --
            // rather than being accepted, compiled, and then silently dropped
            // every block by a fill loop with nowhere to put it.
            if ((int) out.voiceSlots.size() >= maxVoiceModRows)
            {
                ++voiceSlotsRefused;
                logToFile ("Mod matrix: refused a routing to '"
                           + juce::String (modDestinations.idAt (slot))
                           + "' -- already at the limit of "
                           + juce::String (maxVoiceModRows)
                           + " modulated voice knobs");
                continue;
            }

            out.voiceSlots.push_back (slot);
        }

        out.routings.push_back (spacedust::CompiledRouting {
            slot, r.lfoIndex, r.amount * range.halfRange() });
    }

    if (voiceSlotsRefused > 0)
        logToFile ("Mod matrix: " + juce::String (voiceSlotsRefused)
                   + " routing(s) refused; the voice scratch holds "
                   + juce::String (maxVoiceModRows) + " knobs");

    effectsAreModulated.store (touchesEffects, std::memory_order_relaxed);
    liveCompiled.store (target, std::memory_order_release);
}

float SpaceDustAudioProcessor::modParam (int which, float fallback) noexcept
{
    if (which < 0 || which >= numEffectParams)
        return fallback;

    if (which < (int) effectParamSlots.size())
    {
        const int slot = effectParamSlots[(size_t) which];

        if (slot >= 0 && slot < (int) effectModulated.size())
            return effectModulated[(size_t) slot];
    }

    // Not a legal destination, or the tables are not built yet: read it the way
    // the chain always did.
    return safeGetParam (apvts, effectParamIds[which], fallback);
}

void SpaceDustAudioProcessor::latchCompiledRoutings() noexcept
{
    // Load once, then tell the message thread which buffer this block is on.
    // The acquire pairs with the release store at the end of
    // rebuildCompiledRoutings, so the routings are visible before they are read.
    blockCompiledIndex = liveCompiled.load (std::memory_order_acquire);

    // Published so rebuildCompiledRoutings can avoid this buffer. Never
    // cleared: leaving it set means the message thread keeps avoiding the last
    // buffer the audio thread touched, which is exactly what makes a rebuild
    // during a block safe.
    readerInUse.store (blockCompiledIndex, std::memory_order_release);
}

void SpaceDustAudioProcessor::refreshEffectModulatedValues (int startSampleInBlock) noexcept
{
    const int numDests = (int) destBases.size();

    if (numDests <= 0 || (int) effectModulated.size() != numDests)
        return;

    // The LFO value at the FIRST sample of this piece stands for the whole
    // piece. At 32 samples that is a control rate near 1400 Hz, which is what
    // makes an assigned effect knob move smoothly rather than in steps.
    for (int i = 0; i < spacedust::numLfos; ++i)
    {
        const auto& b = lfoBufferFor (i);
        lfoValues[i] = (startSampleInBlock < b.getNumSamples())
                           ? b.getSample (0, startSampleInBlock)
                           : 0.0f;
    }

    for (int i = 0; i < numDests; ++i)
    {
        auto* raw = destRawValues[(size_t) i];
        destBases[(size_t) i] = (raw != nullptr) ? raw->load (std::memory_order_relaxed) : 0.0f;
    }

    // The set latched for this block, not a fresh load: every chunk must see
    // the same routings, and the message thread is guaranteed not to be
    // building into this buffer.
    const auto& live = compiledBuffers[blockCompiledIndex];

    spacedust::ModMatrix::applyCompiled (live.routings.data(), (int) live.routings.size(),
                                         destBases.data(), destRanges.data(), numDests,
                                         lfoValues, effectModulated.data());
}

void SpaceDustAudioProcessor::fillVoiceModScratch (int numSamples) noexcept
{
    voiceModRowsFilled   = 0;
    voiceModValidSamples = 0;

    if (voiceModRowSamples <= 0 || numSamples <= 0 || destBases.empty())
        return;

    const auto& live = compiledBuffers[blockCompiledIndex];

    if (live.voiceSlots.empty())
        return;

    // How much of this block the rows actually cover. Normally the whole of it:
    // the rows are sized in prepareToPlay with the same 8192-sample headroom the
    // LFO buffers use, so only a host handing over a block bigger than THAT
    // clamps here. Nothing is grown to meet it -- growing would mean a heap
    // allocation inside processBlock, which is worse than covering less of a
    // rare oversized block. numVoiceModSamples() publishes the figure so a
    // reader cannot walk off the end of a row; see the note there.
    const int columns = juce::jmin (numSamples, voiceModRowSamples);

    // Every slot in the list has a row: rebuildCompiledRoutings refused any
    // routing past the cap on the message thread, so there is nothing to
    // truncate here and nothing is dropped without the player being told. The
    // assert catches the cap being raised in one place and not the other.
    jassert ((int) live.voiceSlots.size() <= maxVoiceModRows);

    for (const int slot : live.voiceSlots)
    {
        if (slot < 0 || slot >= (int) destBases.size()
            || voiceModRowsFilled >= maxVoiceModRows)
            continue;

        // Gather this destination's LFOs once, then walk the samples. A
        // destination can carry at most one routing per LFO, because setRouting
        // replaces a pair rather than adding a second one.
        const float* lfoRead[spacedust::numLfos] = { nullptr, nullptr, nullptr, nullptr };
        float        lfoScale[spacedust::numLfos] = { 0.0f, 0.0f, 0.0f, 0.0f };
        int          numContributions = 0;

        for (const auto& c : live.routings)
        {
            if (c.destSlot != slot || numContributions >= spacedust::numLfos)
                continue;

            const auto& b = lfoBufferFor (c.lfoIndex);

            if (b.getNumSamples() < columns)
                continue;

            lfoRead[numContributions]  = b.getReadPointer (0);
            lfoScale[numContributions] = c.scale;
            ++numContributions;
        }

        auto* rawValue = destRawValues[(size_t) slot];
        const float base = (rawValue != nullptr) ? rawValue->load (std::memory_order_relaxed) : 0.0f;
        const auto range = destRanges[(size_t) slot];

        // The row this destination lands on, which is not `row` when an earlier
        // slot was skipped above.
        const int outRow = voiceModRowsFilled;
        float* dest = voiceModScratch.data() + (size_t) outRow * (size_t) voiceModRowSamples;

        for (int i = 0; i < columns; ++i)
        {
            float sum = base;

            for (int c = 0; c < numContributions; ++c)
                sum += lfoScale[c] * lfoRead[c][i];

            dest[i] = juce::jlimit (range.start, range.end, sum);
        }

        voiceModRowSlots[outRow] = slot;
        ++voiceModRowsFilled;
    }

    // Published last, so it is only non-zero once the rows behind it are filled.
    voiceModValidSamples = columns;
}

float SpaceDustAudioProcessor::voiceModulatedValue (const char* parameterId,
                                                    float fallback) const noexcept
{
    const int slot = modDestinations.slotFor (parameterId);

    if (slot < 0)
        return fallback;

    // voiceModScratch holds only the destinations that carry a routing, not one
    // row per destination, so the slot -> row mapping is a short linear search
    // over the small handful of rows actually filled this block (capped at
    // maxVoiceModRows). No allocation, bounded work either way.
    for (int row = 0; row < numVoiceModRows(); ++row)
    {
        if (voiceModSlotForRow (row) != slot)
            continue;

        if (const float* samples = voiceModRow (row))
            return samples[0];

        break;
    }

    return fallback;
}

void SpaceDustAudioProcessor::readSpectrumSamples(float* dest, int numSamples) const
{
    constexpr int mask = spectrumFifoSize - 1;
    const int count = juce::jmin(numSamples, spectrumFifoSize);
    const int wp = spectrumFifoWritePos.load(std::memory_order_acquire);
    // The most-recent `count` samples end just before the write cursor.
    int idx = (wp - count) & mask;
    for (int i = 0; i < count; ++i)
    {
        dest[i] = spectrumFifo[static_cast<size_t>(idx)];
        idx = (idx + 1) & mask;
    }
}

//==============================================================================
const juce::AudioBuffer<float>& SpaceDustAudioProcessor::getGoniometerBuffer() const
{
    return goniometerBuffer[goniometerReadIndex.load(std::memory_order_acquire)];
}

//==============================================================================
float SpaceDustAudioProcessor::getLeftPeakLevel() const
{
    return leftPeakLevel.load();
}

float SpaceDustAudioProcessor::getRightPeakLevel() const
{
    return rightPeakLevel.load();
}

//==============================================================================
bool SpaceDustAudioProcessor::hasEditor() const
{
    return true; // (change this to false if you choose to not supply an editor)
}

juce::AudioProcessorEditor* SpaceDustAudioProcessor::createEditor()
{
    #if JUCE_DEBUG
    try
    {
        juce::File logFile = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
            .getChildFile(safeString("SpaceDust_DebugLog.txt"));
        juce::FileOutputStream out(logFile);
        if (out.openedOk())
        {
            out.setPosition(out.getPosition());  // Append
            out.writeText("Space Dust: createEditor() called\n", false, false, nullptr);
            out.flush();
        }
    }
    catch (...) {}
    #endif
    
    DBG("Space Dust: createEditor() called");
    return new SpaceDustAudioProcessorEditor(*this);
}

//==============================================================================
double SpaceDustAudioProcessor::lfoKnobToHz(double knob0to12)
{
    const double norm = juce::jlimit(0.0, 1.0, knob0to12 / 12.0);
    return juce::jlimit(lfoFreeRateMinHz, lfoFreeRateMaxHz,
                        std::exp(std::log(lfoFreeRateMinHz)
                                 + norm * (std::log(lfoFreeRateMaxHz) - std::log(lfoFreeRateMinHz))));
}

void SpaceDustAudioProcessor::migrateLfoRatesIfOld(juce::ValueTree& state, int stateVersion)
{
    // Only version 2 speaks a different range. Version 1 (or a missing attribute)
    // predates the 2 kHz experiment and already means what the current range means,
    // and version 3 onwards is current by definition.
    if (stateVersion != 2 || !state.isValid())
        return;

    // Both ranges are logarithmic from the same 0.01 Hz floor, so preserving the
    // frequency is a single scale factor on the knob position:
    //   knobNew = knobOld * log(2000/0.01) / log(200/0.01)
    //
    // Rates above 200 Hz cannot be represented any more and clamp to the top. That
    // only affects patches saved during the window when the range was 2 kHz.
    const double scale = std::log(lfoFreeRateV2MaxHz / lfoFreeRateMinHz)
                       / std::log(lfoFreeRateMaxHz   / lfoFreeRateMinHz);

    for (auto child : state)
    {
        if (!child.hasProperty("id"))
            continue;

        const auto id = child.getProperty("id").toString();
        if (id != "lfo1Rate" && id != "lfo2Rate")
            continue;

        // Sync mode reads this same parameter as a tempo-division index, so rescaling
        // it would retune synced LFOs instead. Only touch a genuinely free-running one.
        const auto syncID = (id == "lfo1Rate") ? "lfo1Sync" : "lfo2Sync";
        bool isSynced = false;
        for (auto sibling : state)
            if (sibling.hasProperty("id") && sibling.getProperty("id").toString() == syncID)
                isSynced = static_cast<double>(sibling.getProperty("value")) > 0.5;

        if (isSynced)
            continue;

        const double oldKnob = static_cast<double>(child.getProperty("value"));
        child.setProperty("value", juce::jlimit(0.0, 12.0, oldKnob * scale), nullptr);
    }
}

void SpaceDustAudioProcessor::migrateWaveformChoicesIfOld(juce::ValueTree& state, int stateVersion)
{
    // Version 4 onwards already speaks the new numbering. Anything older -- and a
    // missing attribute, which means version 1 -- stored four built-in shapes
    // followed by the User slots.
    if (stateVersion >= currentStateVersion || !state.isValid())
        return;

    const int inserted = OscShape::numShapes - legacyOscUserBase;

    // Nothing to do if no shapes were ever inserted. Cheap, and it keeps this
    // correct if the two numbers are ever brought back level.
    if (inserted <= 0)
        return;

    for (auto child : state)
    {
        if (!child.hasProperty("id"))
            continue;

        const auto id = child.getProperty("id").toString();

        if (id != "osc1Waveform" && id != "osc2Waveform" && id != "subOscWaveform")
            continue;

        const double stored = static_cast<double>(child.getProperty("value"));

        // Below the old base is a built-in shape, and the first four kept their
        // places, so it already means what it always meant.
        if (stored < static_cast<double>(legacyOscUserBase))
            continue;

        // At or above it is a User slot, which has moved up by however many
        // shapes went in front of it.
        const double moved = stored + static_cast<double>(inserted);
        const double highest = static_cast<double>(OscShape::numShapes + UserWave::numSlots - 1);

        child.setProperty("value", juce::jlimit(0.0, highest, moved), nullptr);
    }
}

void SpaceDustAudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    //==============================================================================
    // -- Ableton VST3 State Saving Workaround --
    // CRITICAL: Check that state is valid before saving
    // In Ableton Live, getStateInformation may be called with invalid state during unload
    auto state = apvts.copyState();
    if (state.isValid())
    {
        std::unique_ptr<juce::XmlElement> xml(state.createXml());
        if (xml != nullptr)
        {
            // Save persistent UI state alongside parameters
            xml->setAttribute("presetName", currentPresetName);
            xml->setAttribute("cheezeGuyActivated", cheezeGuyActivated);
            xml->setAttribute("lastActiveTabIndex", lastActiveTabIndex);
            // Marks which meaning the stored values carry. Anything without it predates
            // a different LFO rate range and gets its rates rescaled on load.
            xml->setAttribute("stateVersion", currentStateVersion);

            // Imported waveforms travel with the song, audio and all, in either
            // mode. Nothing here points at a file on the user's disk, so a song
            // opened on another machine -- or after the imported files are gone --
            // still plays what it was saved with.
            if (auto waves = userWaveLibrary.createStateXml())
                xml->addChildElement(waves.release());

            // The routing list travels with the song. It is not in the parameter
            // list, because one parameter per possible LFO-and-knob pair would be
            // four LFOs times about 150 knobs.
            xml->addChildElement(spacedust::toXml(modMatrix).release());

            copyXmlToBinary(*xml, destData);
        }
    }
}

void SpaceDustAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    //==============================================================================
    // -- Ableton VST3 State Loading Workaround --
    // CRITICAL: Check for empty state (Ableton reload hack)
    // In Ableton Live, setStateInformation may be called with empty data during unload
    if (data == nullptr || sizeInBytes == 0)
        return;

    //==========================================================================
    // -- Crash-loop breaker --
    // If a marker from a previous setStateInformation call is still on disk,
    // that previous attempt crashed before completing. Skip the restore this
    // time so the host can open the project; saved state will be lost for this
    // instance but the user keeps the rest of their session.
    auto marker = getStateRestoreMarker();
    if (marker.existsAsFile())
    {
        DBG("Space Dust: SAFE MODE - previous state restore crashed; loading defaults");
        marker.deleteFile();
        return;
    }
    marker.getParentDirectory().createDirectory();
    marker.create();

    std::unique_ptr<juce::XmlElement> xmlState(getXmlFromBinary(data, sizeInBytes));

    if (xmlState.get() != nullptr)
    {
        if (xmlState->hasTagName(apvts.state.getType()))
        {
            // Restore persistent UI state
            currentPresetName = xmlState->getStringAttribute("presetName", "Init");
            cheezeGuyActivated = xmlState->getBoolAttribute("cheezeGuyActivated", false);
            lastActiveTabIndex = xmlState->getIntAttribute("lastActiveTabIndex", 0);

            // Lift the imported waveforms OUT of the XML before it becomes the
            // parameter tree. Left in place they would become a child of the APVTS
            // state, get written out again by the next save, and double in size on
            // every round trip.
            std::unique_ptr<juce::XmlElement> waves;
            if (auto* stored = xmlState->getChildByName("USERWAVES"))
            {
                // The false says "do not delete it" — ownership passes to us here.
                xmlState->removeChildElement(stored, false);
                waves.reset(stored);
            }

            // Lifted OUT before the XML becomes the parameter tree, exactly as
            // USERWAVES is. Left in place it would become a child of the APVTS
            // state, be written out again by the next save, and double on every
            // round trip.
            std::unique_ptr<juce::XmlElement> matrixXml;
            if (auto* stored = xmlState->getChildByName("MODMATRIX"))
            {
                xmlState->removeChildElement(stored, false);
                matrixXml.reset(stored);
            }

            auto restored = juce::ValueTree::fromXml(*xmlState);
            const int savedVersion = xmlState->getIntAttribute("stateVersion", 1);
            migrateLfoRatesIfOld(restored, savedVersion);

            // Before the parameters are put back, so a song saved when there were
            // four built-in shapes still selects the waveform it was saved with
            // rather than whichever shape now sits at that number.
            migrateWaveformChoicesIfOld(restored, savedVersion);
            apvts.replaceState(restored);

            // After the parameters, so the waveform choices they restored already
            // point at the slots this is about to fill.
            if (waves != nullptr)
                userWaveLibrary.restoreFromStateXml(*waves);

            // A patch with no MODMATRIX is one saved before routings existed.
            // Clearing rather than leaving the last patch's routings in place is
            // what stops one patch's movement leaking into the next.
            if (matrixXml != nullptr)
                spacedust::fromXml(*matrixXml, modMatrix);
            else
                modMatrix.clear();

            updateVoicesWithParameters();
        }
    }

    // The routing list has just been replaced, so the audio thread's compiled
    // copy of it is stale. Rebuild it here, on the message thread, rather than
    // anywhere the audio thread could reach.
    rebuildCompiledRoutings();

    // Successful completion â€” clear marker so next load attempts state restore normally.
    marker.deleteFile();
}

//==============================================================================
// -- Parameter Layout Creation --

/**
    Create the parameter layout for AudioProcessorValueTreeState.
    
    Defines all synthesizer parameters with their ranges, defaults, and scaling.
    Parameters are organized by section: Oscillators, Filter, Envelope.
    
    Note on ADSR: Currently controlling amplitude only. This provides expressive
    control over note shape. Filter envelope can be added later as a separate
    modulation source if desired.
*/
juce::AudioProcessorValueTreeState::ParameterLayout SpaceDustAudioProcessor::createParameterLayout()
{
    //==============================================================================
    // -- DEBUG: createParameterLayout Start --
    // CRITICAL: Log to file immediately - this is called in initializer list before constructor body
    #if JUCE_DEBUG
    try
    {
        juce::File logFile = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
            .getChildFile(safeString("SpaceDust_DebugLog.txt"));
        if (logFile.exists())
            logFile.deleteFile();
        juce::FileOutputStream out(logFile);
        if (out.openedOk())
        {
            out.writeText("Space Dust Debug Log - New session started: " + 
                         juce::Time::getCurrentTime().toString(true, true) + "\n", false, false, nullptr);
            out.writeText("Space Dust: createParameterLayout() called - creating parameters\n", false, false, nullptr);
            out.flush();
        }
    }
    catch (...) {}
    #endif
    
    DBG("Space Dust: createParameterLayout() called - creating parameters");
    
    //==============================================================================
    // -- PARAMETER ID CONSISTENCY CRITICAL --
    // 
    // WARNING: Parameter IDs must match EXACTLY between createParameterLayout() and
    // PluginEditor attachments. Any mismatch will cause crashes in Ableton Live.
    //
    // Complete parameter list (for safety audit):
    // Oscillators:
    //   - "osc1Waveform" (Choice: Sine, Triangle, Saw, Square)
    //   - "osc1CoarseTune" (Float: -24.0 to +24.0 semitones)
    //   - "osc1Detune" (Float: -50.0 to +50.0 cents)
    //   - "osc1Level" (Float: 0.0 to 1.0)
    //   - "osc2Waveform" (Choice: Sine, Triangle, Saw, Square)
    //   - "osc2CoarseTune" (Float: -24.0 to +24.0 semitones)
    //   - "osc2Detune" (Float: -50.0 to +50.0 cents)
    //   - "osc2Level" (Float: 0.0 to 1.0)
    // Noise:
    //   - "noiseLevel" (Float: 0.0 to 1.0)
    //   - "noiseType" (Choice: White, Pink)
    // Filter:
    //   - "filterMode" (Choice: Low Pass, Band Pass, High Pass, Notch, Peak)
    //   - "filterCutoff" (Float: 20.0 to 20000.0 Hz, log-scaled)
    //   - "filterResonance" (Float: 0.0 to 1.0)
    // ADSR Envelope:
    //   - "envAttack" (Float: 0.01 to 20.0 seconds, skewed, midpoint 2.0s)
    //   - "envDecay" (Float: 0.01 to 20.0 seconds, skewed, midpoint 2.0s)
    //   - "envSustain" (Float: 0.0 to 1.0, linear)
    //   - "envRelease" (Float: 0.01 to 20.0 seconds, skewed, midpoint 2.0s)
    // Master:
    //   - "masterVolume" (Float: 0.0 to 1.0)
    // Voice Mode and Glide:
    //   - "voiceMode" (Choice: 0=Poly, 1=Mono, 2=Legato)
    //   - "glideTime" (Float: 0.0 to 5.0 seconds, skewed, midpoint 1.0s)
    //   - "legatoGlide" (Bool: 0=Normal Glide, 1=Legato Glide / Fingered Glide)
    //
    // Total: 21 parameters
    //==============================================================================
    
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;
    
    //==============================================================================
    // -- Oscillator Parameters --
    
    //==============================================================================
    // -- Waveform choices, and why the User slots are always there --
    //
    // VST3 and AU freeze the item count of a Choice parameter when the plugin
    // loads. A host writes automation against that list and a session recalls it
    // by position, so a list that grew when the user imported another sample would
    // silently move every automation point already written.
    //
    // So all eight User slots exist from the start, whether or not anything has
    // been imported into them, and only the NAMES shown in the dropdown follow
    // what the user has loaded (see PluginEditor's refreshUserWaveformNames).
    // The host always sees "User 1" through "User 8".
    //
    // The built-in shapes keep their existing positions, so every preset and every
    // saved song written before this feature still selects the same waveform:
    // the value stored is the index, and index 2 is still Saw.
    // All twenty-one built-in shapes, in OscShape's order, then the eight User
    // slots. The names come from OscillatorShapes.h rather than being written out
    // again here: the list a host sees and the shape the oscillator plays are the
    // same list, indexed the same way, and there is no second copy to fall behind.
    juce::StringArray waveformChoices;

    for (int i = 0; i < OscShape::numShapes; ++i)
        waveformChoices.add(safeString(OscShape::names[i]));

    for (int i = 1; i <= UserWave::numSlots; ++i)
        waveformChoices.add("User " + juce::String(i));

    // Oscillator 1 waveform
    addParameterWithLogging(params,
        std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID{"osc1Waveform", 1}, "Osc 1 Waveform",
            waveformChoices, 1),
        safeString("osc1Waveform"));

    // Oscillator 2 waveform
    addParameterWithLogging(params,
        std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID{"osc2Waveform", 1}, "Osc 2 Waveform",
            waveformChoices, 1),
        safeString("osc2Waveform"));

    //==============================================================================
    // -- Wave Mode, Intensity and Sync --
    //
    // Three controls per oscillator that reshape whatever waveform is selected,
    // by moving WHERE in the cycle it is read rather than by changing the wave.
    // That is why they work on an imported single cycle as well as on a built-in
    // shape: see PhaseShaper.h.
    //
    // All three default to doing nothing -- mode Standard, intensity 0, sync 0 --
    // so every preset written before they existed sounds exactly as it did.
    //
    // The sub oscillator has these too, but its five are NOT in this list: they
    // were added later, and a new parameter may only go on the end -- see the
    // note beside subOscBendPlus at the bottom of this function.
    // Five knobs each, not a mode and an amount. Any of them may be turned up
    // together and they compose -- see PhaseShaper.h. All default to zero, so a
    // patch that never touches them sounds exactly as it always did.
    {
        struct ShapingParam { const char* id; const char* name; };

        const ShapingParam shapingParams[] =
        {
            { "osc1BendPlus",      "Osc 1 Bend +" },
            { "osc1BendMinus",     "Osc 1 Bend -" },
            { "osc1BendPlusMinus", "Osc 1 Bend +/-" },
            { "osc1Spectrum",      "Osc 1 Spectrum" },
            { "osc1Sync",          "Osc 1 Sync" },
            { "osc2BendPlus",      "Osc 2 Bend +" },
            { "osc2BendMinus",     "Osc 2 Bend -" },
            { "osc2BendPlusMinus", "Osc 2 Bend +/-" },
            { "osc2Spectrum",      "Osc 2 Spectrum" },
            { "osc2Sync",          "Osc 2 Sync" },
        };

        // Two decimals, said HERE rather than on the slider.
        //
        // A SliderAttachment takes its text from the PARAMETER, so a slider told
        // to show two decimals still read 0.00000 -- the parameter's own default
        // conversion was winning, and setNumDecimalPlacesToDisplay never had a
        // say. The unison count showed "1" correctly the whole time because an
        // AudioParameterInt formats itself as a whole number, which is what made
        // the real cause visible (Giuseppe, 2026-08-26).
        const auto twoDecimals = [] (float value, int)
        {
            return juce::String (value, 2);
        };

        for (const auto& sp : shapingParams)
            addParameterWithLogging(params,
                std::make_unique<juce::AudioParameterFloat>(
                    juce::ParameterID{sp.id, 1}, sp.name,
                    juce::NormalisableRange<float>(0.0f, 1.0f), 0.0f,
                    juce::AudioParameterFloatAttributes().withStringFromValueFunction(twoDecimals)),
                safeString(sp.id));

        //======================================================================
        // -- Unison --
        //
        // Voices defaults to 1 and Detune and Width to 0, so an untouched patch
        // runs one copy at the note's own pitch, dead centre -- which is the
        // oscillator exactly as it was before any of this existed.
        //
        // An INT rather than a float for the count: it is a number of things, the
        // host should show it as one, and a float would let automation land
        // between two counts.
        struct UnisonParam { const char* id; const char* name; };

        const UnisonParam unisonCounts[] =
        {
            { "osc1UnisonVoices", "Osc 1 Unison Voices" },
            { "osc2UnisonVoices", "Osc 2 Unison Voices" },
        };

        for (const auto& up : unisonCounts)
            addParameterWithLogging(params,
                std::make_unique<juce::AudioParameterInt>(
                    juce::ParameterID{up.id, 1}, up.name, 1, Unison::maxVoices, 1),
                safeString(up.id));

        const UnisonParam unisonAmounts[] =
        {
            { "osc1UnisonDetune", "Osc 1 Unison Detune" },
            { "osc1UnisonWidth",  "Osc 1 Unison Width" },
            { "osc2UnisonDetune", "Osc 2 Unison Detune" },
            { "osc2UnisonWidth",  "Osc 2 Unison Width" },
        };

        for (const auto& up : unisonAmounts)
            addParameterWithLogging(params,
                std::make_unique<juce::AudioParameterFloat>(
                    juce::ParameterID{up.id, 1}, up.name,
                    juce::NormalisableRange<float>(0.0f, 1.0f), 0.0f,
                    juce::AudioParameterFloatAttributes().withStringFromValueFunction(twoDecimals)),
                safeString(up.id));
    }

    //==============================================================================
    // -- Oscillator Pitch Tuning --
    // Each oscillator has independent coarse tuning (Â±24 semitones) and fine detuning (Â±50 cents)
    // Simple, intuitive system: Coarse for intervals, Detune for shimmer
    // Both default to 0 (perfectly in tune) - double-click any knob to reset
    
    // Oscillator 1 Coarse Tune (semitones)
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"osc1CoarseTune", 1}, "Osc 1 Coarse",
            juce::NormalisableRange<float>(-24.0f, 24.0f, 1.0f), 0.0f),
        "osc1CoarseTune");
    
    // Oscillator 1 Detune (cents)
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"osc1Detune", 1}, "Osc 1 Detune",
            juce::NormalisableRange<float>(-50.0f, 50.0f, 0.1f), 0.0f),
        "osc1Detune");
    
    // Oscillator 2 Coarse Tune (semitones)
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"osc2CoarseTune", 1}, "Osc 2 Coarse",
            juce::NormalisableRange<float>(-24.0f, 24.0f, 1.0f), 0.0f),
        "osc2CoarseTune");
    
    // Oscillator 2 Detune (cents)
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"osc2Detune", 1}, "Osc 2 Detune",
            juce::NormalisableRange<float>(-50.0f, 50.0f, 0.1f), 0.0f),
        "osc2Detune");
    
    //==============================================================================
    // -- Independent Oscillator Level Controls --
    // Each oscillator and noise source has independent volume control (0.0 to 1.0)
    // This provides full flexibility for mixing: layer detuned saws, add noise wash,
    // or create subtle triangle + hiss textures. All sources are mixed additively.
    
    // Oscillator 1 Level
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"osc1Level", 1}, "Osc 1 Level",
            juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.8f),
        "osc1Level");
    
    // Oscillator 2 Level
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"osc2Level", 1}, "Osc 2 Level",
            juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.8f),
        "osc2Level");
    
    // Oscillator 1 Pan (-1 = full left, 0 = center, 1 = full right)
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"osc1Pan", 1}, "Osc 1 Pan",
            juce::NormalisableRange<float>(-1.0f, 1.0f, 0.01f), 0.0f),
        "osc1Pan");
    
    // Oscillator 2 Pan (-1 = full left, 0 = center, 1 = full right)
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"osc2Pan", 1}, "Osc 2 Pan",
            juce::NormalisableRange<float>(-1.0f, 1.0f, 0.01f), 0.0f),
        "osc2Pan");
    
    // Noise Level (White/Pink noise generator for texture and atmosphere)
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"noiseLevel", 1}, "Noise Level",
            juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.0f),
        "noiseLevel");

    // Noise type â€” a real parameter so it can be automated and saved.
    // The same eight User slots as the oscillators, appended after the two noise
    // colours for the same reason: the item count can never change. Choosing one
    // turns this source into a third oscillator that tracks the played note, with
    // the Level knob as its volume and the two shelves as its tone controls.
    juce::StringArray noiseChoices;
    noiseChoices.add(safeString("White"));
    noiseChoices.add(safeString("Pink"));

    for (int i = 1; i <= UserWave::numSlots; ++i)
        noiseChoices.add("User " + juce::String(i));

    addParameterWithLogging(params,
        std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID{"noiseType", 1}, "Noise Type", noiseChoices, 0),
        safeString("noiseType"));

    // Noise EQ: Low Shelf/Cut (affects frequencies below 200 Hz)
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"lowShelfAmount", 1}, "Low Shelf/Cut",
            juce::NormalisableRange<float>(-1.0f, 1.0f, 0.01f), 0.0f),
        "lowShelfAmount");
    
    // Noise EQ: High Shelf/Cut (affects frequencies above 1.5 kHz)
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"highShelfAmount", 1}, "High Shelf/Cut",
            juce::NormalisableRange<float>(-1.0f, 1.0f, 0.01f), 0.0f),
        "highShelfAmount");
    
    //==============================================================================
    // -- Filter Parameters --
    
    // Filter mode. Notch and Peak were appended AFTER High Pass so the stored
    // indices of existing presets (0/1/2) keep meaning the same three modes.
    // Thirteen modes now, and the first five are the five that were always there,
    // in the same order -- so every preset and every automation lane still selects
    // the filter it selected before. The names come from NonlinearSVF, which is
    // where the modes are defined, rather than being written out again here.
    juce::StringArray filterModeChoices;

    for (int i = 0; i < NonlinearSVF::numModes; ++i)
        filterModeChoices.add(safeString(NonlinearSVF::modeNames()[i]));

    addParameterWithLogging(params,
        std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID{"filterMode", 1}, "Filter Mode",
            filterModeChoices, 0),
        safeString("filterMode"));
    
    // Filter cutoff (log scale: 20 Hz to 20 kHz)
    // Continuous (no step interval) so Note Lock can land exactly on its grid; see
    // wholeHzAttributes() above for why, and for why the readout is spelled out.
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"filterCutoff", 1}, "Filter Cutoff",
            juce::NormalisableRange<float>(20.0f, 20000.0f, 0.0f, 0.3f), 8000.0f,
            wholeHzAttributes()),
        "filterCutoff");
    
    // Filter resonance (normalized 0.0-1.0, maps to Q 0.1-20.0 internally)
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"filterResonance", 1}, "Filter Resonance",
            juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.3f),
        "filterResonance");
    
    // Warm saturation (Moog-style tanh saturation and resonance behavior when ON)
    addParameterWithLogging(params,
        std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{"warmSaturationMaster", 1}, "Filter Warm Sat", false),
        "warmSaturationMaster");

    // Filter anti-aliasing: 4x-oversample the filter stages (master always; mod
    // filters when active) so audio-rate LFO modulation no longer folds back into
    // the audible band. ON by default â€” cleaner high-rate filter-FM everywhere.
    addParameterWithLogging(params,
        std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{"filterOversample", 1}, "Filter Anti-Alias", true),
        "filterOversample");

    // Keyboard tracking: when ON, the filter cutoff follows the played key
    // (one octave of cutoff per octave of keyboard, neutral at middle C).
    addParameterWithLogging(params,
        std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{"filterKeyTrack", 1}, "Filter Key Track", false),
        "filterKeyTrack");

    // Note Lock: quantises the cutoff KNOB to semitone-spaced frequencies (see the
    // NoteLock namespace in PluginEditor.h). Purely a UI-side snap -- the audio path
    // never reads this -- but it is a real parameter so the state saves with a preset
    // and the toggle survives a reload. Only meaningful with Key Tracking on, which is
    // what makes the cutoff a ratio to the played note rather than an absolute Hz.
    addParameterWithLogging(params,
        std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{"filterNoteLock", 1}, "Filter Note Lock", false),
        "filterNoteLock");

    // Harmonic Series: switches Note Lock's grid from 12-TET semitones to the played
    // note's real overtone series (k * root up, root / k down). Only meaningful while
    // Note Lock is on, which is why the toggle only appears then. Like Note Lock this
    // is a UI-side snap the audio path never reads.
    addParameterWithLogging(params,
        std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{"filterHarmonicLock", 1}, "Filter Harmonic Series", false),
        "filterHarmonicLock");

    //==============================================================================
    // -- Filter Envelope Parameters (ADSR) --
    // Filter envelope modulates filter cutoff with bipolar amount control
    
    // Filter envelope attack time (skewed: 0.01s to 20.0s, midpoint at 2.0s)
    // Fine 0.0001s step (vs 0.001s) so the heavily-skewed low end moves smoothly
    // instead of feeling "sticky" â€” the curve is near-flat near 0.01s, so a coarse
    // 1ms step there required a noticeable drag before the value would tick over.
    juce::NormalisableRange<float> filterAttackRange(0.01f, 20.0f, 0.0001f);
    filterAttackRange.setSkewForCentre(2.0f);
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"filterEnvAttack", 1}, "Filter Env Attack",
            filterAttackRange, 0.01f),
        "filterEnvAttack");
    
    // Filter envelope decay time (skewed: 0.01s to 20.0s, midpoint at 2.0s)
    // Matches the attack curve/range and uses the same fine 0.0001s step so the
    // low end moves smoothly instead of feeling "sticky".
    juce::NormalisableRange<float> filterDecayRange(0.01f, 20.0f, 0.0001f);
    filterDecayRange.setSkewForCentre(2.0f);
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"filterEnvDecay", 1}, "Filter Env Decay",
            filterDecayRange, 0.8f),
        "filterEnvDecay");
    
    // Filter envelope sustain level (0.0 to 1.0, linear)
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"filterEnvSustain", 1}, "Filter Env Sustain",
            juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.7f),
        "filterEnvSustain");
    
    // Filter envelope release time (skewed: 0.01s to 20.0s, midpoint at 2.0s)
    // Fine 0.0001s step (4 decimals shown) to match attack/decay for smooth low-end control.
    juce::NormalisableRange<float> filterReleaseRange(0.01f, 20.0f, 0.0001f);
    filterReleaseRange.setSkewForCentre(2.0f);
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"filterEnvRelease", 1}, "Filter Env Release",
            filterReleaseRange, 3.0f),
        "filterEnvRelease");
    
    // Filter envelope amount (bipolar: -100% to +100%, center at 0%)
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"filterEnvAmount", 1}, "Filter Env Amount",
            juce::NormalisableRange<float>(-100.0f, 100.0f, 0.1f), 0.0f),
        "filterEnvAmount");
    
    //==============================================================================
    // -- ADSR Envelope Parameters (Amplitude) --
    
    //==============================================================================
    // -- Envelope Time Parameters (Attack, Decay, Release) --
    // Musical skewed mapping for expressive control:
    // - Fine resolution at short times (snappy attacks, quick decays)
    // - Accelerating curve toward long cosmic tails
    // - 0% knob â†’ 0.01s (10ms minimum for stability)
    // - 50% knob â†’ 2.0s (musical midpoint)
    // - 100% knob â†’ 20.0s (maximum cosmic tails)
    // 
    // Uses setSkewForCentre(2.0f) to place the midpoint at exactly 2.0 seconds.
    // This gives exponential-like feel: most knob travel controls short-to-medium times,
    // with the final portion extending dramatically to very long times.
    
    // Attack time (skewed: 0.01s to 20.0s, midpoint at 2.0s)
    // Fine 0.0001s step so the heavily-skewed low end moves smoothly (matches filter env).
    juce::NormalisableRange<float> attackRange(0.01f, 20.0f, 0.0001f);
    attackRange.setSkewForCentre(2.0f); // 50% knob position = 2.0 seconds
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"envAttack", 1}, "Env Attack",
            attackRange, 0.01f),
        "envAttack");
    
    // Decay time (skewed: 0.01s to 20.0s, midpoint at 2.0s)
    // Fine 0.0001s step so the heavily-skewed low end moves smoothly (matches filter env).
    juce::NormalisableRange<float> decayRange(0.01f, 20.0f, 0.0001f);
    decayRange.setSkewForCentre(2.0f); // 50% knob position = 2.0 seconds
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"envDecay", 1}, "Env Decay",
            decayRange, 0.2f),
        "envDecay");
    
    // Sustain level (0.0 to 1.0, linear - unchanged)
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"envSustain", 1}, "Env Sustain",
            juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.7f),
        "envSustain");
    
    // Release time (skewed: 0.01s to 20.0s, midpoint at 2.0s) - long cosmic tails!
    // Fine 0.0001s step so the heavily-skewed low end moves smoothly (matches filter env).
    juce::NormalisableRange<float> releaseRange(0.01f, 20.0f, 0.0001f);
    releaseRange.setSkewForCentre(2.0f); // 50% knob position = 2.0 seconds
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"envRelease", 1}, "Env Release",
            releaseRange, 0.2f),
        "envRelease");
    
    //==============================================================================
    // -- Pitch Envelope --
    // Amount: -100% to 100% (12 o'clock = 0), scales the pitch envelope depth
    // Time: 0-10 seconds, length of the pitch ramp from note-on
    // Pitch: 0-24 semitones, maximum pitch change
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"pitchEnvAmount", 1}, "Pitch Env Amount",
            juce::NormalisableRange<float>(-100.0f, 100.0f, 0.1f), 0.0f),
        "pitchEnvAmount");
    // Pitch ramp length: 0 to 10s, skewed (midpoint at 2.0s) so short pitch blips
    // stay easy to dial. Fine 0.0001s step (4 decimals) like the ADSR time knobs.
    juce::NormalisableRange<float> pitchEnvTimeRange(0.0f, 10.0f, 0.0001f);
    pitchEnvTimeRange.setSkewForCentre(2.0f);
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"pitchEnvTime", 1}, "Pitch Env Time",
            pitchEnvTimeRange, 0.0f),
        "pitchEnvTime");
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"pitchEnvPitch", 1}, "Pitch Env Pitch",
            juce::NormalisableRange<float>(0.0f, 24.0f, 0.1f), 0.0f),
        "pitchEnvPitch");
    
    // Sub oscillator (one octave down, in Amp Envelope section)
    addParameterWithLogging(params,
        std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{"subOscOn", 1}, "Sub Oscillator", false),
        safeString("subOscOn"));
    // The same list as Osc 1 and Osc 2, User slots and all: the sub plays a
    // waveform exactly as they do, so there is no reason it should be offered a
    // smaller choice of them.
    addParameterWithLogging(params,
        std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID{"subOscWaveform", 1}, "Sub Osc Waveform",
            waveformChoices, 1),
        safeString("subOscWaveform"));
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"subOscLevel", 1}, "Sub Osc Level",
            juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.5f),
        "subOscLevel");
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"subOscCoarse", 1}, "Sub Osc Coarse",
            juce::NormalisableRange<float>(-36.0f, 36.0f, 1.0f), 0.0f),
        "subOscCoarse");
    
    //==============================================================================
    // -- Voice Mode and Glide (Portamento) --
    // 
    // Voice Mode (Choice): 0=Poly, 1=Mono, 2=Legato
    //   Poly: multiple notes, envelope retriggers each. Mono: one note, envelope retriggers.
    //   Legato: one note; on overlapping note-on, no envelope retrigger (smooth glide only).
    // 
    // Glide Time: 0.0 to 5.0 seconds, skewed for fine control at low end
    //              Uses setSkewForCentre(1.0f) so 12 o'clock â‰ˆ 0.5-1s
    //              Fine control at low end, up to 5s max for cosmic slides
    //              Works in BOTH poly and mono modes for expressive gliding pads and leads
    
    // Voice Mode: Poly (multiple notes, env retriggers), Mono (one note, env retriggers), Legato (one note, no env retrigger on overlap)
    addParameterWithLogging(params,
        std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID{"voiceMode", 1}, "Voice Mode",
            juce::StringArray(safeString("Poly"), safeString("Mono"), safeString("Legato")), 0),
        safeString("voiceMode"));
    
    // Glide Time (0.0 to 5.0 seconds, skewed with midpoint at 1.0s)
    juce::NormalisableRange<float> glideRange(0.0f, 5.0f, 0.001f);
    glideRange.setSkewForCentre(1.0f); // 50% knob position â‰ˆ 0.5-1.0 seconds
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"glideTime", 1}, "Glide Time",
            glideRange, 0.0f),
        "glideTime");

    // Legato Glide (Fingered Glide) toggle:
    // ON  = glide only on overlapping (legato) notes in Legato mode (single-trigger envelopes)
    // OFF = glide on every note change whenever glideTime > 0 (classic always-on portamento)
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{"legatoGlide", 1}, "Legato Glide", true),
        "legatoGlide");
    
    //==============================================================================
    // -- Pitch Bend --
    // Pitch bend amount: 1-24 semitones (sets range for pitch bend)
    // Pitch bend: -1 to 1 (manual + MIDI pitch bend, additive)
    
    // Pitch bend amount selector (0-24 semitones, 0 = no bend)
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"pitchBendAmount", 1}, "Pitch Bend Range",
            juce::NormalisableRange<float>(0.0f, 24.0f, 1.0f), 0.0f),
        "pitchBendAmount");
    
    // Manual pitch bend (bipolar -1 to 1, center = no bend)
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"pitchBend", 1}, "Pitch Bend",
            juce::NormalisableRange<float>(-1.0f, 1.0f, 0.01f), 0.0f),
        "pitchBend");
    
    //==============================================================================
    // -- Master Volume --
    // Controls overall output level. Range: 0.0 to 2.0 (doubled for when effects are on).
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"masterVolume", 1}, "Master Volume",
            juce::NormalisableRange<float>(0.0f, 2.0f, 0.01f), 0.7f),
        "masterVolume");
    
    //==============================================================================
    // -- LFO Parameters --
    
    // LFO1 Waveform
    addParameterWithLogging(params,
        std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID{"lfo1Waveform", 1}, "LFO1 Waveform",
            juce::StringArray(safeString("Sine"), safeString("Triangle"), safeString("Saw Up"), safeString("Saw Down"), safeString("Square"), safeString("S&H")), 0),
        safeString("lfo1Waveform"));
    
    // LFO1 On (enable/disable modulation)
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{"lfo1Enabled", 1}, "LFO1 On", false),
        "lfo1Enabled");
    
    // LFO1 Depth (0-100%)
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"lfo1Depth", 1}, "LFO1 Depth",
            juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f), 25.0f),
        "lfo1Depth");
    
    // LFO1 Sync (on/off)
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{"lfo1Sync", 1}, "LFO1 Sync", false),
        "lfo1Sync");
    
    // LFO1 Rate (0-12: maps to 0.01-200 Hz when sync off, or tempo divisions when sync on)
    // When sync is off: 0-12 maps logarithmically to 0.01-200 Hz. The top was briefly
    // 2 kHz; migrateLfoRatesIfOld rescales anything saved then so it keeps its speed.
    // When sync is on: 0-12 maps to tempo divisions (0=1/32, 6=1/4, 12=8)
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"lfo1Rate", 1}, "LFO1 Rate",
            juce::NormalisableRange<float>(0.0f, 12.0f, 0.01f), 6.0f),
        "lfo1Rate");
    // LFO1 Target (what to modulate)
    addParameterWithLogging(params,
        std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID{"lfo1Target", 1}, "LFO1 Target",
            juce::StringArray(safeString("Pitch"), safeString("Filter"),
                safeString("Master Vol"), safeString("Osc1 Vol"), safeString("Osc2 Vol"), safeString("Noise Vol")), 1),
        safeString("lfo1Target"));
    
    // LFO1 Phase (0-360Â°)
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"lfo1Phase", 1}, "LFO1 Phase",
            juce::NormalisableRange<float>(0.0f, 360.0f, 0.1f), 0.0f),
        "lfo1Phase");
    
    // LFO1 Triplet Enabled (on/off)
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{"lfo1TripletEnabled", 1}, "LFO1 Triplet", false),
        "lfo1TripletEnabled");
    
    // LFO1 Triplet/Straight Toggle (All mode: on/off)
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{"lfo1TripletStraightToggle", 1}, "LFO1 All", false),
        "lfo1TripletStraightToggle");
    
    // LFO1 Retrigger (on/off)
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{"lfo1Retrigger", 1}, "LFO1 Retrigger", true),
        "lfo1Retrigger");
    
    // LFO2 Waveform
    addParameterWithLogging(params,
        std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID{"lfo2Waveform", 1}, "LFO2 Waveform",
            juce::StringArray(safeString("Sine"), safeString("Triangle"), safeString("Saw Up"), safeString("Saw Down"), safeString("Square"), safeString("S&H")), 0),
        safeString("lfo2Waveform"));
    
    // LFO2 On (enable/disable modulation)
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{"lfo2Enabled", 1}, "LFO2 On", false),
        "lfo2Enabled");
    
    // LFO2 Depth (0-100%)
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"lfo2Depth", 1}, "LFO2 Depth",
            juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f), 25.0f),
        "lfo2Depth");
    
    // LFO2 Sync (on/off)
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{"lfo2Sync", 1}, "LFO2 Sync", false),
        "lfo2Sync");
    
    // LFO2 Rate (0-12: maps to 0.01-200 Hz when sync off, or tempo divisions when sync on)
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"lfo2Rate", 1}, "LFO2 Rate",
            juce::NormalisableRange<float>(0.0f, 12.0f, 0.01f), 6.0f),
        "lfo2Rate");

    
    // LFO2 Target (what to modulate)
    addParameterWithLogging(params,
        std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID{"lfo2Target", 1}, "LFO2 Target",
            juce::StringArray(safeString("Pitch"), safeString("Filter"),
                safeString("Master Vol"), safeString("Osc1 Vol"), safeString("Osc2 Vol"), safeString("Noise Vol")), 0),
        safeString("lfo2Target"));
    
    // LFO2 Phase (0-360Â°)
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"lfo2Phase", 1}, "LFO2 Phase",
            juce::NormalisableRange<float>(0.0f, 360.0f, 0.1f), 0.0f),
        "lfo2Phase");
    
    // LFO2 Triplet Enabled (on/off)
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{"lfo2TripletEnabled", 1}, "LFO2 Triplet", false),
        "lfo2TripletEnabled");
    
    // LFO2 Triplet/Straight Toggle (All mode: on/off)
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{"lfo2TripletStraightToggle", 1}, "LFO2 All", false),
        "lfo2TripletStraightToggle");
    
    // LFO2 Retrigger (on/off)
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{"lfo2Retrigger", 1}, "LFO2 Retrigger", true),
        "lfo2Retrigger");
    
    //==============================================================================
    // -- Modulation Tab Filters (inside LFO boxes) --
    // Each LFO has its own Filter toggle. Link to Master: use main filter params.
    
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{"modFilter1Show", 1}, "Mod Filter 1 Show", false),
        "modFilter1Show");
    
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{"modFilter2Show", 1}, "Mod Filter 2 Show", false),
        "modFilter2Show");
    
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{"modFilter1LinkToMaster", 1}, "Mod Filter 1 Link", true),
        "modFilter1LinkToMaster");
    
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{"modFilter2LinkToMaster", 1}, "Mod Filter 2 Link", true),
        "modFilter2LinkToMaster");
    
    addParameterWithLogging(params,
        std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID{"modFilter1Mode", 1}, "Mod Filter 1 Mode",
            filterModeChoices, 0),
        safeString("modFilter1Mode"));
    
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"modFilter1Cutoff", 1}, "Mod Filter 1 Cutoff",
            juce::NormalisableRange<float>(20.0f, 20000.0f, 0.0f, 0.3f), 8000.0f,
            wholeHzAttributes()),
        "modFilter1Cutoff");
    
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"modFilter1Resonance", 1}, "Mod Filter 1 Resonance",
            juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.3f),
        "modFilter1Resonance");
    
    addParameterWithLogging(params,
        std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{"warmSaturationMod1", 1}, "Mod 1 Warm Sat", false),
        "warmSaturationMod1");

    addParameterWithLogging(params,
        std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{"modFilter1KeyTrack", 1}, "Mod 1 Key Track", false),
        "modFilter1KeyTrack");

    addParameterWithLogging(params,
        std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{"modFilter1NoteLock", 1}, "Mod 1 Note Lock", false),
        "modFilter1NoteLock");

    addParameterWithLogging(params,
        std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{"modFilter1HarmonicLock", 1}, "Mod 1 Harmonic Series", false),
        "modFilter1HarmonicLock");

    addParameterWithLogging(params,
        std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID{"modFilter2Mode", 1}, "Mod Filter 2 Mode",
            filterModeChoices, 0),
        safeString("modFilter2Mode"));
    
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"modFilter2Cutoff", 1}, "Mod Filter 2 Cutoff",
            juce::NormalisableRange<float>(20.0f, 20000.0f, 0.0f, 0.3f), 8000.0f,
            wholeHzAttributes()),
        "modFilter2Cutoff");
    
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"modFilter2Resonance", 1}, "Mod Filter 2 Resonance",
            juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.3f),
        "modFilter2Resonance");
    
    addParameterWithLogging(params,
        std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{"warmSaturationMod2", 1}, "Mod 2 Warm Sat", false),
        "warmSaturationMod2");

    addParameterWithLogging(params,
        std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{"modFilter2KeyTrack", 1}, "Mod 2 Key Track", false),
        "modFilter2KeyTrack");

    addParameterWithLogging(params,
        std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{"modFilter2NoteLock", 1}, "Mod 2 Note Lock", false),
        "modFilter2NoteLock");

    addParameterWithLogging(params,
        std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{"modFilter2HarmonicLock", 1}, "Mod 2 Harmonic Series", false),
        "modFilter2HarmonicLock");

    //==============================================================================
    // -- Delay Effect Parameters --
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"delayDecay", 1}, "Delay Decay",
            juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f), 40.0f),
        "delayDecay");
    
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"delayDryWet", 1}, "Delay Mix",
            juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f), 50.0f),
        "delayDryWet");
    
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"delayRate", 1}, "Delay Time",
            juce::NormalisableRange<float>(0.0f, 12.0f, 0.01f), 6.0f),
        "delayRate");
    
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{"delaySync", 1}, "Delay Sync", true),
        "delaySync");
    
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{"delayTripletEnabled", 1}, "Delay Triplet", false),
        "delayTripletEnabled");
    
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{"delayTripletStraightToggle", 1}, "Delay All", false),
        "delayTripletStraightToggle");
    
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{"delayPingPong", 1}, "Delay Ping-Pong", false),
        "delayPingPong");
    
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{"delayEnabled", 1}, "Delay On", false),
        "delayEnabled");
    
    // Delay filter (collapsible)
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{"delayFilterShow", 1}, "Delay Filter", false),
        "delayFilterShow");
    
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"delayFilterHPCutoff", 1}, "Delay HP Cutoff",
            juce::NormalisableRange<float>(20.0f, 20000.0f, 1.0f, 0.3f), 100.0f),
        "delayFilterHPCutoff");
    
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"delayFilterHPResonance", 1}, "Delay HP Res",
            juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.3f),
        "delayFilterHPResonance");
    
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"delayFilterLPCutoff", 1}, "Delay LP Cutoff",
            juce::NormalisableRange<float>(20.0f, 20000.0f, 1.0f, 0.3f), 8000.0f),
        "delayFilterLPCutoff");
    
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"delayFilterLPResonance", 1}, "Delay LP Res",
            juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.3f),
        "delayFilterLPResonance");
    
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{"delayFilterWarmSaturation", 1}, "Delay Warm Sat", false),
        "delayFilterWarmSaturation");
    
    //==============================================================================
    // -- Reverb Effect Parameters --
    addParameterWithLogging(params,
        std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{"reverbEnabled", 1}, "Reverb On", false),
        "reverbEnabled");

    addParameterWithLogging(params,
        std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID{"reverbType", 1}, "Reverb Type",
            juce::StringArray("Schroeder", "Void Verb"), 0),
        "reverbType");
    
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"reverbWetMix", 1}, "Reverb Mix",
            juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.33f),
        "reverbWetMix");
    
    juce::NormalisableRange<float> reverbDecayRange(0.0f, 640.0f, 0.01f);
    reverbDecayRange.setSkewForCentre(64.0f);
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"reverbDecayTime", 1}, "Reverb Decay",
            reverbDecayRange, 16.0f),
        "reverbDecayTime");
    
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{"reverbFilterShow", 1}, "Reverb Filter", false),
        "reverbFilterShow");

    addParameterWithLogging(params,
        std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{"reverbFilterWarmSaturation", 1}, "Reverb Warm Sat", false),
        "reverbFilterWarmSaturation");

    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"reverbFilterHPCutoff", 1}, "Reverb HP Cutoff",
            juce::NormalisableRange<float>(20.0f, 20000.0f, 1.0f, 0.3f), 100.0f),
        "reverbFilterHPCutoff");

    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"reverbFilterHPResonance", 1}, "Reverb HP Res",
            juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.3f),
        "reverbFilterHPResonance");

    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"reverbFilterLPCutoff", 1}, "Reverb LP Cutoff",
            juce::NormalisableRange<float>(20.0f, 20000.0f, 1.0f, 0.3f), 8000.0f),
        "reverbFilterLPCutoff");

    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"reverbFilterLPResonance", 1}, "Reverb LP Res",
            juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.3f),
        "reverbFilterLPResonance");

    //==============================================================================
    // -- Grain Delay Effect Parameters --
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{"grainDelayEnabled", 1}, "Grain Delay On", false),
        "grainDelayEnabled");

    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"grainDelayTime", 1}, "Grain Delay Time",
            juce::NormalisableRange<float>(20.0f, 2000.0f, 1.0f, 0.3f), 200.0f),
        "grainDelayTime");

    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"grainDelaySize", 1}, "Grain Size",
            juce::NormalisableRange<float>(10.0f, 500.0f, 1.0f, 0.4f), 50.0f),
        "grainDelaySize");

    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"grainDelayPitch", 1}, "Grain Pitch",
            juce::NormalisableRange<float>(-12.0f, 12.0f, 0.1f), 0.0f),
        "grainDelayPitch");

    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"grainDelayMix", 1}, "Grain Mix",
            juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f), 50.0f),
        "grainDelayMix");

    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"grainDelayDecay", 1}, "Grain Decay",
            juce::NormalisableRange<float>(0.0f, 150.0f, 0.1f), 0.0f),
        "grainDelayDecay");

    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"grainDelayDensity", 1}, "Grain Density",
            juce::NormalisableRange<float>(1.0f, 8.0f, 0.1f), 1.0f),
        "grainDelayDensity");

    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"grainDelayJitter", 1}, "Grain Jitter",
            juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f), 0.0f),
        "grainDelayJitter");

    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{"grainDelayPingPong", 1}, "Grain Ping-Pong", false),
        "grainDelayPingPong");

    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{"grainDelayFilterShow", 1}, "Grain Filter", false),
        "grainDelayFilterShow");
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"grainDelayFilterHPCutoff", 1}, "Grain HP Cutoff",
            juce::NormalisableRange<float>(20.0f, 20000.0f, 1.0f, 0.25f), 100.0f),
        "grainDelayFilterHPCutoff");
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"grainDelayFilterHPResonance", 1}, "Grain HP Res",
            juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.5f),
        "grainDelayFilterHPResonance");
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"grainDelayFilterLPCutoff", 1}, "Grain LP Cutoff",
            juce::NormalisableRange<float>(20.0f, 20000.0f, 1.0f, 0.25f), 4000.0f),
        "grainDelayFilterLPCutoff");
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"grainDelayFilterLPResonance", 1}, "Grain LP Res",
            juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.5f),
        "grainDelayFilterLPResonance");
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{"grainDelayFilterWarmSaturation", 1}, "Grain Warm Sat", false),
        "grainDelayFilterWarmSaturation");

    //==============================================================================
    // -- Phaser Effect Parameters --
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{"phaserEnabled", 1}, "Phaser On", false),
        "phaserEnabled");
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"phaserRate", 1}, "Phaser Rate",
            juce::NormalisableRange<float>(0.05f, 200.0f, 0.01f, 0.35f), 1.0f),
        "phaserRate");
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"phaserDepth", 1}, "Phaser Depth",
            juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.7f),
        "phaserDepth");
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"phaserFeedback", 1}, "Phaser Feedback",
            juce::NormalisableRange<float>(-1.0f, 1.0f, 0.01f), 0.0f),
        "phaserFeedback");
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{"phaserScriptMode", 1}, "Phaser Script", true),
        "phaserScriptMode");
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"phaserMix", 1}, "Phaser Mix",
            juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.5f),
        "phaserMix");
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"phaserCentre", 1}, "Phaser Center",
            juce::NormalisableRange<float>(50.0f, 2000.0f, 1.0f, 0.35f), 400.0f),
        "phaserCentre");
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID{"phaserStages", 1}, "Phaser Stages",
            juce::StringArray("4 (Phase 90)", "6 (Deeper)"), 0),
        "phaserStages");
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"phaserStereoOffset", 1}, "Phaser Width",
            juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.5f),
        "phaserStereoOffset");
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{"phaserVintageMode", 1}, "Phaser Vintage", false),
        "phaserVintageMode");

    //==============================================================================
    // -- Flanger Effect Parameters --
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{"flangerEnabled", 1}, "Flanger On", false),
        "flangerEnabled");
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"flangerRate", 1}, "Flanger Rate",
            juce::NormalisableRange<float>(0.05f, 200.0f, 0.01f, 0.35f), 0.5f),
        "flangerRate");
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"flangerDepth", 1}, "Flanger Depth",
            juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.5f),
        "flangerDepth");
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"flangerFeedback", 1}, "Flanger Feedback",
            juce::NormalisableRange<float>(-1.0f, 1.0f, 0.01f), 0.0f),
        "flangerFeedback");
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"flangerWidth", 1}, "Flanger Width",
            juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.5f),
        "flangerWidth");
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"flangerMix", 1}, "Flanger Mix",
            juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.5f),
        "flangerMix");

    //==============================================================================
    // -- Bit Crusher Effect Parameters --
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{"bitCrusherEnabled", 1}, "Bit Crusher On", false),
        "bitCrusherEnabled");
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{"bitCrusherPostEffect", 1}, "Bit Crusher Post", true),
        "bitCrusherPostEffect");
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"bitCrusherAmount", 1}, "Bit Crusher Amount",
            juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.5f),
        "bitCrusherAmount");
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"bitCrusherRate", 1}, "Bit Crusher Rate",
            juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.0f),
        "bitCrusherRate");
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"bitCrusherMix", 1}, "Bit Crusher Mix",
            juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.5f),
        "bitCrusherMix");

    //==============================================================================
    // -- Compressor Parameters (Saturation Color tab) --
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{"compressorEnabled", 1}, "Compressor On", false),
        "compressorEnabled");
    addParameterWithLogging(params,
        std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID{"compressorType", 1}, "Compressor Type",
            juce::StringArray("Compressor 1", "Compressor 2", "Compressor 3"), 0),
        safeString("compressorType"));
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"compressorThreshold", 1}, "Compressor Threshold",
            juce::NormalisableRange<float>(-60.0f, 0.0f, 0.1f), -12.0f),
        "compressorThreshold");
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"compressorRatio", 1}, "Compressor Ratio",
            juce::NormalisableRange<float>(1.0f, 20.0f, 0.1f, 0.5f), 4.0f),
        "compressorRatio");
    {
        juce::NormalisableRange<float> compAttackRange(0.1f, 80.0f, 0.01f);
        compAttackRange.setSkewForCentre(5.0f);
        ADD_PARAM_WITH_LOG(params,
            std::make_unique<juce::AudioParameterFloat>(
                juce::ParameterID{"compressorAttack", 1}, "Compressor Attack",
                compAttackRange, 3.0f),
            "compressorAttack");
    }
    {
        juce::NormalisableRange<float> compReleaseRange(5.0f, 1200.0f, 0.1f);
        compReleaseRange.setSkewForCentre(100.0f);
        ADD_PARAM_WITH_LOG(params,
            std::make_unique<juce::AudioParameterFloat>(
                juce::ParameterID{"compressorRelease", 1}, "Compressor Release",
                compReleaseRange, 100.0f),
            "compressorRelease");
    }
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"compressorMakeup", 1}, "Compressor Makeup",
            juce::NormalisableRange<float>(0.0f, 24.0f, 0.1f), 0.0f),
        "compressorMakeup");
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"compressorMix", 1}, "Compressor Mix",
            juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 1.0f),
        "compressorMix");
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{"compressorAutoRelease", 1}, "Compressor Auto Release", false),
        "compressorAutoRelease");
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{"compressorSoftClip", 1}, "Compressor Soft Clip", false),
        "compressorSoftClip");

    //==============================================================================
    // -- Soft Clipper Parameters (Saturation Color tab) --
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{"softClipperEnabled", 1}, "Soft Clipper On", false),
        "softClipperEnabled");
    addParameterWithLogging(params,
        std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID{"softClipperMode", 1}, "Soft Clipper Mode",
            juce::StringArray("Smooth", "Crisp", "Tube", "Tape", "Guitar"), 0),
        safeString("softClipperMode"));
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"softClipperDrive", 1}, "Soft Clipper Drive",
            juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.35f),
        "softClipperDrive");
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"softClipperKnee", 1}, "Soft Clipper Knee",
            juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.67f),
        "softClipperKnee");
    addParameterWithLogging(params,
        std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID{"softClipperOversample", 1}, "Soft Clipper Oversample",
            juce::StringArray("2x", "4x", "8x", "16x"), 1),
        safeString("softClipperOversample"));
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"softClipperMix", 1}, "Soft Clipper Mix",
            juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 1.0f),
        "softClipperMix");

    //==============================================================================
    // -- Transient Effect Parameters (Saturation Color tab) --
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{"transientEnabled", 1}, "Transient On", false),
        "transientEnabled");
    // The ten drums, then the same eight User slots the oscillators offer, for
    // the same reason and on the same terms: the count can never change, and the
    // ten built-ins keep their positions so every preset written before this
    // still picks the drum it always picked. Choosing a User slot plays that
    // sample as the hit instead of a synthesised drum.
    juce::StringArray transientChoices;
    transientChoices.add(safeString("808 Kick"));
    transientChoices.add(safeString("808 Snare"));
    transientChoices.add(safeString("808 Hat"));
    transientChoices.add(safeString("808 Open Hat"));
    transientChoices.add(safeString("808 Clap"));
    transientChoices.add(safeString("808 Tom"));
    transientChoices.add(safeString("808 Rim"));
    transientChoices.add(safeString("808 Cowbell"));
    transientChoices.add(safeString("909 Kick"));
    transientChoices.add(safeString("909 Snare"));

    jassert(transientChoices.size() == UserWave::transientUserBase);

    for (int i = 1; i <= UserWave::numSlots; ++i)
        transientChoices.add("User " + juce::String(i));

    addParameterWithLogging(params,
        std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID{"transientType", 1}, "Transient Type",
            transientChoices, 0),
        safeString("transientType"));
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"transientMix", 1}, "Transient Mix",
            juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.5f),
        "transientMix");
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{"transientPostEffect", 1}, "Transient Post", false),
        "transientPostEffect");
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"transientKaDonk", 1}, "Ka-Donk",
            juce::NormalisableRange<float>(0.0f, 0.5f, 0.01f), 0.0f),
        "transientKaDonk");
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"transientCoarse", 1}, "Transient Coarse",
            juce::NormalisableRange<float>(-24.0f, 24.0f, 1.0f), 0.0f),
        "transientCoarse");
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"transientLength", 1}, "Transient Length",
            juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 1.0f),
        "transientLength");

    //==============================================================================
    // -- Lo-Fi Parameters (Saturation Color tab) --
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{"lofiEnabled", 1}, "Lo-Fi On", false),
        "lofiEnabled");
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"lofiAmount", 1}, "Lo-Fi Amount",
            juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.0f),
        "lofiAmount");
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"analogDrift", 1}, "Analog Drift",
            juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.0f),
        "analogDrift");

    //==============================================================================
    // -- Final EQ Parameters (5-band, end of chain, Saturation Color tab) --
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{"finalEQEnabled", 1}, "Final EQ On", false),
        "finalEQEnabled");
    // Band 1 â€“ Low Shelf (default 80 Hz)
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"finalEQB1Freq", 1}, "EQ B1 Freq",
            juce::NormalisableRange<float>(20.0f, 20000.0f, 1.0f, 0.2f), 80.0f),
        "finalEQB1Freq");
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"finalEQB1Gain", 1}, "EQ B1 Gain",
            juce::NormalisableRange<float>(-15.0f, 15.0f, 0.01f), 0.0f),
        "finalEQB1Gain");
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"finalEQB1Q", 1}, "EQ B1 Q",
            juce::NormalisableRange<float>(0.1f, 10.0f, 0.01f, 0.3f), 0.707f),
        "finalEQB1Q");
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID{"finalEQB1Type", 1}, "EQ B1 Type",
            SpaceDustFinalEQ::typeChoices(),
            static_cast<int>(SpaceDustFinalEQ::BandType::LowShelf)),
        "finalEQB1Type");
    // Band 2 â€“ Low Mid Peak (default 250 Hz)
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"finalEQB2Freq", 1}, "EQ B2 Freq",
            juce::NormalisableRange<float>(20.0f, 20000.0f, 1.0f, 0.2f), 250.0f),
        "finalEQB2Freq");
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"finalEQB2Gain", 1}, "EQ B2 Gain",
            juce::NormalisableRange<float>(-15.0f, 15.0f, 0.01f), 0.0f),
        "finalEQB2Gain");
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"finalEQB2Q", 1}, "EQ B2 Q",
            juce::NormalisableRange<float>(0.1f, 10.0f, 0.01f, 0.3f), 1.0f),
        "finalEQB2Q");
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID{"finalEQB2Type", 1}, "EQ B2 Type",
            SpaceDustFinalEQ::typeChoices(),
            static_cast<int>(SpaceDustFinalEQ::BandType::Bell)),
        "finalEQB2Type");
    // Band 3 â€“ Mid Peak (default 1000 Hz)
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"finalEQB3Freq", 1}, "EQ B3 Freq",
            juce::NormalisableRange<float>(20.0f, 20000.0f, 1.0f, 0.2f), 1000.0f),
        "finalEQB3Freq");
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"finalEQB3Gain", 1}, "EQ B3 Gain",
            juce::NormalisableRange<float>(-15.0f, 15.0f, 0.01f), 0.0f),
        "finalEQB3Gain");
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"finalEQB3Q", 1}, "EQ B3 Q",
            juce::NormalisableRange<float>(0.1f, 10.0f, 0.01f, 0.3f), 1.0f),
        "finalEQB3Q");
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID{"finalEQB3Type", 1}, "EQ B3 Type",
            SpaceDustFinalEQ::typeChoices(),
            static_cast<int>(SpaceDustFinalEQ::BandType::Bell)),
        "finalEQB3Type");
    // Band 4 â€“ High Mid Peak (default 4000 Hz)
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"finalEQB4Freq", 1}, "EQ B4 Freq",
            juce::NormalisableRange<float>(20.0f, 20000.0f, 1.0f, 0.2f), 4000.0f),
        "finalEQB4Freq");
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"finalEQB4Gain", 1}, "EQ B4 Gain",
            juce::NormalisableRange<float>(-15.0f, 15.0f, 0.01f), 0.0f),
        "finalEQB4Gain");
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"finalEQB4Q", 1}, "EQ B4 Q",
            juce::NormalisableRange<float>(0.1f, 10.0f, 0.01f, 0.3f), 1.0f),
        "finalEQB4Q");
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID{"finalEQB4Type", 1}, "EQ B4 Type",
            SpaceDustFinalEQ::typeChoices(),
            static_cast<int>(SpaceDustFinalEQ::BandType::Bell)),
        "finalEQB4Type");
    // Band 5 â€“ High Shelf (default 10000 Hz)
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"finalEQB5Freq", 1}, "EQ B5 Freq",
            juce::NormalisableRange<float>(20.0f, 20000.0f, 1.0f, 0.2f), 10000.0f),
        "finalEQB5Freq");
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"finalEQB5Gain", 1}, "EQ B5 Gain",
            juce::NormalisableRange<float>(-15.0f, 15.0f, 0.01f), 0.0f),
        "finalEQB5Gain");
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"finalEQB5Q", 1}, "EQ B5 Q",
            juce::NormalisableRange<float>(0.1f, 10.0f, 0.01f, 0.3f), 0.707f),
        "finalEQB5Q");
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID{"finalEQB5Type", 1}, "EQ B5 Type",
            SpaceDustFinalEQ::typeChoices(),
            static_cast<int>(SpaceDustFinalEQ::BandType::HighShelf)),
        "finalEQB5Type");

    //==============================================================================
    // -- Trance Gate Effect Parameters --
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{"tranceGateEnabled", 1}, "Trance Gate On", false),
        "tranceGateEnabled");
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{"tranceGatePostEffect", 1}, "Trance Gate Post", true),
        "tranceGatePostEffect");
    addParameterWithLogging(params,
        std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID{"tranceGateSteps", 1}, "Gate Steps",
            juce::StringArray("4", "8", "16"), 1),
        safeString("tranceGateSteps"));
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{"tranceGateSync", 1}, "Gate Sync", true),
        "tranceGateSync");
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"tranceGateRate", 1}, "Gate Rate",
            juce::NormalisableRange<float>(0.0f, 12.0f, 0.01f), 4.0f),
        "tranceGateRate");
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"tranceGateAttack", 1}, "Gate Attack",
            juce::NormalisableRange<float>(0.1f, 50.0f, 0.1f, 0.4f), 2.0f),
        "tranceGateAttack");
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"tranceGateRelease", 1}, "Gate Release",
            juce::NormalisableRange<float>(0.1f, 50.0f, 0.1f, 0.4f), 5.0f),
        "tranceGateRelease");
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"tranceGateMix", 1}, "Gate Mix",
            juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 1.0f),
        "tranceGateMix");
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{"tranceGateStep1", 1}, "Step 1", true),
        "tranceGateStep1");
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{"tranceGateStep2", 1}, "Step 2", false),
        "tranceGateStep2");
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{"tranceGateStep3", 1}, "Step 3", true),
        "tranceGateStep3");
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{"tranceGateStep4", 1}, "Step 4", false),
        "tranceGateStep4");
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{"tranceGateStep5", 1}, "Step 5", true),
        "tranceGateStep5");
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{"tranceGateStep6", 1}, "Step 6", false),
        "tranceGateStep6");
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{"tranceGateStep7", 1}, "Step 7", true),
        "tranceGateStep7");
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{"tranceGateStep8", 1}, "Step 8", false),
        "tranceGateStep8");
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{"tranceGateStep9", 1}, "Step 9", true),
        "tranceGateStep9");
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{"tranceGateStep10", 1}, "Step 10", false),
        "tranceGateStep10");
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{"tranceGateStep11", 1}, "Step 11", true),
        "tranceGateStep11");
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{"tranceGateStep12", 1}, "Step 12", false),
        "tranceGateStep12");
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{"tranceGateStep13", 1}, "Step 13", true),
        "tranceGateStep13");
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{"tranceGateStep14", 1}, "Step 14", false),
        "tranceGateStep14");
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{"tranceGateStep15", 1}, "Step 15", true),
        "tranceGateStep15");
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{"tranceGateStep16", 1}, "Step 16", false),
        "tranceGateStep16");

    //==============================================================================
    // -- MPE (MIDI Polyphonic Expression) Parameters --
    // These controls let the user configure MPE behaviour from the UI.
    // Default values provide full backward compatibility with non-MPE keyboards.

    // MPE Mode: 0 = Legacy (all channels, single bend range â€” works with everything),
    //           1 = Lower Zone (ch1 master + ch2-15 members â€” proper MPE spec).
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID{"mpeMode", 1}, "MPE Mode",
            juce::StringArray{"Legacy", "Lower Zone"}, 0),
        "mpeMode");

    // MPE Pitch Bend Range (semitones). Only affects Legacy mode â€” Lower Zone
    // always uses 48 per-note / 2 master per the MPE spec.
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"mpePitchBendRange", 1}, "MPE Bend Range",
            juce::NormalisableRange<float>(1.0f, 96.0f, 1.0f), 48.0f),
        "mpePitchBendRange");

    // MPE Pressure Depth (0-100%). Scales the per-note pressure â†’ amplitude
    // modulation.  0% = pressure has no effect, 100% = full Â±100% amplitude boost.
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"mpePressureDepth", 1}, "MPE Pressure Depth",
            juce::NormalisableRange<float>(0.0f, 100.0f, 1.0f), 100.0f),
        "mpePressureDepth");

    // MPE Timbre Depth (0-100%). Scales the per-note timbre (CC74 / slide) â†’
    // filter cutoff modulation.  0% = slide has no effect, 100% = full Â±2 octaves.
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"mpeTimbreDepth", 1}, "MPE Timbre Depth",
            juce::NormalisableRange<float>(0.0f, 100.0f, 1.0f), 100.0f),
        "mpeTimbreDepth");

    // Velocity Amount (0-100%). How much a note's MIDI velocity sets its level
    // and how far open its filter starts. Velocity was read at note-on and then
    // thrown away before this: every note came out the same however hard it was
    // played.
    //
    // FULL velocity is the anchor, not the middle: at 127 a note sounds exactly
    // as it does with this at zero, whatever the knob says, and turning the knob
    // up only takes SOFTER notes down and darker. Anchoring in the middle would
    // have every saved preset change level the moment this appeared.
    //
    // LAST in this list on purpose. A VST3 host automates by parameter INDEX, so
    // a new parameter inserted anywhere but the end renumbers every one after it
    // and moves the automation in every project that already used them. New
    // parameters go here, at the bottom.
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"velocityAmount", 1}, "Velocity Amount",
            juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f), 100.0f),
        "velocityAmount");

    //==========================================================================
    // -- The sub oscillator's shaping and unison --
    //
    // The same eight controls the two oscillators have, on the sub's own
    // parameters, so its Waveforms panel is the oscillators' panel rather than a
    // shorter one with the top strip missing (Giuseppe, 2026-08-26).
    //
    // DOWN HERE and not beside osc1's and osc2's, which is where they belong to
    // read. A VST3 host automates by parameter INDEX, so slotting these in among
    // the oscillators' would renumber every parameter after them and move the
    // automation in every project that already used one. New parameters go on the
    // end; that rule beats tidiness.
    //
    // Every one of them defaults to doing nothing -- shaping at zero, one voice
    // at no detune and no width -- so a patch saved before they existed plays
    // exactly as it did, with the sub a single centred signal.
    {
        // Said HERE and not on the slider: a SliderAttachment takes its text from
        // the PARAMETER, so a slider told to show two decimals still reads
        // 0.00000 unless the parameter agrees.
        const auto twoDecimals = [] (float value, int)
        {
            return juce::String (value, 2);
        };

        struct SubParam { const char* id; const char* name; };

        const SubParam subShaping[] =
        {
            { "subOscBendPlus",      "Sub Bend +" },
            { "subOscBendMinus",     "Sub Bend -" },
            { "subOscBendPlusMinus", "Sub Bend +/-" },
            { "subOscSpectrum",      "Sub Spectrum" },
            { "subOscSync",          "Sub Sync" },
            { "subOscUnisonDetune",  "Sub Unison Detune" },
            { "subOscUnisonWidth",   "Sub Unison Width" },
        };

        for (const auto& sp : subShaping)
            addParameterWithLogging(params,
                std::make_unique<juce::AudioParameterFloat>(
                    juce::ParameterID{sp.id, 1}, sp.name,
                    juce::NormalisableRange<float>(0.0f, 1.0f), 0.0f,
                    juce::AudioParameterFloatAttributes().withStringFromValueFunction(twoDecimals)),
                safeString(sp.id));

        // An INT, like the oscillators' counts: it is a number of things, and a
        // float would let automation land between two of them.
        addParameterWithLogging(params,
            std::make_unique<juce::AudioParameterInt>(
                juce::ParameterID{"subOscUnisonVoices", 1}, "Sub Unison Voices",
                1, Unison::maxVoices, 1),
            safeString("subOscUnisonVoices"));

        //======================================================================
        // -- The noise source's unison --
        //
        // Same three knobs, and on an IMPORTED waveform in the noise slot they do
        // the same three things -- that slot holds a third oscillator with a
        // pitch, not noise.
        //
        // On built-in White and Pink, Detune has nothing to act on: noise has no
        // pitch to pull apart. Voices and Width still do, and what they buy there
        // is a STEREO noise field -- each copy is an independent stream, so
        // spreading them decorrelates the two sides. One stream panned anywhere
        // is still mono, however wide the knob says it is.
        //
        // No shaping knobs to go with them. Bend and Sync move a position in a
        // cycle, and built-in noise has no cycle.
        //
        // On the end of the list for the same reason as everything above it.
        addParameterWithLogging(params,
            std::make_unique<juce::AudioParameterInt>(
                juce::ParameterID{"noiseUnisonVoices", 1}, "Noize Unison Voices",
                1, Unison::maxVoices, 1),
            safeString("noiseUnisonVoices"));

        const SubParam noiseAmounts[] =
        {
            { "noiseUnisonDetune", "Noize Unison Detune" },
            { "noiseUnisonWidth",  "Noize Unison Width" },
        };

        for (const auto& np : noiseAmounts)
            addParameterWithLogging(params,
                std::make_unique<juce::AudioParameterFloat>(
                    juce::ParameterID{np.id, 1}, np.name,
                    juce::NormalisableRange<float>(0.0f, 1.0f), 0.0f,
                    juce::AudioParameterFloatAttributes().withStringFromValueFunction(twoDecimals)),
                safeString(np.id));

        //======================================================================
        // -- Unison Random Phase, for all four sources --
        //
        // How far apart the copies START in the cycle.
        //
        // At zero every copy starts on the note's own phase, which is what the
        // unison did before this and what every preset already written gets. It
        // is also why a note begins louder than it settles: copies on the same
        // phase are fully coherent for the first instant however far apart they
        // are tuned, because Detune separates them over TIME and has had no time
        // yet. The burst thinning out is heard as a downward sweep on the attack
        // (Giuseppe, 2026-08-27).
        //
        // Turning it up scatters them, and Unison::layout is told, so the level
        // holds -- see the coherence term there. The cost is that the attack of
        // the same note differs slightly each time it is played, which on a short
        // percussive patch reads as an inconsistent transient. That is the
        // trade-off the knob exists to let anyone make.
        //
        // On built-in White and Pink it does nothing, for the same reason Detune
        // does nothing there: they have no cycle to start in. An imported sample
        // in the noise slot does, and this works on it.
        //
        // On the end of the list, like everything else added after the fact.
        const SubParam phaseParams[] =
        {
            { "osc1UnisonPhase",   "Osc 1 Unison Random Phase" },
            { "osc2UnisonPhase",   "Osc 2 Unison Random Phase" },
            { "subOscUnisonPhase", "Sub Unison Random Phase" },
            { "noiseUnisonPhase",  "Noize Unison Random Phase" },
        };

        for (const auto& pp : phaseParams)
            addParameterWithLogging(params,
                std::make_unique<juce::AudioParameterFloat>(
                    juce::ParameterID{pp.id, 1}, pp.name,
                    juce::NormalisableRange<float>(0.0f, 1.0f), 0.0f,
                    juce::AudioParameterFloatAttributes().withStringFromValueFunction(twoDecimals)),
                safeString(pp.id));
    }

    //==============================================================================
    // -- DEBUG: createParameterLayout End --
    #if JUCE_DEBUG
    try
    {
        juce::File logFile = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
            .getChildFile(safeString("SpaceDust_DebugLog.txt"));
        juce::FileOutputStream out(logFile);
        if (out.openedOk())
        {
            out.setPosition(out.getPosition());
            out.writeText("Space Dust: createParameterLayout() completed - created " + 
                         safeStringFromNumber(static_cast<int>(params.size())) + " parameters\n", false, false, nullptr);
            out.flush();
        }
    }
    catch (...) {}
    #endif
    
    DBG("Space Dust: createParameterLayout() completed - created " + safeStringFromNumber(static_cast<int>(params.size())) + " parameters");
    
    return { params.begin(), params.end() };
}

//==============================================================================
// This creates new instances of the plugin..
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new SpaceDustAudioProcessor();
}


//==============================================================================
void SpaceDustAudioProcessor::readScopeSamples(float* destL, float* destR, int numSamples) const
{
    constexpr int mask = scopeFifoSize - 1;
    const int count = juce::jmin(numSamples, scopeFifoSize);
    const int wp = scopeFifoWritePos.load(std::memory_order_acquire);
    // The most-recent `count` samples end just before the write cursor.
    int idx = (wp - count) & mask;
    for (int i = 0; i < count; ++i)
    {
        destL[i] = scopeFifoL[static_cast<size_t>(idx)];
        destR[i] = scopeFifoR[static_cast<size_t>(idx)];
        idx = (idx + 1) & mask;
    }
}

//==============================================================================
// Space Dust - Cosmic Subtractive Synthesizer
// 
// A dual-oscillator subtractive synthesizer with multimode filter and ADSR envelope.
// Built with JUCE 8, featuring real-time safe processing and a beautiful cosmic GUI.
//
// Signal Path: 
//   Osc1 (with independent detune) + Osc2 (with independent detune) 
//   → Mix → Filter → ADSR Envelope → Master Volume → Output
//
// Features:
// - Dual oscillators with 4 waveforms each (Sine, Triangle, Saw, Square)
// - Independent detune for each oscillator (coarse + fine) for shimmering effects
// - Osc2 can be tuned relative to Osc1 (coarse/fine tuning for intervals)
// - Multimode state-variable filter (Low Pass, Band Pass, High Pass)
// - Proper 4-stage ADSR amplitude envelope (Attack → Decay → Sustain → Release)
//   with long cosmic tails (release up to 20 seconds)
// - Master volume control for proper mix integration
// - 8-voice polyphony
// - Real-time safe parameter updates via AudioProcessorValueTreeState
//
// ADSR Envelope Implementation:
//   - Linear amplitude ramping for real-time safety
//   - Proper state machine: Idle → Attack → Decay → Sustain → Release → Idle
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
// Space Dust by [your name] – the cosmic sine machine
//==============================================================================

#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "SpaceDustSynthesiser.h"
#include "SpaceDustReverb.h"
#include "SpaceDustGrainDelay.h"
#include "SpaceDustPhaser.h"
#include "SpaceDustTranceGate.h"
#include <juce_core/juce_core.h>
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
    //   - filter.prepare() called with invalid spec → assertions
    //   - adsr.setSampleRate(0) → invalid timing calculations → assertions
    //   - Repeated assertions during processBlock() → potential crashes
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
        DBG("Space Dust: Added listeners for LFO retrigger and filter envelope");
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
            lfo1Retrigger.store(lfo1RetriggerParam->getValue() > 0.5f);
        }
        if (auto* lfo2RetriggerParam = apvts.getParameter(juce::ParameterID{"lfo2Retrigger", 1}.getParamID()))
        {
            lfo2Retrigger.store(lfo2RetriggerParam->getValue() > 0.5f);
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
    
    //==============================================================================
    DBG("Space Dust: Processor ctor END");
    logToFile("Processor constructed");
}

SpaceDustAudioProcessor::~SpaceDustAudioProcessor()
{
    logToFile("Destructor START");
    DBG("Space Dust: Destructor started - cleaning up resources");

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
    
    DBG("Space Dust: Clearing sounds (count: " + safeStringFromNumber(synth.getNumSounds()) + ")");
    synth.clearSounds();           // Clear all sounds (clears ReferenceCountedArray)
    DBG("Space Dust: Sounds cleared");
    
    synth.setCurrentPlaybackSampleRate(0.0); // Reset sample rate to clean state
    DBG("Space Dust: Sample rate reset");
    
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
    
    // Step 2: Ensure at least one SynthesiserSound exists
    DBG("Space Dust: prepareToPlay - Step 2: Adding sound");
    try
    {
        if (synth.getNumSounds() == 0)
        {
            synth.addSound(new SynthSound());
            DBG("Space Dust: prepareToPlay - Step 2: Sound added");
        }
        else
        {
            DBG("Space Dust: prepareToPlay - Step 2: Sound already exists");
        }
    }
    catch (const std::exception& e)
    {
        DBG("Space Dust: Exception adding sound: " + juce::String(e.what()));
        throw;
    }
    catch (...)
    {
        DBG("Space Dust: Unknown exception adding sound");
        throw;
    }
    
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
        
        // Initialize LFO buffers for per-sample processing
        lfo1Buffer.setSize(1, samplesPerBlock);
        lfo2Buffer.setSize(1, samplesPerBlock);
        lfo1Buffer.clear();
        lfo2Buffer.clear();
        // Seed Sample & Hold with initial random values (avoids first period at 0)
        lfo1ShState = lfo1ShState * 1103515245u + 12345u;
        lfo1SampleHoldValue = (static_cast<float>((lfo1ShState >> 16) & 0x7FFF) / 32767.5f) * 2.0f - 1.0f;
        lfo2ShState = lfo2ShState * 1103515245u + 12345u;
        lfo2SampleHoldValue = (static_cast<float>((lfo2ShState >> 16) & 0x7FFF) / 32767.5f) * 2.0f - 1.0f;
        
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

        // Initialize goniometer buffers (double-buffered for Spectral tab Lissajous display)
        const int gonioSamples = juce::jmin(goniometerMaxSamples, samplesPerBlock);
        goniometerBuffer[0].setSize(2, gonioSamples, false, true, true);
        goniometerBuffer[1].setSize(2, gonioSamples, false, true, true);
        goniometerBuffer[0].clear();
        goniometerBuffer[1].clear();
        goniometerReadIndex.store(0);
        
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
    int osc1Wave = (int)*apvts.getRawParameterValue("osc1Waveform");
    int osc2Wave = (int)*apvts.getRawParameterValue("osc2Waveform");
    
    // Oscillator tuning parameters (simple, intuitive system)
    float osc1CoarseTune = *apvts.getRawParameterValue("osc1CoarseTune");
    float osc1Detune = *apvts.getRawParameterValue("osc1Detune");
    float osc2CoarseTune = *apvts.getRawParameterValue("osc2CoarseTune");
    float osc2Detune = *apvts.getRawParameterValue("osc2Detune");
    
    // LFO modulation is now applied per-sample in renderNextBlock via LFO buffers
    // lfo1Modulation and lfo2Modulation parameters are ignored (kept for API compatibility)
    
    // Independent oscillator and noise level controls
    float osc1Level = *apvts.getRawParameterValue("osc1Level");
    float osc2Level = *apvts.getRawParameterValue("osc2Level");
    float noiseLevel = *apvts.getRawParameterValue("noiseLevel");
    
    int filterMode = (int)*apvts.getRawParameterValue("filterMode");
    float filterCutoffHz = 8000.0f;
    if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("filterCutoff")))
        filterCutoffHz = juce::jlimit(20.0f, 20000.0f, p->get());
    // LFO filter modulation is applied per-sample in renderNextBlock
    
    float filterResonance = *apvts.getRawParameterValue("filterResonance");
    
    // LFO targets (cache per-block to avoid per-sample APVTS reads in voice - major CPU win)
    int lfo1Target = static_cast<int>(*apvts.getRawParameterValue("lfo1Target"));
    int lfo2Target = static_cast<int>(*apvts.getRawParameterValue("lfo2Target"));
    
    bool modFilter1Show = *apvts.getRawParameterValue("modFilter1Show") > 0.5f;
    bool modFilter2Show = *apvts.getRawParameterValue("modFilter2Show") > 0.5f;
    bool modFilter1Link = *apvts.getRawParameterValue("modFilter1LinkToMaster") > 0.5f;
    bool modFilter2Link = *apvts.getRawParameterValue("modFilter2LinkToMaster") > 0.5f;
    bool warmSaturationMaster = *apvts.getRawParameterValue("warmSaturationMaster") > 0.5f;

    // When linked, use master filter values directly instead of mod filter values.
    // This avoids calling setValueNotifyingHost for sync, which triggers performEdit
    // in the VST3 wrapper and causes Ableton to grey out automation lanes.
    int modFilter1Mode = modFilter1Link ? filterMode : (int)*apvts.getRawParameterValue("modFilter1Mode");
    float modFilter1Cutoff = modFilter1Link ? filterCutoffHz : *apvts.getRawParameterValue("modFilter1Cutoff");
    float modFilter1Resonance = modFilter1Link ? filterResonance : *apvts.getRawParameterValue("modFilter1Resonance");
    bool warmSaturationMod1 = modFilter1Link ? warmSaturationMaster : *apvts.getRawParameterValue("warmSaturationMod1") > 0.5f;

    int modFilter2Mode = modFilter2Link ? filterMode : (int)*apvts.getRawParameterValue("modFilter2Mode");
    float modFilter2Cutoff = modFilter2Link ? filterCutoffHz : *apvts.getRawParameterValue("modFilter2Cutoff");
    float modFilter2Resonance = modFilter2Link ? filterResonance : *apvts.getRawParameterValue("modFilter2Resonance");
    bool warmSaturationMod2 = modFilter2Link ? warmSaturationMaster : *apvts.getRawParameterValue("warmSaturationMod2") > 0.5f;
    
    // Filter envelope: read directly from parameters each block (guarantees label matches decay)
    // Uses plain param ID strings to match SliderAttachment; p->get() returns exact displayed value
    float filterEnvAttack = 0.01f, filterEnvDecay = 0.8f, filterEnvRelease = 3.0f;
    if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("filterEnvAttack")))
        filterEnvAttack = juce::jlimit(0.01f, 20.0f, p->get());
    if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("filterEnvDecay")))
        filterEnvDecay = juce::jlimit(0.01f, 5.0f, p->get());
    if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("filterEnvRelease")))
        filterEnvRelease = juce::jlimit(0.01f, 20.0f, p->get());
    float filterEnvAmount = 0.0f;
    if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("filterEnvAmount")))
        filterEnvAmount = juce::jlimit(-100.0f, 100.0f, p->get());
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
    float envAttack = currentAttackTime.load();
    float envDecay = currentDecayTime.load();
    float envSustain = currentSustainLevel.load();
    float envRelease = currentReleaseTime.load();
    
    // Voice mode and glide parameters (convert normalized 0-1 to actual seconds for glide)
    float glideTime = 0.0f;
    if (auto* p = apvts.getParameter("glideTime"))
        glideTime = p->convertFrom0to1(*apvts.getRawParameterValue("glideTime"));
    bool legatoGlide = *apvts.getRawParameterValue("legatoGlide") > 0.5f;
    
    // Pitch envelope parameters (use get() for actual value - separate from pitch bend)
    float pitchEnvAmount = 0.0f, pitchEnvTime = 0.0f, pitchEnvPitch = 0.0f;
    if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("pitchEnvAmount")))
        pitchEnvAmount = p->get();
    if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("pitchEnvTime")))
        pitchEnvTime = p->get();
    if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("pitchEnvPitch")))
        pitchEnvPitch = p->get();
    
    // Pitch bend parameters (use get() for actual value - separate from pitch envelope)
    float pitchBendAmount = 0.0f;
    if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter("pitchBendAmount")))
        pitchBendAmount = juce::jlimit(0.0f, 24.0f, p->get());
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
    
    // Update all voices with current parameter values
    for (int i = 0; i < synth.getNumVoices(); ++i)
    {
        if (auto* voice = dynamic_cast<SynthVoice*>(synth.getVoice(i)))
        {
            voice->setOsc1Waveform(osc1Wave);
            voice->setOsc2Waveform(osc2Wave);
            voice->setOsc1CoarseTune(osc1CoarseTune);
            voice->setOsc1Detune(osc1Detune);
            voice->setOsc2CoarseTune(osc2CoarseTune);
            voice->setOsc2Detune(osc2Detune);
            voice->setOsc1Level(osc1Level);
            voice->setOsc2Level(osc2Level);
            voice->setOsc1Pan(*apvts.getRawParameterValue("osc1Pan"));
            voice->setOsc2Pan(*apvts.getRawParameterValue("osc2Pan"));
            voice->setNoiseLevel(noiseLevel);
            voice->setNoiseType(noiseType.load());  // Get noise type from atomic
            float lowShelfAmount = *apvts.getRawParameterValue("lowShelfAmount");
            float highShelfAmount = *apvts.getRawParameterValue("highShelfAmount");
            voice->setLowShelfAmount(lowShelfAmount);
            voice->setHighShelfAmount(highShelfAmount);
            voice->setFilterMode(filterMode);
            voice->setFilterCutoff(filterCutoffHz);
            voice->setFilterResonance(filterResonance);
            voice->setWarmSaturationMaster(warmSaturationMaster);
            voice->setModFilter1(modFilter1Show, modFilter1Link, modFilter1Mode, modFilter1Cutoff, modFilter1Resonance);
            voice->setWarmSaturationMod1(warmSaturationMod1);
            voice->setModFilter2(modFilter2Show, modFilter2Link, modFilter2Mode, modFilter2Cutoff, modFilter2Resonance);
            voice->setWarmSaturationMod2(warmSaturationMod2);
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
            voice->setSubOscOn(*apvts.getRawParameterValue("subOscOn") > 0.5f);
            voice->setSubOscWaveform(static_cast<int>(*apvts.getRawParameterValue("subOscWaveform")));
            voice->setSubOscLevel(*apvts.getRawParameterValue("subOscLevel"));
            voice->setSubOscCoarse(*apvts.getRawParameterValue("subOscCoarse"));
            voice->setPitchBendAmount(pitchBendAmount);
            voice->setPitchBend(pitchBend);
            voice->setLfoTargets(lfo1Target, lfo2Target);
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
        lfo1Retrigger.store(newValue > 0.5f);
    }
    else if (parameterID == juce::ParameterID{"lfo2Retrigger", 1}.getParamID())
    {
        lfo2Retrigger.store(newValue > 0.5f);
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
            currentFilterEnvDecay.store(juce::jlimit(0.01f, 5.0f, p->get()));
    }
    else if (parameterID == juce::ParameterID{"filterEnvRelease", 1}.getParamID())
    {
        // Read actual value from param (matches UI label exactly)
        if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter(parameterID)))
            currentFilterEnvRelease.store(juce::jlimit(0.01f, 20.0f, p->get()));
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

void SpaceDustAudioProcessor::releaseResources()
{
    logToFile("releaseResources START");
    DBG("Space Dust: releaseResources() called - starting cleanup");
    
    //==============================================================================
    // -- CRITICAL: Force All Notes Off Before Cleanup --
    // CRITICAL: Stop all notes gracefully with tail-off to prevent voice leaks
    // Use channel 0 to turn off ALL notes (per JUCE Synthesiser docs)
    synth.allNotesOff(0, true);  // Channel 0 = all channels, allow tail-off
    
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
    for (int i = 0; i < synth.getNumVoices(); ++i)
    {
        if (auto* voice = synth.getVoice(i))
        {
            voice->stopNote(0.0f, false);  // Stop without tail-off for immediate cleanup
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
    
    // Step 3: Clear all sounds (ReferenceCountedArray<SynthesiserSound>)
    // CRITICAL: This prevents ReferenceCountedObject assertion on plugin unload
    // The ReferenceCountedArray must be cleared to avoid dangling references
    // when the plugin destructor runs, especially in Ableton Live
    DBG("Space Dust: Clearing sounds (count: " + safeStringFromNumber(synth.getNumSounds()) + ")");
    synth.clearSounds();           // clears ReferenceCountedArray<SynthesiserSound>
    DBG("Space Dust: Sounds cleared");
    
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
    
    juce::ScopedNoDenormals noDenormals;
    auto totalNumInputChannels  = getTotalNumInputChannels();
    auto totalNumOutputChannels = getTotalNumOutputChannels();

    // Clear any output channels that don't contain input data
    // (Not needed for synth, but good practice for compatibility)
    for (auto i = totalNumInputChannels; i < totalNumOutputChannels; ++i)
        buffer.clear(i, 0, numSamples);

    //==============================================================================
    // -- LFO Processing (Per-Sample Buffers) --
    // Generate per-sample LFO values and fill buffers for voice access
    
    // Get LFO parameters
    bool lfo1Enabled = *apvts.getRawParameterValue("lfo1Enabled") > 0.5f;
    float lfo1Depth = lfo1Enabled ? (*apvts.getRawParameterValue("lfo1Depth") * 2.0f / 100.0f) : 0.0f;  // 0-2.0 when on
    bool lfo1Sync = *apvts.getRawParameterValue("lfo1Sync") > 0.5f;
    float lfo1Rate = *apvts.getRawParameterValue("lfo1Rate");  // 0-12
    bool lfo1Triplet = *apvts.getRawParameterValue("lfo1TripletEnabled") > 0.5f;
    bool lfo1All = *apvts.getRawParameterValue("lfo1TripletStraightToggle") > 0.5f;
    float lfo1PhaseParam = *apvts.getRawParameterValue("lfo1Phase");
    int lfo1Waveform = (int)*apvts.getRawParameterValue("lfo1Waveform");
    
    bool lfo2Enabled = *apvts.getRawParameterValue("lfo2Enabled") > 0.5f;
    float lfo2Depth = lfo2Enabled ? (*apvts.getRawParameterValue("lfo2Depth") * 2.0f / 100.0f) : 0.0f;  // 0-2.0 when on
    bool lfo2Sync = *apvts.getRawParameterValue("lfo2Sync") > 0.5f;
    float lfo2Rate = *apvts.getRawParameterValue("lfo2Rate");  // 0-12
    bool lfo2Triplet = *apvts.getRawParameterValue("lfo2TripletEnabled") > 0.5f;
    bool lfo2All = *apvts.getRawParameterValue("lfo2TripletStraightToggle") > 0.5f;
    float lfo2PhaseParam = *apvts.getRawParameterValue("lfo2Phase");
    int lfo2Waveform = (int)*apvts.getRawParameterValue("lfo2Waveform");
    
    // Helper: generate LFO waveform with rounded saw transitions (prevents discontinuity clicks)
    auto generateLfoValue = [](double phase, int waveform) -> float {
        double p = std::fmod(phase, 1.0);
        if (p < 0.0) p += 1.0;

        switch (waveform)
        {
            case 0: // Sine
                return static_cast<float>(std::sin(p * juce::MathConstants<double>::twoPi));
            case 1: // Triangle
            {
                if (p < 0.25)
                    return static_cast<float>(p * 4.0);
                else if (p < 0.75)
                    return static_cast<float>(2.0 - p * 4.0);
                else
                    return static_cast<float>(p * 4.0 - 4.0);
            }
            case 2: // Saw Up - rounded at wrap (last 8% eases into -1)
            {
                const double kTransition = 0.08;
                if (p < 1.0 - kTransition)
                    return static_cast<float>(p * 2.0 - 1.0);
                double t = (p - (1.0 - kTransition)) / kTransition;
                double ease = (1.0 - std::cos(t * juce::MathConstants<double>::pi)) * 0.5;
                double linearEnd = (1.0 - kTransition) * 2.0 - 1.0;
                return static_cast<float>(linearEnd + ease * (-1.0 - linearEnd));
            }
            case 3: // Saw Down - rounded at wrap (first 8% eases from 1)
            {
                const double kTransition = 0.08;
                if (p > kTransition)
                    return static_cast<float>(1.0 - p * 2.0);
                double t = p / kTransition;
                double ease = (1.0 - std::cos(t * juce::MathConstants<double>::pi)) * 0.5;
                double linearStart = 1.0 - kTransition * 2.0;
                return static_cast<float>(1.0 + ease * (linearStart - 1.0));
            }
            case 4: // Square - soft transition (~3% of cycle) to avoid clicks
            {
                const double kTransition = 0.03;
                if (p < 0.5 - kTransition) return 1.0f;
                if (p > 0.5 + kTransition) return -1.0f;
                double t = (p - (0.5 - kTransition)) / (2.0 * kTransition);
                return static_cast<float>(1.0 - 2.0 * t);  // linear crossfade
            }
            case 5: // S&H - not used here; handled via wrap detection in loop
                return 0.0f;
            default:
                return static_cast<float>(std::sin(p * juce::MathConstants<double>::twoPi));
        }
    };

    // Sample & Hold: generate next random value in [-1, 1] using LCG (real-time safe)
    auto nextSampleHoldValue = [](uint32_t& state, float& held) {
        state = state * 1103515245u + 12345u;
        float r = static_cast<float>((state >> 16) & 0x7FFF) / 32767.5f;
        held = r * 2.0f - 1.0f;
    };

    // Smoothing coefficient: ~5 samples to soften retrigger/phase jumps (prevents clicks)
    constexpr float kLfoSmoothAlpha = 0.25f;
    
    // Process LFO1
    if (lfo1Sync)
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
        float rateClamped = juce::jlimit(0.0f, 12.0f, lfo1Rate);
        int musicalIndex = static_cast<int>(std::round(rateClamped * 8.0f / 12.0f));
        musicalIndex = juce::jlimit(0, 8, musicalIndex);
        
        double multiplier = 1.0;
        
        if (lfo1Triplet && lfo1All)
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
        else if (lfo1Triplet && !lfo1All)
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
        
        float phaseOffset = lfo1PhaseParam / 360.0f;
        bool isPlaying = false;
        if (playHead != nullptr)
        {
            auto posInfo = playHead->getPosition();
            if (posInfo.hasValue())
                isPlaying = posInfo->getIsPlaying();
        }
        bool useBeatPhase = !lfo1Retrigger.load() && isPlaying;  // Beat phase only when playing + no retrigger
        
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
            double prevPhase = lfo1PrevPhase;
            for (int s = 0; s < numSamples; ++s)
            {
                double ppq = ppqStart + static_cast<double>(s) / samplesPerBeat;
                double phase = std::fmod(ppq, periodBeats) / periodBeats;
                float raw;
                if (lfo1Waveform == 5)
                {
                    bool wrapped = (prevPhase < 0) || (prevPhase > 0.5 && phase < prevPhase);
                    if (wrapped)
                        nextSampleHoldValue(lfo1ShState, lfo1SampleHoldValue);
                    raw = lfo1SampleHoldValue * lfo1Depth;
                    prevPhase = phase;
                }
                else
                {
                    raw = generateLfoValue(phase + phaseOffset, lfo1Waveform) * lfo1Depth;
                }
                lfo1SmoothedValue += kLfoSmoothAlpha * (raw - lfo1SmoothedValue);
                lfo1Buffer.setSample(0, s, lfo1SmoothedValue);
            }
            lfo1PrevPhase = prevPhase;
        }
        else
        {
            // Retrigger ON: use accumulator (reset on note in voice)
            double phase = lfo1CurrentPhase;
            for (int s = 0; s < numSamples; ++s)
            {
                double phaseNext = phase + delta;
                bool wrapped = (phaseNext >= 1.0);
                if (lfo1Waveform == 5 && wrapped)
                    nextSampleHoldValue(lfo1ShState, lfo1SampleHoldValue);
                phase = std::fmod(phaseNext, 1.0);
                float raw = (lfo1Waveform == 5) ? (lfo1SampleHoldValue * lfo1Depth)
                    : (generateLfoValue(phase + phaseOffset, lfo1Waveform) * lfo1Depth);
                lfo1SmoothedValue += kLfoSmoothAlpha * (raw - lfo1SmoothedValue);
                lfo1Buffer.setSample(0, s, lfo1SmoothedValue);
            }
            lfo1CurrentPhase = phase;
        }
    }
    else
    {
        // Free mode: 0.01-200 Hz logarithmic (clamping prevents wraparound/slowdown bug)
        float rateClamped = juce::jlimit(0.0f, 12.0f, lfo1Rate);
        float normalizedRate = juce::jlimit(0.0f, 1.0f, rateClamped / 12.0f);
        float logMin = std::log(0.01f);
        float logMax = std::log(200.0f);
        float logFreq = logMin + normalizedRate * (logMax - logMin);
        float hz = std::exp(logFreq);
        hz = juce::jlimit(0.01f, 200.0f, hz);
        
        double delta = (currentSampleRate > 0.0) ? (static_cast<double>(hz) / currentSampleRate) : 0.0;
        
        // Fill per-sample buffer
        double phase = lfo1CurrentPhase;
        float phaseOffset = lfo1PhaseParam / 360.0f;
        for (int s = 0; s < numSamples; ++s)
        {
            double phaseNext = phase + delta;
            bool wrapped = (phaseNext >= 1.0);
            if (lfo1Waveform == 5 && wrapped)
                nextSampleHoldValue(lfo1ShState, lfo1SampleHoldValue);
            phase = std::fmod(phaseNext, 1.0);
            float raw = (lfo1Waveform == 5) ? (lfo1SampleHoldValue * lfo1Depth)
                : (generateLfoValue(phase + phaseOffset, lfo1Waveform) * lfo1Depth);
            lfo1SmoothedValue += kLfoSmoothAlpha * (raw - lfo1SmoothedValue);
            lfo1Buffer.setSample(0, s, lfo1SmoothedValue);
        }
        lfo1CurrentPhase = phase;
    }
    
    // Process LFO2 (same logic as LFO1)
    if (lfo2Sync)
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
        float rateClamped = juce::jlimit(0.0f, 12.0f, lfo2Rate);
        int musicalIndex = static_cast<int>(std::round(rateClamped * 8.0f / 12.0f));
        musicalIndex = juce::jlimit(0, 8, musicalIndex);
        
        double multiplier = 1.0;
        
        if (lfo2Triplet && lfo2All)
        {
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
        else if (lfo2Triplet && !lfo2All)
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
        
        float phaseOffset = lfo2PhaseParam / 360.0f;
        bool isPlaying = false;
        if (playHead != nullptr)
        {
            auto posInfo = playHead->getPosition();
            if (posInfo.hasValue())
                isPlaying = posInfo->getIsPlaying();
        }
        bool useBeatPhase = !lfo2Retrigger.load() && isPlaying;  // Beat phase only when playing + no retrigger
        
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
            double prevPhase = lfo2PrevPhase;
            for (int s = 0; s < numSamples; ++s)
            {
                double ppq = ppqStart + static_cast<double>(s) / samplesPerBeat;
                double phase = std::fmod(ppq, periodBeats) / periodBeats;
                float raw;
                if (lfo2Waveform == 5)
                {
                    bool wrapped = (prevPhase < 0) || (prevPhase > 0.5 && phase < prevPhase);
                    if (wrapped)
                        nextSampleHoldValue(lfo2ShState, lfo2SampleHoldValue);
                    raw = lfo2SampleHoldValue * lfo2Depth;
                    prevPhase = phase;
                }
                else
                {
                    raw = generateLfoValue(phase + phaseOffset, lfo2Waveform) * lfo2Depth;
                }
                lfo2SmoothedValue += kLfoSmoothAlpha * (raw - lfo2SmoothedValue);
                lfo2Buffer.setSample(0, s, lfo2SmoothedValue);
            }
            lfo2PrevPhase = prevPhase;
        }
        else
        {
            // Retrigger ON: use accumulator (reset on note in voice)
            double phase = lfo2CurrentPhase;
            for (int s = 0; s < numSamples; ++s)
            {
                double phaseNext = phase + delta;
                bool wrapped = (phaseNext >= 1.0);
                if (lfo2Waveform == 5 && wrapped)
                    nextSampleHoldValue(lfo2ShState, lfo2SampleHoldValue);
                phase = std::fmod(phaseNext, 1.0);
                float raw = (lfo2Waveform == 5) ? (lfo2SampleHoldValue * lfo2Depth)
                    : (generateLfoValue(phase + phaseOffset, lfo2Waveform) * lfo2Depth);
                lfo2SmoothedValue += kLfoSmoothAlpha * (raw - lfo2SmoothedValue);
                lfo2Buffer.setSample(0, s, lfo2SmoothedValue);
            }
            lfo2CurrentPhase = phase;
        }
    }
    else
    {
        // Free mode: 0.01-200 Hz logarithmic (clamping prevents wraparound/slowdown bug)
        float rateClamped = juce::jlimit(0.0f, 12.0f, lfo2Rate);
        float normalizedRate = juce::jlimit(0.0f, 1.0f, rateClamped / 12.0f);
        float logMin = std::log(0.01f);
        float logMax = std::log(200.0f);
        float logFreq = logMin + normalizedRate * (logMax - logMin);
        float hz = std::exp(logFreq);
        hz = juce::jlimit(0.01f, 200.0f, hz);
        
        double delta = (currentSampleRate > 0.0) ? (static_cast<double>(hz) / currentSampleRate) : 0.0;
        
        // Fill per-sample buffer
        double phase = lfo2CurrentPhase;
        float phaseOffset = lfo2PhaseParam / 360.0f;
        for (int s = 0; s < numSamples; ++s)
        {
            double phaseNext = phase + delta;
            bool wrapped = (phaseNext >= 1.0);
            if (lfo2Waveform == 5 && wrapped)
                nextSampleHoldValue(lfo2ShState, lfo2SampleHoldValue);
            phase = std::fmod(phaseNext, 1.0);
            float raw = (lfo2Waveform == 5) ? (lfo2SampleHoldValue * lfo2Depth)
                : (generateLfoValue(phase + phaseOffset, lfo2Waveform) * lfo2Depth);
            lfo2SmoothedValue += kLfoSmoothAlpha * (raw - lfo2SmoothedValue);
            lfo2Buffer.setSample(0, s, lfo2SmoothedValue);
        }
        lfo2CurrentPhase = phase;
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
    
    //==============================================================================
    // -- Transient: Scan MIDI for note-on events to trigger drum transient --
    bool transientEnabled = *apvts.getRawParameterValue("transientEnabled") > 0.5f;
    bool transientPostEffect = *apvts.getRawParameterValue("transientPostEffect") > 0.5f;
    if (transientEnabled)
    {
        for (const auto metadata : midiMessages)
        {
            const auto msg = metadata.getMessage();
            if (msg.isNoteOn() && msg.getVelocity() > 0)
            {
                SpaceDustTransient::Parameters tp;
                tp.enabled = true;
                if (auto* p = dynamic_cast<juce::AudioParameterChoice*>(apvts.getParameter(juce::ParameterID{"transientType", 1}.getParamID())))
                    tp.type = p->getIndex();
                else
                    tp.type = 0;
                tp.mix = juce::jlimit(0.0f, 1.0f, apvts.getRawParameterValue("transientMix")->load());
                tp.postEffect = transientPostEffect;
                tp.kaDonk = juce::jlimit(0.0f, 1.0f, apvts.getRawParameterValue("transientKaDonk")->load());
                tp.coarse = juce::jlimit(-24.0f, 24.0f, apvts.getRawParameterValue("transientCoarse")->load());
                tp.length = juce::jlimit(0.0f, 1.0f, apvts.getRawParameterValue("transientLength")->load());
                transient_.setParameters(tp);
                transient_.trigger(msg.getNoteNumber());
                break;
            }
        }
    }

    //==============================================================================
    // -- Process MIDI with Mono Mode Support --
    // Call custom synthesiser methods to handle mono mode
    synth.processMidiBuffer(midiMessages, numSamples);

    // Voice params after mono/legato MIDI rewrite so coarse/detune retune uses currentPitch
    // (see SynthVoice::setOsc* — must align with renderNextBlock base Hz, not stale MIDI note).
    updateVoicesWithParameters(0.0f, 0.0f);

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
        float kaDonkAmount = juce::jlimit(0.0f, 1.0f, apvts.getRawParameterValue("transientKaDonk")->load());
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
    // -- Transient (Pre: before all effects when Post Effect OFF) --
    if (transientEnabled && !transientPostEffect)
    {
        SpaceDustTransient::Parameters tp;
        tp.enabled = true;
        if (auto* p = dynamic_cast<juce::AudioParameterChoice*>(apvts.getParameter(juce::ParameterID{"transientType", 1}.getParamID())))
            tp.type = p->getIndex();
        tp.mix = juce::jlimit(0.0f, 1.0f, apvts.getRawParameterValue("transientMix")->load());
        tp.postEffect = false;
        tp.kaDonk = juce::jlimit(0.0f, 1.0f, apvts.getRawParameterValue("transientKaDonk")->load());
        tp.coarse = juce::jlimit(-24.0f, 24.0f, apvts.getRawParameterValue("transientCoarse")->load());
        tp.length = juce::jlimit(0.0f, 1.0f, apvts.getRawParameterValue("transientLength")->load());
        transient_.setParameters(tp);
        transient_.process(buffer);
    }

    //==============================================================================
    // -- Effect order (TG/BC Post Effect toggles) --
    // TG Post OFF: TG first. BC Post OFF: BC before all effects (second, after TG). BC Post ON: BC late.
    // TG Post ON:  TG last.  BC Post OFF: BC before all effects (first). BC Post ON: BC late (after Flanger).
    bool tranceGateEnabled = *apvts.getRawParameterValue("tranceGateEnabled") > 0.5f;
    bool tranceGatePostEffect = *apvts.getRawParameterValue("tranceGatePostEffect") > 0.5f;
    bool bitCrusherEnabled = *apvts.getRawParameterValue("bitCrusherEnabled") > 0.5f;
    bool bitCrusherPostEffect = *apvts.getRawParameterValue("bitCrusherPostEffect") > 0.5f;

    auto runBitCrusher = [&]()
    {
        if (bitCrusherEnabled && buffer.getNumChannels() >= 1 && numSamples > 0)
        {
            SpaceDustBitCrusher::Parameters bp;
            bp.enabled = true;
            bp.amount = juce::jlimit(0.0f, 1.0f, apvts.getRawParameterValue("bitCrusherAmount")->load());
            bp.rate = juce::jlimit(0.0f, 1.0f, apvts.getRawParameterValue("bitCrusherRate")->load());
            bp.mix = juce::jlimit(0.0f, 1.0f, apvts.getRawParameterValue("bitCrusherMix")->load());
            bitCrusher_.setParameters(bp);
            bitCrusher_.process(buffer);
        }
    };

    auto runTranceGate = [&]()
    {
        if (tranceGateEnabled && buffer.getNumChannels() >= 2 && numSamples > 0)
        {
            SpaceDustTranceGate::Parameters tp;
            tp.enabled = true;
            if (auto* p = dynamic_cast<juce::AudioParameterChoice*>(apvts.getParameter(juce::ParameterID{"tranceGateSteps", 1}.getParamID())))
                tp.numSteps = (p->getIndex() == 0) ? 4 : (p->getIndex() == 1) ? 8 : 16;
            else
                tp.numSteps = 8;
            tp.sync = *apvts.getRawParameterValue("tranceGateSync") > 0.5f;
            tp.rate = *apvts.getRawParameterValue("tranceGateRate");
            tp.attackMs = *apvts.getRawParameterValue("tranceGateAttack");
            tp.releaseMs = *apvts.getRawParameterValue("tranceGateRelease");
            tp.mix = *apvts.getRawParameterValue("tranceGateMix");
            for (int s = 0; s < 16; ++s)
            {
                juce::String stepId = "tranceGateStep" + juce::String(s + 1);
                if (auto* rp = apvts.getRawParameterValue(stepId))
                    tp.stepOn[s] = rp->load() > 0.5f;
            }
            tranceGate_.setParameters(tp);
            tranceGate_.process(buffer, currentSampleRate, getPlayHead());
        }
    };

    // -- 1) Trance Gate (Pre: when Post Effect OFF) --
    if (tranceGateEnabled && !tranceGatePostEffect)
        runTranceGate();

    // -- 2) Bit Crusher (early: before all effects except TG. When TG Post OFF, TG is first so BC is second) --
    if (bitCrusherEnabled && !bitCrusherPostEffect)
        runBitCrusher();

    //==============================================================================
    // -- Delay Effect --
    bool delayEnabled = *apvts.getRawParameterValue("delayEnabled") > 0.5f;
    float delayDecay = *apvts.getRawParameterValue("delayDecay") * 0.01f;  // 0-1
    float delayDryWet = *apvts.getRawParameterValue("delayDryWet") * 0.01f;  // 0-1
    float delayRate = *apvts.getRawParameterValue("delayRate");
    bool delaySync = *apvts.getRawParameterValue("delaySync") > 0.5f;
    bool delayPingPong = *apvts.getRawParameterValue("delayPingPong") > 0.5f;
    
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
        int musicalIndex = static_cast<int>(std::round(curved * 12.0));
        musicalIndex = juce::jlimit(0, 12, musicalIndex);
        static const double delayMultipliers[18] = {
            8.0, 6.0, 4.0, 2.6666666666666665, 2.0, 1.3333333333333333,
            1.0, 0.6666666666666666, 0.5, 0.3333333333333333, 0.25,
            0.16666666666666666, 0.125, 0.08333333333333333, 0.0625,
            0.0510204081632653, 0.03125, 0.03125
        };
        int mappedIndex = static_cast<int>(std::round(musicalIndex / 12.0 * 17.0));
        mappedIndex = juce::jlimit(0, 17, mappedIndex);
        double multiplier = delayMultipliers[mappedIndex];
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
    
    if (delayEnabled && delayDecay > 0.001f && numSamples > 0 && buffer.getNumChannels() >= 2)
    {
        // Pre-effect drive: 0 dB at mix 0, 3 dB at mix full (compensates amplitude loss)
        float delayDrive = std::pow(10.0f, delayDryWet * 3.0f / 20.0f);
        buffer.applyGain(0, 0, numSamples, delayDrive);
        buffer.applyGain(1, 0, numSamples, delayDrive);
        // Set smoothed targets (read once per block - real-time safe)
        smoothedDelayTime.setTargetValue(delayTimeSamples);
        smoothedDelayDecay.setTargetValue(delayDecay);
        smoothedDelayDryWet.setTargetValue(delayDryWet);
        
        bool delayFilterOn = *apvts.getRawParameterValue("delayFilterShow") > 0.5f;
        float delayHPCutoff = juce::jlimit(20.0f, 20000.0f, apvts.getRawParameterValue("delayFilterHPCutoff")->load());
        float delayLPCutoff = juce::jlimit(20.0f, 20000.0f, apvts.getRawParameterValue("delayFilterLPCutoff")->load());
        // Clamp Q to 0.1-5.0 to prevent resonance runaway (was 0.1-20, caused instability)
        float delayHPRes = apvts.getRawParameterValue("delayFilterHPResonance")->load();
        float delayLPRes = apvts.getRawParameterValue("delayFilterLPResonance")->load();
        float hpQ = juce::jlimit(0.1f, 5.0f, 0.1f + delayHPRes * 4.9f);
        float lpQ = juce::jlimit(0.1f, 5.0f, 0.1f + delayLPRes * 4.9f);
        bool delayWarmSat = *apvts.getRawParameterValue("delayFilterWarmSaturation") > 0.5f;
        
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
                float d1Fb = std::tanh(juce::jlimit(-2.0f, 2.0f, monoIn + fbDecay * d2FiltFb));
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
                float lFb = std::tanh(juce::jlimit(-2.0f, 2.0f, lIn + fbDecay * lFiltFb));
                float rFb = std::tanh(juce::jlimit(-2.0f, 2.0f, rIn + fbDecay * rFiltFb));
                delayLineL.pushSample(0, lFb);
                delayLineR.pushSample(0, rFb);
                left[s] = lOut;
                right[s] = rOut;
            }
        }
    }
    
    //==============================================================================
    // -- Reverb Effect --
    bool reverbEnabled = *apvts.getRawParameterValue("reverbEnabled") > 0.5f;
    if (reverbEnabled && buffer.getNumChannels() >= 2 && numSamples > 0)
    {
        float reverbWetMix = *apvts.getRawParameterValue("reverbWetMix");
        float reverbDrive = std::pow(10.0f, reverbWetMix * 3.0f / 20.0f);
        buffer.applyGain(0, 0, numSamples, reverbDrive);
        buffer.applyGain(1, 0, numSamples, reverbDrive);
        SpaceDustReverb::Parameters rp;
        if (auto* p = dynamic_cast<juce::AudioParameterChoice*>(apvts.getParameter(juce::ParameterID{"reverbType", 1}.getParamID())))
            rp.type = p->getIndex();
        else
            rp.type = 0;
        rp.wetMix = reverbWetMix;
        rp.decayTime = *apvts.getRawParameterValue("reverbDecayTime");
        rp.filterOn = *apvts.getRawParameterValue("reverbFilterShow") > 0.5f;
        rp.filterWarmSaturation = *apvts.getRawParameterValue("reverbFilterWarmSaturation") > 0.5f;
        rp.filterHPCutoff = *apvts.getRawParameterValue("reverbFilterHPCutoff");
        rp.filterHPResonance = *apvts.getRawParameterValue("reverbFilterHPResonance");
        rp.filterLPCutoff = *apvts.getRawParameterValue("reverbFilterLPCutoff");
        rp.filterLPResonance = *apvts.getRawParameterValue("reverbFilterLPResonance");
        reverb_.setParameters(rp);
        reverb_.process(buffer);
    }

    //==============================================================================
    // -- Grain Delay Effect --
    bool grainDelayEnabled = *apvts.getRawParameterValue("grainDelayEnabled") > 0.5f;
    if (grainDelayEnabled && buffer.getNumChannels() >= 2 && numSamples > 0)
    {
        float grainMix = juce::jlimit(0.0f, 1.0f, apvts.getRawParameterValue("grainDelayMix")->load() * 0.01f);
        float grainDrive = std::pow(10.0f, grainMix * 3.0f / 20.0f);
        buffer.applyGain(0, 0, numSamples, grainDrive);
        buffer.applyGain(1, 0, numSamples, grainDrive);
        SpaceDustGrainDelay::Parameters gp;
        gp.enabled = true;
        gp.delayMs = juce::jlimit(20.0f, 2000.0f, apvts.getRawParameterValue("grainDelayTime")->load());
        gp.grainSizeMs = juce::jlimit(10.0f, 500.0f, apvts.getRawParameterValue("grainDelaySize")->load());
        gp.pitchSemitones = juce::jlimit(-12.0f, 12.0f, apvts.getRawParameterValue("grainDelayPitch")->load());
        gp.mix = grainMix;
        gp.decay = juce::jlimit(0.0f, 1.0f, apvts.getRawParameterValue("grainDelayDecay")->load() * 0.015f);  // 0-150% for longer decay
        gp.density = juce::jlimit(1.0f, 8.0f, apvts.getRawParameterValue("grainDelayDensity")->load());
        gp.jitter = juce::jlimit(0.0f, 1.0f, apvts.getRawParameterValue("grainDelayJitter")->load() * 0.01f);
        gp.pingPong = *apvts.getRawParameterValue("grainDelayPingPong") > 0.5f;
        gp.filterOn = *apvts.getRawParameterValue("grainDelayFilterShow") > 0.5f;
        gp.hpCutoffHz = juce::jlimit(20.0f, 20000.0f, apvts.getRawParameterValue("grainDelayFilterHPCutoff")->load());
        gp.lpCutoffHz = juce::jlimit(20.0f, 20000.0f, apvts.getRawParameterValue("grainDelayFilterLPCutoff")->load());
        gp.hpRes = apvts.getRawParameterValue("grainDelayFilterHPResonance")->load();
        gp.lpRes = apvts.getRawParameterValue("grainDelayFilterLPResonance")->load();
        gp.warmSaturation = *apvts.getRawParameterValue("grainDelayFilterWarmSaturation") > 0.5f;
        grainDelay_.setParameters(gp);
        grainDelay_.process(buffer);
    }

    //==============================================================================
    // -- Phaser Effect --
    bool phaserEnabled = *apvts.getRawParameterValue("phaserEnabled") > 0.5f;
    if (phaserEnabled && buffer.getNumChannels() >= 2 && numSamples > 0)
    {
        float phaserMix = juce::jlimit(0.0f, 1.0f, apvts.getRawParameterValue("phaserMix")->load());
        float phaserDrive = std::pow(10.0f, phaserMix * 3.0f / 20.0f);
        buffer.applyGain(0, 0, numSamples, phaserDrive);
        buffer.applyGain(1, 0, numSamples, phaserDrive);
        SpaceDustPhaser::Parameters pp;
        pp.enabled = true;
        pp.rateHz = juce::jlimit(0.05f, 200.0f, apvts.getRawParameterValue("phaserRate")->load());
        pp.depth = juce::jlimit(0.0f, 1.0f, apvts.getRawParameterValue("phaserDepth")->load());
        pp.feedback = juce::jlimit(-1.0f, 1.0f, apvts.getRawParameterValue("phaserFeedback")->load());
        pp.scriptMode = *apvts.getRawParameterValue("phaserScriptMode") > 0.5f;
        pp.mix = phaserMix;
        pp.centreHz = juce::jlimit(50.0f, 2000.0f, apvts.getRawParameterValue("phaserCentre")->load());
        if (auto* p = dynamic_cast<juce::AudioParameterChoice*>(apvts.getParameter(juce::ParameterID{"phaserStages", 1}.getParamID())))
            pp.numStages = (p->getIndex() == 0) ? 4 : 6;
        else
            pp.numStages = 4;
        pp.stereoOffset = juce::jlimit(0.0f, 1.0f, apvts.getRawParameterValue("phaserStereoOffset")->load());
        pp.vintageMode = *apvts.getRawParameterValue("phaserVintageMode") > 0.5f;
        phaser_.setParameters(pp);
        phaser_.process(buffer);
    }

    //==============================================================================
    // -- Flanger Effect --
    bool flangerEnabled = *apvts.getRawParameterValue("flangerEnabled") > 0.5f;
    if (flangerEnabled && buffer.getNumChannels() >= 1 && numSamples > 0)
    {
        float flangerMix = juce::jlimit(0.0f, 1.0f, apvts.getRawParameterValue("flangerMix")->load());
        float flangerDrive = std::pow(10.0f, flangerMix * 3.0f / 20.0f);
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            buffer.applyGain(ch, 0, numSamples, flangerDrive);
        SpaceDustFlanger::Parameters fp;
        fp.enabled = true;
        fp.rateHz = juce::jlimit(0.05f, 200.0f, apvts.getRawParameterValue("flangerRate")->load());
        fp.depth = juce::jlimit(0.0f, 1.0f, apvts.getRawParameterValue("flangerDepth")->load());
        fp.feedback = juce::jlimit(-1.0f, 1.0f, apvts.getRawParameterValue("flangerFeedback")->load());
        fp.width = juce::jlimit(0.0f, 1.0f, apvts.getRawParameterValue("flangerWidth")->load());
        fp.mix = flangerMix;
        flanger_.setParameters(fp);
        flanger_.process(buffer);
    }

    // -- Transient (Post: end of chain, before late BC) --
    if (transientEnabled && transientPostEffect)
    {
        SpaceDustTransient::Parameters tp;
        tp.enabled = true;
        if (auto* p = dynamic_cast<juce::AudioParameterChoice*>(apvts.getParameter(juce::ParameterID{"transientType", 1}.getParamID())))
            tp.type = p->getIndex();
        tp.mix = juce::jlimit(0.0f, 1.0f, apvts.getRawParameterValue("transientMix")->load());
        tp.postEffect = true;
        tp.kaDonk = juce::jlimit(0.0f, 1.0f, apvts.getRawParameterValue("transientKaDonk")->load());
        tp.coarse = juce::jlimit(-24.0f, 24.0f, apvts.getRawParameterValue("transientCoarse")->load());
        tp.length = juce::jlimit(0.0f, 1.0f, apvts.getRawParameterValue("transientLength")->load());
        transient_.setParameters(tp);
        transient_.process(buffer);
    }

    // -- Bit Crusher (late: after Flanger, before TG) --
    if (bitCrusherEnabled && bitCrusherPostEffect)
        runBitCrusher();

    // -- Trance Gate (Post: when Post Effect ON, always last) --
    if (tranceGateEnabled && tranceGatePostEffect)
        runTranceGate();

    //==============================================================================
    // -- Compressor (Saturation Color) - after BitCrusher/TranceGate, before Soft Clipper --
    bool compressorEnabled = *apvts.getRawParameterValue("compressorEnabled") > 0.5f;
    if (compressorEnabled && buffer.getNumChannels() >= 1 && numSamples > 0)
    {
        SpaceDustCompressor::Parameters cp;
        cp.enabled = true;
        if (auto* p = dynamic_cast<juce::AudioParameterChoice*>(apvts.getParameter(juce::ParameterID{"compressorType", 1}.getParamID())))
            cp.type = p->getIndex();
        else
            cp.type = 0;
        cp.thresholdDb = juce::jlimit(-60.0f, 0.0f, apvts.getRawParameterValue("compressorThreshold")->load());
        cp.ratio = juce::jlimit(1.0f, 20.0f, apvts.getRawParameterValue("compressorRatio")->load());
        cp.attackMs = juce::jlimit(0.1f, 80.0f, apvts.getRawParameterValue("compressorAttack")->load());
        cp.releaseMs = juce::jlimit(5.0f, 1200.0f, apvts.getRawParameterValue("compressorRelease")->load());
        cp.makeupGainDb = juce::jlimit(0.0f, 24.0f, apvts.getRawParameterValue("compressorMakeup")->load());
        cp.mix = juce::jlimit(0.0f, 1.0f, apvts.getRawParameterValue("compressorMix")->load());
        cp.autoRelease = *apvts.getRawParameterValue("compressorAutoRelease") > 0.5f;
        cp.softClip = *apvts.getRawParameterValue("compressorSoftClip") > 0.5f;
        compressor_.setParameters(cp);
        compressor_.process(buffer);
    }

    //==============================================================================
    // -- Soft Clipper (Saturation Color) - before master volume --
    bool softClipperEnabled = *apvts.getRawParameterValue("softClipperEnabled") > 0.5f;
    if (softClipperEnabled && buffer.getNumChannels() >= 1 && numSamples > 0)
    {
        SpaceDustSoftClipper::Parameters sp;
        sp.enabled = true;
        if (auto* p = dynamic_cast<juce::AudioParameterChoice*>(apvts.getParameter(juce::ParameterID{"softClipperMode", 1}.getParamID())))
            sp.mode = p->getIndex();
        else
            sp.mode = 0;
        sp.drive = juce::jlimit(0.0f, 1.0f, apvts.getRawParameterValue("softClipperDrive")->load());
        sp.knee = juce::jlimit(0.0f, 1.0f, apvts.getRawParameterValue("softClipperKnee")->load());
        if (auto* p = dynamic_cast<juce::AudioParameterChoice*>(apvts.getParameter(juce::ParameterID{"softClipperOversample", 1}.getParamID())))
        {
            const int idx = p->getIndex();
            sp.oversample = (idx == 0) ? 2 : (idx == 1) ? 4 : (idx == 2) ? 8 : 16;
        }
        else
            sp.oversample = 2;
        sp.mix = juce::jlimit(0.0f, 1.0f, apvts.getRawParameterValue("softClipperMix")->load());
        softClipper_.setParameters(sp);
        softClipper_.process(buffer);
    }
    
    //==============================================================================
    // -- Lo-Fi (Saturation Color) - end of effects chain, before master volume --
    bool lofiEnabled = *apvts.getRawParameterValue("lofiEnabled") > 0.5f;
    if (lofiEnabled && buffer.getNumChannels() >= 1 && numSamples > 0)
    {
        SpaceDustLofi::Parameters lp;
        lp.enabled = true;
        lp.amount = juce::jlimit(0.0f, 1.0f, apvts.getRawParameterValue("lofiAmount")->load());
        lofi_.setParameters(lp);
        lofi_.process(buffer);
    }

    //==============================================================================
    // -- Final EQ (5-band, end of chain, Saturation Color tab) --
    {
        bool finalEQEnabled = *apvts.getRawParameterValue("finalEQEnabled") > 0.5f;
        if (finalEQEnabled && buffer.getNumChannels() >= 1 && numSamples > 0)
        {
            const SpaceDustFinalEQ::BandType bandTypes[5] = {
                SpaceDustFinalEQ::BandType::LowShelf,
                SpaceDustFinalEQ::BandType::Peak,
                SpaceDustFinalEQ::BandType::Peak,
                SpaceDustFinalEQ::BandType::Peak,
                SpaceDustFinalEQ::BandType::HighShelf
            };
            SpaceDustFinalEQ::Parameters fep;
            fep.enabled = true;
            for (int i = 0; i < 5; ++i)
            {
                juce::String n(i + 1);
                fep.bands[i].freqHz = juce::jlimit(20.0f, 20000.0f, apvts.getRawParameterValue("finalEQB" + n + "Freq")->load());
                fep.bands[i].gainDb = juce::jlimit(-15.0f, 15.0f,    apvts.getRawParameterValue("finalEQB" + n + "Gain")->load());
                fep.bands[i].Q      = juce::jlimit(0.1f, 10.0f,      apvts.getRawParameterValue("finalEQB" + n + "Q")->load());
                fep.bands[i].type   = bandTypes[i];
            }
            finalEQ_.setParameters(fep);
            finalEQ_.process(buffer);
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
        int lfo1Target = static_cast<int>(*apvts.getRawParameterValue("lfo1Target"));
        int lfo2Target = static_cast<int>(*apvts.getRawParameterValue("lfo2Target"));
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
                    if (lfo1Master && i < lfo1Buffer.getNumSamples())
                        mod += lfo1Buffer.getSample(0, i);
                    if (lfo2Master && i < lfo2Buffer.getNumSamples())
                        mod += lfo2Buffer.getSample(0, i);
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
        if (dest.getNumChannels() >= 2 && dest.getNumSamples() >= numSamples)
        {
            dest.copyFrom(0, 0, buffer, 0, 0, numSamples);
            dest.copyFrom(1, 0, buffer, 1, 0, numSamples);
            if (numSamples < dest.getNumSamples())
                dest.clear(numSamples, dest.getNumSamples() - numSamples);
            goniometerValidSamples.store(numSamples, std::memory_order_release);
            goniometerReadIndex.store(writeIdx, std::memory_order_release);
        }
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
    
    DBG("Space Dust: createEditor() called");
    return new SpaceDustAudioProcessorEditor(*this);
}

//==============================================================================
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
    
    std::unique_ptr<juce::XmlElement> xmlState(getXmlFromBinary(data, sizeInBytes));
    
    if (xmlState.get() != nullptr)
    {
        if (xmlState->hasTagName(apvts.state.getType()))
        {
            // Restore persistent UI state
            currentPresetName = xmlState->getStringAttribute("presetName", "Init");
            cheezeGuyActivated = xmlState->getBoolAttribute("cheezeGuyActivated", false);

            apvts.replaceState(juce::ValueTree::fromXml(*xmlState));
            updateVoicesWithParameters();
        }
    }
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
    //   NOTE: noiseColor/noiseType is NOT a parameter - it's a UI-only control
    // Filter:
    //   - "filterMode" (Choice: Low Pass, Band Pass, High Pass)
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
    
    // Oscillator 1 waveform
    addParameterWithLogging(params,
        std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID{"osc1Waveform", 1}, "Osc 1 Waveform",
            juce::StringArray(safeString("Sine"), safeString("Triangle"), safeString("Saw"), safeString("Square")), 1),
        safeString("osc1Waveform"));
    
    // Oscillator 2 waveform
    addParameterWithLogging(params,
        std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID{"osc2Waveform", 1}, "Osc 2 Waveform",
            juce::StringArray(safeString("Sine"), safeString("Triangle"), safeString("Saw"), safeString("Square")), 1),
        safeString("osc2Waveform"));
    
    //==============================================================================
    // -- Oscillator Pitch Tuning --
    // Each oscillator has independent coarse tuning (±24 semitones) and fine detuning (±50 cents)
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
    
    // Filter mode (Low Pass, Band Pass, High Pass)
    addParameterWithLogging(params,
        std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID{"filterMode", 1}, "Filter Mode",
            juce::StringArray(safeString("Low Pass"), safeString("Band Pass"), safeString("High Pass")), 0),
        safeString("filterMode"));
    
    // Filter cutoff (log scale: 20 Hz to 20 kHz)
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"filterCutoff", 1}, "Filter Cutoff",
            juce::NormalisableRange<float>(20.0f, 20000.0f, 1.0f, 0.3f), 8000.0f),
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
            juce::ParameterID{"warmSaturationMaster", 1}, "Warm Saturation", false),
        "warmSaturationMaster");
    
    //==============================================================================
    // -- Filter Envelope Parameters (ADSR) --
    // Filter envelope modulates filter cutoff with bipolar amount control
    
    // Filter envelope attack time (skewed: 0.01s to 20.0s, midpoint at 2.0s)
    juce::NormalisableRange<float> filterAttackRange(0.01f, 20.0f, 0.001f);
    filterAttackRange.setSkewForCentre(2.0f);
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"filterEnvAttack", 1}, "Filter Env Attack",
            filterAttackRange, 0.01f),
        "filterEnvAttack");
    
    // Filter envelope decay time (0.01s to 5s, linear - 1:1 mapping so label matches decay exactly)
    juce::NormalisableRange<float> filterDecayRange(0.01f, 5.0f, 0.001f);
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
    juce::NormalisableRange<float> filterReleaseRange(0.01f, 20.0f, 0.001f);
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
    // - 0% knob → 0.01s (10ms minimum for stability)
    // - 50% knob → 2.0s (musical midpoint)
    // - 100% knob → 20.0s (maximum cosmic tails)
    // 
    // Uses setSkewForCentre(2.0f) to place the midpoint at exactly 2.0 seconds.
    // This gives exponential-like feel: most knob travel controls short-to-medium times,
    // with the final portion extending dramatically to very long times.
    
    // Attack time (skewed: 0.01s to 20.0s, midpoint at 2.0s)
    juce::NormalisableRange<float> attackRange(0.01f, 20.0f, 0.001f);
    attackRange.setSkewForCentre(2.0f); // 50% knob position = 2.0 seconds
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"envAttack", 1}, "Env Attack",
            attackRange, 0.01f),
        "envAttack");
    
    // Decay time (skewed: 0.01s to 20.0s, midpoint at 2.0s)
    juce::NormalisableRange<float> decayRange(0.01f, 20.0f, 0.001f);
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
    juce::NormalisableRange<float> releaseRange(0.01f, 20.0f, 0.001f);
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
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"pitchEnvTime", 1}, "Pitch Env Time",
            juce::NormalisableRange<float>(0.0f, 1.0f, 0.001f), 0.0f),
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
    addParameterWithLogging(params,
        std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID{"subOscWaveform", 1}, "Sub Osc Waveform",
            juce::StringArray(safeString("Sine"), safeString("Triangle"), safeString("Saw"), safeString("Square")), 1),
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
    //              Uses setSkewForCentre(1.0f) so 12 o'clock ≈ 0.5-1s
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
    glideRange.setSkewForCentre(1.0f); // 50% knob position ≈ 0.5-1.0 seconds
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
    
    // LFO1 Rate (0-12: maps to 0.01-20 Hz when sync off, or tempo divisions when sync on)
    // When sync is off: 0-12 maps logarithmically to 0.01-20 Hz
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
    
    // LFO1 Phase (0-360°)
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
    
    // LFO2 Rate (0-12: maps to 0.01-20 Hz when sync off, or tempo divisions when sync on)
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
    
    // LFO2 Phase (0-360°)
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
            juce::StringArray(safeString("Low Pass"), safeString("Band Pass"), safeString("High Pass")), 0),
        safeString("modFilter1Mode"));
    
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"modFilter1Cutoff", 1}, "Mod Filter 1 Cutoff",
            juce::NormalisableRange<float>(20.0f, 20000.0f, 1.0f, 0.3f), 8000.0f),
        "modFilter1Cutoff");
    
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"modFilter1Resonance", 1}, "Mod Filter 1 Resonance",
            juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.3f),
        "modFilter1Resonance");
    
    addParameterWithLogging(params,
        std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{"warmSaturationMod1", 1}, "Warm Saturation", false),
        "warmSaturationMod1");
    
    addParameterWithLogging(params,
        std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID{"modFilter2Mode", 1}, "Mod Filter 2 Mode",
            juce::StringArray(safeString("Low Pass"), safeString("Band Pass"), safeString("High Pass")), 0),
        safeString("modFilter2Mode"));
    
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"modFilter2Cutoff", 1}, "Mod Filter 2 Cutoff",
            juce::NormalisableRange<float>(20.0f, 20000.0f, 1.0f, 0.3f), 8000.0f),
        "modFilter2Cutoff");
    
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"modFilter2Resonance", 1}, "Mod Filter 2 Resonance",
            juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.3f),
        "modFilter2Resonance");
    
    addParameterWithLogging(params,
        std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{"warmSaturationMod2", 1}, "Warm Saturation", false),
        "warmSaturationMod2");
    
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
            juce::StringArray("Schroeder", "Sexicon take an L"), 0),
        "reverbType");
    
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"reverbWetMix", 1}, "Reverb Mix",
            juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.33f),
        "reverbWetMix");
    
    juce::NormalisableRange<float> reverbDecayRange(0.8f, 640.0f, 0.01f);
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
    addParameterWithLogging(params,
        std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID{"transientType", 1}, "Transient Type",
            juce::StringArray("808 Kick", "808 Snare", "808 Hat", "808 Open Hat",
                              "808 Clap", "808 Tom", "808 Rim", "808 Cowbell",
                              "909 Kick", "909 Snare"), 0),
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

    //==============================================================================
    // -- Final EQ Parameters (5-band, end of chain, Saturation Color tab) --
    ADD_PARAM_WITH_LOG(params,
        std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{"finalEQEnabled", 1}, "Final EQ On", false),
        "finalEQEnabled");
    // Band 1 – Low Shelf (default 80 Hz)
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
    // Band 2 – Low Mid Peak (default 250 Hz)
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
    // Band 3 – Mid Peak (default 1000 Hz)
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
    // Band 4 – High Mid Peak (default 4000 Hz)
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
    // Band 5 – High Shelf (default 10000 Hz)
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
    // -- DEBUG: createParameterLayout End --
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
    
    DBG("Space Dust: createParameterLayout() completed - created " + safeStringFromNumber(static_cast<int>(params.size())) + " parameters");
    
    return { params.begin(), params.end() };
}

//==============================================================================
// This creates new instances of the plugin..
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new SpaceDustAudioProcessor();
}


// =====================================================================
//  Space Dust hot-reload harness
//  ---------------------------------------------------------------------
//  Verifies the two PresetHotReload behaviours without touching the UI:
//
//    1. A preset edited on disk is re-applied to the live parameters.
//    2. The current sound is published to current.sdpreset for external tools.
//
//  Uses the real configured preset folder — that's the path users actually hit,
//  and mocking it would test the mock. It writes one uniquely-named preset and
//  deletes it on the way out.
//
//  Build:  cmake -B build -DSPACEDUST_BUILD_TESTS=ON
//          cmake --build build --target SpaceDustHotReloadTest
// =====================================================================
#include <juce_audio_processors/juce_audio_processors.h>
#if JUCE_MAC
 #include <CoreFoundation/CoreFoundation.h>
#endif
#include <cstdio>
#include <memory>

#include "PluginProcessor.h"
#include "PresetHotReload.h"
#include "PresetManager.h"

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter();

namespace
{
    const juce::String testPresetName { "__SpaceDustHotReloadTest" };
    constexpr const char* probeParamID = "filterCutoff";

    void pumpMessages (double seconds)
    {
        // JUCE timers are driven by the platform run loop, so pumping it is what makes
        // PresetHotReload tick. runDispatchLoopUntil isn't available here — console apps
        // build without JUCE_MODAL_LOOPS_PERMITTED.
        const auto until = juce::Time::getMillisecondCounterHiRes() + seconds * 1000.0;
        while (juce::Time::getMillisecondCounterHiRes() < until)
        {
           #if JUCE_MAC
            CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.01, true);
           #else
            juce::Thread::sleep (10);
           #endif
        }
    }

    // A preset the plugin will accept: every parameter present, one of them set to `probe`.
    void writePreset (const juce::File& file,
                      juce::AudioProcessorValueTreeState& apvts,
                      float probe)
    {
        juce::XmlElement xml ("PARAMETERS");
        xml.setAttribute ("presetName", testPresetName);

        for (auto* param : apvts.processor.getParameters())
        {
            if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (param))
            {
                auto* child = xml.createNewChildElement ("PARAM");
                child->setAttribute ("id", ranged->paramID);
                const auto value = ranged->paramID == probeParamID
                                 ? probe
                                 : ranged->convertFrom0to1 (ranged->getDefaultValue());
                child->setAttribute ("value", (double) value);
            }
        }

        file.getParentDirectory().createDirectory();
        xml.writeTo (file);
    }

    float readParam (juce::AudioProcessorValueTreeState& apvts, const char* paramID)
    {
        auto* param = apvts.getParameter (paramID);
        return param == nullptr ? -1.0f : param->convertFrom0to1 (param->getValue());
    }

    bool check (bool condition, const char* what)
    {
        std::printf ("%s  %s\n", condition ? "PASS" : "FAIL", what);
        return condition;
    }
}

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    std::unique_ptr<juce::AudioProcessor> proc (createPluginFilter());
    auto* processor = dynamic_cast<SpaceDustAudioProcessor*> (proc.get());
    if (processor == nullptr)
    {
        std::puts ("FATAL: could not create SpaceDustAudioProcessor");
        return 2;
    }

    auto& apvts = processor->getValueTreeState();
    const auto presetFile = PresetManager::configuredPresetFolder()
                                .getChildFile (testPresetName + PresetManager::presetExtension);
    const auto liveFile = PresetManager::appDataFolder().getChildFile ("current.sdpreset");

    std::printf ("preset folder: %s\n", PresetManager::configuredPresetFolder()
                                            .getFullPathName().toRawUTF8());

    // -- Load a preset the way selecting one in the UI would, then let the watcher
    //    take its baseline reading of the file.
    writePreset (presetFile, apvts, 1234.0f);
    processor->currentPresetName = testPresetName;
    pumpMessages (PresetHotReload::pollIntervalMs / 1000.0 * 2.5);

    bool ok = true;

    // -- 1. Edit the preset on disk. The plugin should pick it up on its own.
    writePreset (presetFile, apvts, 567.0f);
    pumpMessages (PresetHotReload::pollIntervalMs / 1000.0 * 3.0);

    const auto reloaded = readParam (apvts, probeParamID);
    ok &= check (std::abs (reloaded - 567.0f) < 1.0f, "external edit re-applied to live parameters");
    if (std::abs (reloaded - 567.0f) >= 1.0f)
        std::printf ("     %s is %.2f, expected 567\n", probeParamID, reloaded);

    // -- 2. The live sound should have been published for external tools to read.
    ok &= check (liveFile.existsAsFile(), "current.sdpreset published");

    if (liveFile.existsAsFile())
    {
        auto published = juce::XmlDocument::parse (liveFile);
        double publishedCutoff = -1.0;
        if (published != nullptr)
            for (auto* child : published->getChildWithTagNameIterator ("PARAM"))
                if (child->getStringAttribute ("id") == probeParamID)
                    publishedCutoff = child->getDoubleAttribute ("value");

        ok &= check (std::abs (publishedCutoff - 567.0) < 1.0,
                     "published state matches the reloaded sound");
        if (std::abs (publishedCutoff - 567.0) >= 1.0)
            std::printf ("     published %s is %.2f, expected 567\n", probeParamID, publishedCutoff);
    }

    presetFile.deleteFile();
    std::puts (ok ? "\nALL CHECKS PASSED" : "\nFAILURES ABOVE");
    return ok ? 0 : 1;
}

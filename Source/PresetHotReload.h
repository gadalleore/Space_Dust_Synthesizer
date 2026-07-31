#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

/**
    Keeps the plugin in step with preset files edited outside it.

    Two jobs, both driven from one message-thread timer:

    - Publishes the current parameter values to "current.sdpreset" in the Space Dust app
      data folder, so an external tool can see what the user is actually hearing.
    - Watches the file backing the currently-loaded preset and re-applies it when it
      changes on disk, so editing a preset externally is heard immediately instead of
      after the user re-picks it from the preset menu.

    Lives on the processor rather than the editor: the user may well have the plugin
    window closed while working, and the processor outlives the editor.
*/
class PresetHotReload : private juce::Timer
{
public:
    PresetHotReload(juce::AudioProcessor& processorToUse,
                    juce::AudioProcessorValueTreeState& stateToUse,
                    const juce::String& currentPresetNameRef);
    ~PresetHotReload() override;

    void start();
    void stop();

    // Polling beats a filesystem watcher here: a few lines, identical on every platform,
    // and a second of latency is imperceptible in an edit-and-listen loop.
    static constexpr int pollIntervalMs = 1000;

private:
    void timerCallback() override;
    void publishCurrentState();
    void reloadPresetIfChangedOnDisk();
    juce::File currentPresetFile() const;

    juce::AudioProcessor& processor;
    juce::AudioProcessorValueTreeState& valueTreeState;
    const juce::String& currentPresetName;

    juce::MemoryBlock lastPublishedValues;
    juce::File watchedFile;
    juce::Time watchedFileTime;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PresetHotReload)
};

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>

class PresetManager
{
public:
    PresetManager(juce::AudioProcessorValueTreeState& apvts);

    //==============================================================================
    // Preset Operations
    void savePreset(const juce::String& presetName);
    void loadPreset(const juce::File& presetFile);
    void loadInitPreset();

    //==============================================================================
    // Folder Management
    void setPresetFolder(const juce::File& folder);
    juce::File getPresetFolder() const;
    juce::Array<juce::File> getAvailablePresets() const;

    //==============================================================================
    // Current State
    juce::String getCurrentPresetName() const { return currentPresetName; }
    void setCurrentPresetName(const juce::String& name) { currentPresetName = name; }

    //==============================================================================
    // File extension
    static constexpr const char* presetExtension = ".sdpreset";
    static constexpr const char* presetWildcard = "*.sdpreset";

private:
    juce::AudioProcessorValueTreeState& valueTreeState;
    juce::File presetFolder;
    juce::String currentPresetName { "Init" };

    juce::File getDefaultPresetFolder() const;
    void savePresetFolderConfig() const;
    void loadPresetFolderConfig();
    juce::File getConfigFile() const;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PresetManager)
};

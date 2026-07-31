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

    //==============================================================================
    // Where presets live, resolved the same way whether or not an editor (and so a
    // PresetManager) exists. PresetHotReload needs this from the processor, which
    // outlives the editor, so the lookup can't be tied to an instance.
    static juce::File configuredPresetFolder();
    // Per-user settings folder. Also where the plugin publishes its live state.
    static juce::File appDataFolder();

private:
    juce::AudioProcessorValueTreeState& valueTreeState;
    juce::File presetFolder;
    juce::String currentPresetName { "Init" };

    static juce::File getDefaultPresetFolder();
    void savePresetFolderConfig() const;
    void loadPresetFolderConfig();
    // Per-user config (user-writable). Preferred on read, and the only write target.
    static juce::File getUserConfigFile();
    // Where older macOS builds wrote the per-user config. Read-only fallback.
    static juce::File getLegacyUserConfigFile();
    // System-wide config written by an all-users installer. Read-only fallback.
    static juce::File getSystemConfigFile();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PresetManager)
};

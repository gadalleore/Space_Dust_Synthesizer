#include "PresetManager.h"

PresetManager::PresetManager(juce::AudioProcessorValueTreeState& apvts)
    : valueTreeState(apvts)
{
    // Load saved preset folder from config, or use default
    loadPresetFolderConfig();

    // Create preset folder if it doesn't exist
    if (!presetFolder.exists())
        presetFolder.createDirectory();
}

//==============================================================================
void PresetManager::savePreset(const juce::String& presetName)
{
    if (presetName.isEmpty())
        return;

    auto state = valueTreeState.copyState();
    std::unique_ptr<juce::XmlElement> xml(state.createXml());

    if (xml == nullptr)
        return;

    // Add preset name as an attribute on the root element
    xml->setAttribute("presetName", presetName);

    auto presetFile = presetFolder.getChildFile(presetName + presetExtension);
    xml->writeTo(presetFile);

    currentPresetName = presetName;
}

//==============================================================================
void PresetManager::loadPreset(const juce::File& presetFile)
{
    if (!presetFile.existsAsFile())
        return;

    auto xml = juce::XmlDocument::parse(presetFile);
    if (xml == nullptr)
        return;

    // Verify tag matches APVTS state type
    if (!xml->hasTagName(valueTreeState.state.getType()))
        return;

    // Extract preset name from XML attribute, or use filename
    auto name = xml->getStringAttribute("presetName",
                    presetFile.getFileNameWithoutExtension());

    valueTreeState.replaceState(juce::ValueTree::fromXml(*xml));
    currentPresetName = name;
}

//==============================================================================
void PresetManager::loadInitPreset()
{
    // Reset all parameters to their default values
    auto& params = valueTreeState.processor.getParameters();
    for (auto* param : params)
    {
        if (auto* rangedParam = dynamic_cast<juce::RangedAudioParameter*>(param))
            rangedParam->setValueNotifyingHost(rangedParam->getDefaultValue());
    }

    currentPresetName = "Init";
}

//==============================================================================
void PresetManager::setPresetFolder(const juce::File& folder)
{
    presetFolder = folder;
    if (!presetFolder.exists())
        presetFolder.createDirectory();
    savePresetFolderConfig();
}

juce::File PresetManager::getPresetFolder() const
{
    return presetFolder;
}

juce::Array<juce::File> PresetManager::getAvailablePresets() const
{
    juce::Array<juce::File> presets;
    if (presetFolder.exists())
    {
        presetFolder.findChildFiles(presets, juce::File::findFiles, false, presetWildcard);
        // Sort alphabetically by name
        presets.sort();
    }
    return presets;
}

//==============================================================================
juce::File PresetManager::getDefaultPresetFolder() const
{
    return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
        .getChildFile("Space Dust")
        .getChildFile("Presets");
}

juce::File PresetManager::getConfigFile() const
{
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("Space Dust")
        .getChildFile("config.xml");
}

void PresetManager::savePresetFolderConfig() const
{
    auto configFile = getConfigFile();
    configFile.getParentDirectory().createDirectory();

    juce::XmlElement config("SpaceDustConfig");
    config.setAttribute("presetFolder", presetFolder.getFullPathName());
    config.writeTo(configFile);
}

void PresetManager::loadPresetFolderConfig()
{
    auto configFile = getConfigFile();

    if (configFile.existsAsFile())
    {
        auto xml = juce::XmlDocument::parse(configFile);
        if (xml != nullptr && xml->hasTagName("SpaceDustConfig"))
        {
            auto folderPath = xml->getStringAttribute("presetFolder");
            if (folderPath.isNotEmpty())
            {
                presetFolder = juce::File(folderPath);
                if (presetFolder.exists())
                    return;
            }
        }
    }

    // Fall back to default
    presetFolder = getDefaultPresetFolder();
}

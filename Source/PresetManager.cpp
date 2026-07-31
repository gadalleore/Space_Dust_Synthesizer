#include "PresetManager.h"

// The folder presets and config live under. Supplied by CMake from the product name so
// that a side-by-side V2 build keeps its own presets and cannot write over the shipping
// V1's (see the V2 test identity block in CMakeLists.txt). The fallback is the real
// shipping name, so a build without the definition behaves exactly as V1 always has.
#ifndef SPACEDUST_DATA_FOLDER
 #define SPACEDUST_DATA_FOLDER "Space Dust"
#endif

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
        {
            // Wrap each reset in a balanced gesture. A burst of naked
            // setValueNotifyingHost (performEdit) calls corrupts FL Studio's
            // "Last Tweaked" tracking, which breaks subsequently-created automation.
            rangedParam->beginChangeGesture();
            rangedParam->setValueNotifyingHost(rangedParam->getDefaultValue());
            rangedParam->endChangeGesture();
        }
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
juce::File PresetManager::getDefaultPresetFolder()
{
    return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
        .getChildFile(SPACEDUST_DATA_FOLDER)
        .getChildFile("Presets");
}

juce::File PresetManager::appDataFolder()
{
    auto userData = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory);

   #if JUCE_MAC
    // JUCE maps userApplicationDataDirectory to "~/Library" on macOS, not to
    // "~/Library/Application Support" where per-user settings belong and where the
    // installer writes config.xml. Be explicit rather than inheriting that surprise.
    return userData.getChildFile("Application Support").getChildFile("Space Dust");
   #else
    return userData.getChildFile("Space Dust");
   #endif
}

juce::File PresetManager::getUserConfigFile()
{
    // %APPDATA%\Space Dust\config.xml on Windows, ~/Library/Application Support/... on macOS.
    // Always user-writable so the plugin can persist preset-folder changes without elevation.
    return appDataFolder().getChildFile("config.xml");
}

juce::File PresetManager::getLegacyUserConfigFile()
{
    // Builds before appDataFolder() was corrected wrote here on macOS. Read-only, so a
    // user who had changed their preset folder in-plugin doesn't silently lose it.
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile(SPACEDUST_DATA_FOLDER)
        .getChildFile("config.xml");
}

juce::File PresetManager::getSystemConfigFile()
{
    // %ProgramData%\Space Dust\config.xml on Windows. Written by the all-users installer;
    // used as a read-only fallback when no per-user config exists yet.
    return juce::File::getSpecialLocation(juce::File::commonApplicationDataDirectory)
        .getChildFile(SPACEDUST_DATA_FOLDER)
        .getChildFile("config.xml");
}

void PresetManager::savePresetFolderConfig() const
{
    // Always write to the user location: the plugin runs at user privileges and cannot
    // write to ProgramData. On load, the user config takes precedence over the system one.
    auto configFile = getUserConfigFile();
    configFile.getParentDirectory().createDirectory();

    juce::XmlElement config("SpaceDustConfig");
    config.setAttribute("presetFolder", presetFolder.getFullPathName());
    config.writeTo(configFile);
}

juce::File PresetManager::configuredPresetFolder()
{
    auto tryLoad = [](const juce::File& configFile, juce::File& out) -> bool
    {
        if (!configFile.existsAsFile())
            return false;
        auto xml = juce::XmlDocument::parse(configFile);
        if (xml == nullptr || !xml->hasTagName("SpaceDustConfig"))
            return false;
        auto folderPath = xml->getStringAttribute("presetFolder");
        if (folderPath.isEmpty())
            return false;
        out = juce::File(folderPath);
        return out.exists();
    };

    juce::File folder;
    // User config (per-user install OR previous in-plugin folder change) wins.
    if (tryLoad(getUserConfigFile(), folder))
        return folder;
    // Then wherever older macOS builds put it.
    if (tryLoad(getLegacyUserConfigFile(), folder))
        return folder;
    // Fall back to system config written by an all-users installer.
    if (tryLoad(getSystemConfigFile(), folder))
        return folder;

    return getDefaultPresetFolder();
}

void PresetManager::loadPresetFolderConfig()
{
    presetFolder = configuredPresetFolder();
}

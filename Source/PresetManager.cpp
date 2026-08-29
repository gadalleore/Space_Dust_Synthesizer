#include "PresetManager.h"
#include "PluginProcessor.h"   // migrateLfoRatesIfOld / stateVersion

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

    // Marks which meaning the stored values carry, exactly as getStateInformation
    // does. Missing, it read back as version 1 on every load -- so every preset,
    // however recently saved, had the LFO-rate and waveform migrations run over
    // it each time it was opened, renumbering waveforms that were already right.
    xml->setAttribute("stateVersion", SpaceDustAudioProcessor::currentStateVersion);

    // A preset carries the SOUND. Everything getStateInformation writes that the
    // sound depends on is written here too; the two attributes it writes that the
    // sound does not depend on -- cheezeGuyActivated and lastActiveTabIndex --
    // are deliberately left out. They are per-session window state, and a preset
    // that moved the player's open tab when they auditioned it would be a bug.
    if (auto* spaceDust = dynamic_cast<SpaceDustAudioProcessor*>(&valueTreeState.processor))
    {
        // The imported waveforms go in with it, audio and all. A preset that named a
        // parameter for a user slot but did not carry the slot was a preset that
        // sounded different on the next machine, or after the user tidied up.
        if (auto waves = spaceDust->getUserWaveLibrary().createStateXml())
            xml->addChildElement(waves.release());

        // The routing list and the drawn pitch shape travel with the preset for
        // the same reason they travel with the song: neither is in the parameter
        // list, so copyState() above does not contain them. Without these two
        // lines a preset silently discarded every routing and the whole curve --
        // it saved and reloaded as a patch whose LFOs went nowhere.
        xml->addChildElement(spacedust::toXml(spaceDust->modMatrix).release());
        xml->addChildElement(spacedust::pitchCurveToXml(spaceDust->pitchCurve).release());
    }

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

    // This whole function mirrors SpaceDustAudioProcessor::setStateInformation.
    // A preset and a song carry the same sound, so they must be restored the same
    // way; the two paths having drifted apart is exactly how presets came to lose
    // their routings and their curve. Where this differs, the difference is
    // deliberate and said so at the point it happens.

    // Lift the waveforms OUT before the rest becomes the parameter tree. Left in
    // place they would become a child of the APVTS state and be written out again
    // by the next save, doubling in size on every round trip.
    std::unique_ptr<juce::XmlElement> waves;

    if (auto* stored = xml->getChildByName("USERWAVES"))
    {
        // The false says "do not delete it" - ownership passes to us here.
        xml->removeChildElement(stored, false);
        waves.reset(stored);
    }

    // Lifted OUT for the same reason and in the same way as USERWAVES above.
    std::unique_ptr<juce::XmlElement> matrixXml;

    if (auto* stored = xml->getChildByName("MODMATRIX"))
    {
        xml->removeChildElement(stored, false);
        matrixXml.reset(stored);
    }

    std::unique_ptr<juce::XmlElement> curveXml;

    if (auto* stored = xml->getChildByName("PITCHCURVE"))
    {
        xml->removeChildElement(stored, false);
        curveXml.reset(stored);
    }

    // Presets saved before the LFO free-rate range was widened to 2000 Hz store a knob
    // position that meant a different frequency. Rescale so they sound as recorded.
    auto restored = juce::ValueTree::fromXml(*xml);
    const int savedVersion = xml->getIntAttribute("stateVersion", 1);
    SpaceDustAudioProcessor::migrateLfoRatesIfOld(restored, savedVersion);

    // Presets written when there were four built-in shapes store a User slot as 4
    // upwards. Seventeen shapes now sit in front of those slots, so the stored
    // number has to move up with them or the preset comes back on a shape it was
    // never saved with.
    SpaceDustAudioProcessor::migrateWaveformChoicesIfOld(restored, savedVersion);

    valueTreeState.replaceState(restored);

    if (auto* spaceDust = dynamic_cast<SpaceDustAudioProcessor*>(&valueTreeState.processor))
    {
        // After the parameters, so the waveform choices they restored already point at
        // the slots this is about to fill. A preset from before waveforms travelled
        // has no block here at all, and leaves the user's slots exactly as they were.
        if (waves != nullptr)
            spaceDust->getUserWaveLibrary().restoreFromStateXml(*waves);

        // A preset with no MODMATRIX is one saved before routings existed.
        // Clearing rather than leaving the last preset's routings in place is
        // what stops one preset's movement leaking into the next -- and stepping
        // down a preset list is where that leak shows up hardest.
        const bool hadSavedModMatrix = (matrixXml != nullptr);

        if (hadSavedModMatrix)
            spacedust::fromXml(*matrixXml, spaceDust->modMatrix);
        else
            spaceDust->modMatrix.clear();

        // Same reasoning as MODMATRIX just above.
        const bool hadSavedPitchCurve = (curveXml != nullptr);

        if (hadSavedPitchCurve)
            spacedust::pitchCurveFromXml(*curveXml, spaceDust->pitchCurve);
        else
            spaceDust->pitchCurve.clear();

        // After the clear/fromXml above, not before, for the reason set out in
        // full inside migrateLfoTargetsIfOld. `restored` is a plain ValueTree
        // that still holds lfo1Target/lfo2Target and pitchEnvAmount/Time/Pitch,
        // which this build has no parameters for. Without this call every
        // preset in the player's existing library loaded with its LFOs routed
        // nowhere and no pitch envelope at all.
        SpaceDustAudioProcessor::migrateLfoTargetsIfOld(restored,
                                                        spaceDust->modMatrix,
                                                        hadSavedModMatrix,
                                                        spaceDust->pitchCurve,
                                                        hadSavedPitchCurve);

        spaceDust->updateVoicesWithParameters();

        // The routing list has just been replaced, so the audio thread's compiled
        // copy of it is stale. This is the message thread -- loadPreset is only
        // ever reached from the editor's preset box -- which is the only place
        // this may be called from, because it allocates.
        spaceDust->rebuildCompiledRoutings();
    }

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
    return userData.getChildFile("Application Support").getChildFile(SPACEDUST_DATA_FOLDER);
   #else
    return userData.getChildFile(SPACEDUST_DATA_FOLDER);
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

#include "PresetHotReload.h"

#include "PresetManager.h"

namespace
{
    // The name PresetManager gives a preset that has never been saved. There's no file
    // behind it, so there's nothing to watch.
    const juce::String initPresetName { "Init" };

    juce::File liveStateFile()
    {
        return PresetManager::appDataFolder().getChildFile("current.sdpreset");
    }
}

PresetHotReload::PresetHotReload(juce::AudioProcessor& processorToUse,
                                 juce::AudioProcessorValueTreeState& stateToUse,
                                 const juce::String& currentPresetNameRef)
    : processor(processorToUse),
      valueTreeState(stateToUse),
      currentPresetName(currentPresetNameRef)
{
}

PresetHotReload::~PresetHotReload()
{
    stop();
}

void PresetHotReload::start()
{
    startTimer(pollIntervalMs);
}

void PresetHotReload::stop()
{
    stopTimer();
}

juce::File PresetHotReload::currentPresetFile() const
{
    if (currentPresetName.isEmpty() || currentPresetName == initPresetName)
        return {};

    return PresetManager::configuredPresetFolder()
        .getChildFile(currentPresetName + PresetManager::presetExtension);
}

void PresetHotReload::timerCallback()
{
    // Order matters: reload first, then publish. Publishing first would write out the
    // pre-reload values and immediately be superseded.
    reloadPresetIfChangedOnDisk();
    publishCurrentState();
}

void PresetHotReload::publishCurrentState()
{
    // Comparing the raw parameter values is enough to know whether anything moved, and is
    // far cheaper than serialising and diffing XML once a second. At four bytes per
    // parameter the snapshot is under a kilobyte, so there's nothing to gain from hashing.
    juce::MemoryOutputStream snapshot;
    for (auto* param : processor.getParameters())
        if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*>(param))
            snapshot.writeFloat(ranged->getValue());

    if (snapshot.getMemoryBlock() == lastPublishedValues)
        return;

    auto state = valueTreeState.copyState();
    std::unique_ptr<juce::XmlElement> xml(state.createXml());
    if (xml == nullptr)
        return;

    xml->setAttribute("presetName", currentPresetName);

    auto target = liveStateFile();
    target.getParentDirectory().createDirectory();
    if (xml->writeTo(target))
        lastPublishedValues = snapshot.getMemoryBlock();
}

void PresetHotReload::reloadPresetIfChangedOnDisk()
{
    auto file = currentPresetFile();

    if (file != watchedFile)
    {
        // Switched preset (or back to Init). Take the current timestamp as the baseline
        // so selecting a preset doesn't immediately count as an external edit.
        watchedFile = file;
        watchedFileTime = file.existsAsFile() ? file.getLastModificationTime() : juce::Time();
        return;
    }

    if (!file.existsAsFile())
        return;

    auto modified = file.getLastModificationTime();
    if (modified == watchedFileTime)
        return;

    watchedFileTime = modified;

    auto xml = juce::XmlDocument::parse(file);
    if (xml == nullptr || !xml->hasTagName(valueTreeState.state.getType()))
        return;

    // Deliberately not replaceState(): that swaps the tree out from under the host and
    // the editor's attachments without telling either. Pushing each parameter through
    // setValueNotifyingHost keeps automation, undo and the UI in sync.
    //
    // Only changed parameters are pushed. loadInitPreset()'s comment records that a burst
    // of these calls corrupts FL Studio's "Last Tweaked" tracking; balanced gestures fix
    // the correctness problem, but firing all 200-plus every second is still needless
    // host traffic when a typical external edit touches a handful.
    for (auto* child : xml->getChildWithTagNameIterator("PARAM"))
    {
        const auto paramID = child->getStringAttribute("id");
        if (paramID.isEmpty() || !child->hasAttribute("value"))
            continue;

        auto* param = valueTreeState.getParameter(paramID);
        if (param == nullptr)
            continue;  // Preset written by a newer build; ignore what we don't have.

        const auto target = param->convertTo0to1(
            static_cast<float>(child->getDoubleAttribute("value")));

        if (std::abs(param->getValue() - target) < 1.0e-6f)
            continue;

        param->beginChangeGesture();
        param->setValueNotifyingHost(target);
        param->endChangeGesture();
    }
}

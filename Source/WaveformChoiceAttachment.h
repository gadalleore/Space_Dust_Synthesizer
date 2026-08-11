#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <cmath>

/**
    Binds a waveform dropdown to its Choice parameter BY ITEM ID, not by position.

    WHY NOT THE STOCK ATTACHMENT

    juce::AudioProcessorValueTreeState::ComboBoxAttachment maps the parameter to
    the box's item INDEX, and so requires the menu and the parameter to hold the
    same number of entries. That requirement cannot be met here, because the two
    lists answer to different masters:

      the PARAMETER must always offer all eight User slots, filled or not. VST3
      and AU freeze a Choice parameter's item count when the plugin loads, and a
      host writes automation against that list and recalls it by position. A list
      that grew when the user imported another sample would silently move every
      automation point already written.

      the MENU must show only the slots that hold a waveform. Six rows reading
      "User 3", "User 4"... are six things the player cannot choose and does not
      want to scroll past.

    So the menu carries each entry's parameter index in its ITEM ID -- id is
    always index + 1 -- and is then free to contain any subset of the parameter's
    choices, in any number. Rebuilding the menu cannot disturb the parameter, and
    a preset written when a slot was full still selects the right thing.

    The one case the caller must handle is a parameter pointing at a slot that is
    now empty: there is no item to select, so the box would go blank. See
    SpaceDustAudioProcessorEditor::rebuildWaveformMenus, which keeps that one
    entry visible rather than letting the selection disappear.
*/
class WaveformChoiceAttachment : private juce::ComboBox::Listener
{
public:
    WaveformChoiceAttachment (juce::RangedAudioParameter& parameterToUse,
                              juce::ComboBox& comboBoxToUse,
                              juce::UndoManager* undoManager = nullptr)
        : combo (comboBoxToUse),
          parameter (parameterToUse),
          attachment (parameterToUse, [this] (float value) { setValue (value); }, undoManager)
    {
        attachment.sendInitialUpdate();
        combo.addListener (this);
    }

    ~WaveformChoiceAttachment() override
    {
        combo.removeListener (this);
    }

    /** The parameter's current choice index, which is also the id it selects
        minus one. Read from the parameter rather than from the box, because the
        box may be missing the entry for an empty slot. */
    int currentIndex() const
    {
        return (int) std::lround (parameter.convertFrom0to1 (parameter.getValue()));
    }

private:
    void setValue (float newValue)
    {
        // dontSendNotification: this came FROM the parameter, so writing it back
        // would be a loop, and an automation pass would fight the host for it.
        const juce::ScopedValueSetter<bool> guard (ignoreCallbacks, true);
        combo.setSelectedId ((int) std::lround (newValue) + 1, juce::dontSendNotification);
    }

    void comboBoxChanged (juce::ComboBox*) override
    {
        if (ignoreCallbacks)
            return;

        const int id = combo.getSelectedId();

        // Zero means nothing is selected, which happens while the menu is being
        // rebuilt. It is not a choice the user made, so it must not be written to
        // the parameter -- doing so would reset the waveform to Sine every time a
        // sample was imported.
        if (id <= 0)
            return;

        attachment.setValueAsCompleteGesture ((float) (id - 1));
    }

    juce::ComboBox& combo;
    juce::RangedAudioParameter& parameter;
    juce::ParameterAttachment attachment;
    bool ignoreCallbacks = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WaveformChoiceAttachment)
};

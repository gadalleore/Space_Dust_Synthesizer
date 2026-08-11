#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "SpaceDustLookAndFeel.h"
#include "UserWavetable.h"

/**
    The Waveforms window -- the waveform list, and where a sample joins it.

    Opened by the small button beside each of the three waveform dropdowns. The
    list it shows IS that dropdown's list, in the same order: the built-in shapes
    first, then the eight import slots. Clicking any row selects it, exactly as
    choosing it from the dropdown would.

    That equivalence is deliberate and is held by construction rather than by
    being kept in step by hand. The dropdown's item ids run 1..N in list order and
    the parameter's choices run 0..N-1 in the same order, so a row's position IS
    its item id minus one -- see rowForSelection() and selectRow(). Nothing here
    knows the name of a single waveform.

    A built-in row and an import row differ in one way only: there is nothing to
    import, rename or clear on a built-in, so those four controls go dead while
    one is shown. They are otherwise the same kind of thing and are drawn, chosen
    and previewed the same way.

    Deliberately thin: it decides nothing about audio. Every rule about what a
    dropped file becomes lives in UserWaveLibrary and WaveAnalysis, where it can
    be tested without a window on the screen. This draws what they concluded.
*/
class WaveformEditorComponent : public juce::Component,
                                public juce::FileDragAndDropTarget
{
public:
    WaveformEditorComponent (UserWaveLibrary& library, SpaceDustLookAndFeel& lookAndFeel);
    ~WaveformEditorComponent() override;

    //==========================================================================
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;

    //==========================================================================
    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void fileDragEnter (const juce::StringArray& files, int x, int y) override;
    void fileDragMove (const juce::StringArray& files, int x, int y) override;
    void fileDragExit (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;

    /** Point the window at the dropdown that opened it.

        The window drives that dropdown directly, so choosing a waveform here is
        the same act as choosing it there -- one code path, and no way for the two
        lists to disagree about what is selected.

        userBase is where that dropdown's User entries start, which is also how
        many built-in entries it has in front of them. */
    void setTarget (juce::ComboBox* targetCombo, int userBase);

    /** Show and select the given import slot. */
    void selectSlot (int slotIndex);

    /** Rebuild every control from the library and the target dropdown. */
    void refresh();

    /** Widest the window ever needs to be, so it does not resize when opened from
        a dropdown with a different number of built-in shapes. */
    static int preferredWidth();
    static int preferredHeight();

private:
    //==========================================================================
    void importInto (int slotIndex, const juce::File& file);
    void browseForFile();
    void applyMode (UserWave::Mode mode);

    /** How many rows the list has: built-ins plus the eight import slots. */
    int numRows() const { return userBase + UserWave::numSlots; }

    /** The row the target dropdown currently has selected. */
    int rowForSelection() const;

    /** Select a row, which is the same as choosing it in the dropdown. */
    void selectRow (int row);

    /** The import slot a row refers to, or -1 for a built-in row. */
    int slotForRow (int row) const { return row >= userBase ? row - userBase : -1; }

    /** Which row covers this point, or -1. */
    int rowAt (juce::Point<int> position) const;

    /** Whether a row is drawn at all. Empty import slots are not: there is
        nothing in them to choose, and eight rows reading "empty" told the user
        nothing they could act on. */
    bool isRowVisible (int row) const;

    /** Where a row sits once the hidden ones have been closed up, or -1 if it is
        itself hidden. Rows cannot be positioned by their own index -- that would
        leave a hole wherever an empty slot was skipped. */
    int displayPositionForRow (int row) const;

    int numVisibleRows() const;

    /** Where a dropped file goes when it does not land on an import row: the slot
        being shown, or the first empty one. Never a built-in. */
    int fallbackImportSlot() const;

    juce::Rectangle<int> rowBounds (int row) const;
    juce::Rectangle<int> listBounds() const;
    juce::Rectangle<int> detailBounds() const;

    void paintList (juce::Graphics&);
    void paintDetail (juce::Graphics&);

    /** Draw any row's waveform, built-in or imported, into an area. One function
        for both so the list reads as one kind of thing. */
    void paintRowWaveform (juce::Graphics&, juce::Rectangle<int> area, int row,
                           juce::Colour colour, float thickness, int repeats) const;

    void setStatus (const juce::String& message, bool isError);

    juce::String rowName (int row) const;
    juce::String rowDetail (int row) const;

    UserWaveLibrary& library;
    SpaceDustLookAndFeel& lookAndFeel;

    juce::ComboBox* targetCombo = nullptr;

    /** Built-in entries in front of the import slots: 4 for the oscillators
        (Sine, Triangle, Saw, Square), 2 for the noise source (White, Pink). */
    int userBase = UserWave::oscUserBase;

    /** Import slot the detail panel acts on. Held apart from the row selection so
        that showing a built-in still leaves a sensible target for a dropped file. */
    int activeSlot = 0;

    int dragTargetRow = -1;
    bool dragActive = false;

    juce::GroupComponent listGroup;
    juce::GroupComponent detailGroup;

    juce::TextButton loadButton { "Load File..." };
    juce::TextButton clearButton { "Clear Slot" };
    juce::TextButton singleCycleButton { "Single Cycle" };
    juce::TextButton fullSampleButton { "Full Sample" };
    juce::TextEditor nameEditor;
    juce::Label nameLabel;

    juce::String statusMessage;
    bool statusIsError = false;

    std::unique_ptr<juce::FileChooser> fileChooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WaveformEditorComponent)
};

//==============================================================================
/**
    The window the component lives in.

    Kept alive by the editor and hidden rather than deleted when closed, so the
    selection and the window position survive being shut and reopened.
*/
class WaveformEditorWindow : public juce::DocumentWindow
{
public:
    WaveformEditorWindow (UserWaveLibrary& library, SpaceDustLookAndFeel& lookAndFeel);

    void closeButtonPressed() override { setVisible (false); }

    /** Bring the window up, pointed at the dropdown that asked for it. */
    void showFor (juce::ComboBox* targetCombo, int userBase, int slotIndex);

    /** Redraw after the library changed under it. */
    void refreshContent();

private:
    /** Let a file dragged out of Explorer reach this window even when the host is
        running with raised privileges.

        Windows blocks messages sent from a lower privilege level to a higher one,
        and a file drop is such a message. Without this, drag and drop silently
        does nothing in an elevated DAW -- no error, no cursor change, nothing --
        while the Load File button keeps working, which is a confusing pair of
        symptoms to be handed. Does nothing on any other platform, and nothing on
        Windows when the process is not elevated. */
    void allowFileDropsFromLowerPrivilege();

    WaveformEditorComponent* content = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WaveformEditorWindow)
};

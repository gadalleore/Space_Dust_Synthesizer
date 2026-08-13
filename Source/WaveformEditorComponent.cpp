#include "WaveformEditorComponent.h"

#include "WaveAnalysis.h"

#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX 1
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN 1
 #endif
 #include <windows.h>
#endif

namespace
{
    // Straight from the main panel, so the window reads as part of the same
    // instrument rather than as a utility that happens to be attached to it.
    const juce::Colour backgroundNavy { 0xff0a0a1f };
    const juce::Colour rowDropTarget  { 0xff1e4a5e };

    // The toggle's own colours, so a row and a toggle are the same object drawn
    // twice rather than two things that merely look similar. Taken from
    // drawToggleStyleButton; if that changes, these follow.
    const juce::Colour toggleNavy      { 0xff1a1a2f };
    const juce::Colour toggleBorder    { 0xff3a3a5f };
    const juce::Colour toggleLitNavy   { 0xff1a4a5f };
    const juce::Colour toggleLitBorder { 0xff00b4ff };
    const juce::Colour labelCyan      { 0xffa0d8ff };
    const juce::Colour valueCyan      { 0xff6dd5fa };
    const juce::Colour knobArcCyan    { 0xff00d4ff };
    const juce::Colour dimText        { 0xff6a7a99 };
    const juce::Colour warningAmber   { 0xffffc266 };
    const juce::Colour errorRed       { 0xffff8080 };

    constexpr int margin = 10;
    constexpr int listWidth = 240;
    constexpr int detailWidth = 400;
    constexpr int rowHeight = 32;
    constexpr int groupTitleInset = 26;
    constexpr int groupPadding = 10;
    constexpr int statusHeight = 20;

    /** Button size, taken from the main panel: every toggle there is 22 high and
        between 86 and 130 wide. A utility window that used its own sizes was the
        giveaway that it was a utility window. */
    constexpr int buttonHeight = 22;
    constexpr int buttonWidth = 90;
    constexpr int buttonGap = 6;

    /** Height of the two control rows plus the gap between them. */
    constexpr int controlStripHeight = buttonHeight * 2 + 8 + 4;

    /** The list area is always tall enough for every row THIS list could hold, so
        it does not change size as waveforms are imported and cleared. The space
        under the last row is not wasted -- it is the drop zone. */
    int maxRowsFor (int userBase)
    {
        return userBase + UserWave::numSlots;
    }

    juce::String modeName (UserWave::Mode mode)
    {
        return mode == UserWave::Mode::SingleCycle ? "Single Cycle" : "Full Sample";
    }

    /** The built-in shapes, for drawing only.

        These mirror the switch in SynthVoice::generateWaveform. They are written
        out again here rather than shared with it because the voice's version is
        the hot path -- up to four times per sample per oscillator -- and because
        what is wanted here is a picture, not a signal: no band limiting and no
        phase, just the outline, at a size where a few pixels could not be seen. */
    float builtInShapeValue (int shape, double phase01)
    {
        constexpr double twoPi = 6.283185307179586;

        switch (shape)
        {
            case 0: return (float) std::sin (twoPi * phase01);
            case 1: return (float) (4.0 * std::abs (phase01 - std::floor (phase01 + 0.5)) - 1.0);
            case 2: return (float) (2.0 * (phase01 - std::floor (phase01 + 0.5)));
            case 3: return std::sin (twoPi * phase01) > 0.0 ? 1.0f : -1.0f;
            default: return 0.0f;
        }
    }

    /** How loud a drum is, as a fraction of the way through its hit.

        The ten built-in drums have no waveform to draw and no two of them look
        alike, so the row shows the one thing they all are: a burst that starts
        at full and falls away. Drawing a specific shape would be a guess; this
        is what a scope would show of any of them at this size. */
    float drumEnvelopeValue (double across)
    {
        return (float) std::exp (-4.5 * juce::jlimit (0.0, 1.0, across));
    }
}

//==============================================================================
int WaveformEditorComponent::preferredWidth()
{
    return margin + listWidth + margin + detailWidth + margin;
}

int WaveformEditorComponent::preferredHeight (int userBase)
{
    return margin + groupTitleInset + maxRowsFor (userBase) * rowHeight + groupPadding
         + margin + statusHeight + margin;
}

//==============================================================================
WaveformEditorComponent::WaveformEditorComponent (UserWaveLibrary& libraryToUse,
                                                  SpaceDustLookAndFeel& lookAndFeelToUse)
    : library (libraryToUse), lookAndFeel (lookAndFeelToUse)
{
    setSize (preferredWidth(), preferredHeight (userBase));

    // One call and every button, editor and group in here is drawn by the same
    // code that draws the main panel -- same fonts, same cyan, same rounded
    // boxes. Anything styled by hand below is only what the plugin styles by hand
    // on its own panel too.
    setLookAndFeel (&lookAndFeel);

    listGroup.setText ("Waveforms");
    detailGroup.setText ("Detail");

    // The property every group box on the main panel sets. It is what gives them
    // their cyan boundary and the inward glow behind it; without it a box falls
    // back to JUCE's default outline, which is the grey that made this window
    // look like it came from a different plugin (Giuseppe, 2026-08-12).
    listGroup.getProperties().set ("viewportGlow", true);
    detailGroup.getProperties().set ("viewportGlow", true);

    addAndMakeVisible (listGroup);
    addAndMakeVisible (detailGroup);

    loadButton.setButtonText ("Load File...");
    clearButton.setButtonText ("Clear Slot");
    updateAllButton.setButtonText ("Update All");
    singleCycleButton.setButtonText ("Single Cycle");
    fullSampleButton.setButtonText ("Full Sample");

    addAndMakeVisible (loadButton);
    addAndMakeVisible (clearButton);
    addAndMakeVisible (updateAllButton);
    addAndMakeVisible (singleCycleButton);
    addAndMakeVisible (fullSampleButton);
    addAndMakeVisible (nameEditor);
    addAndMakeVisible (nameLabel);

    loadButton.onClick = [this] { browseForFile(); };

    clearButton.onClick = [this]
    {
        const int slot = activeSlot;
        const int row = userBase + slot;

        // Move the selection off the row BEFORE the slot is emptied, not after.
        //
        // The dropdown only lists slots that hold something, so a cleared slot
        // has no entry left to sit on -- and the parameter would still be
        // pointing at it. That is what used to leave "User n (missing)" in the
        // menu. Stepping up to the row above first means that by the time the
        // menus are rebuilt the parameter already names something real, and the
        // cleared entry simply goes.
        //
        // The row above, not the slot above: an empty slot in between is not
        // drawn, so what the player sees above this row may be an earlier slot
        // or the last built-in shape. Row 0 is always a built-in, so the search
        // below always finds one.
        if (targetCombo != nullptr && targetCombo->getSelectedId() == row + 1)
        {
            for (int above = row - 1; above >= 0; --above)
            {
                if (isRowVisible (above))
                {
                    selectRow (above);
                    break;
                }
            }
        }

        library.clearSlot (group, slot);
        setStatus ("Slot " + juce::String (slot + 1) + " cleared from "
                   + UserWave::groupName (group) + ".", false);
        refresh();
    };

    updateAllButton.setTooltip ("Put this waveform into the same slot of Waveform 1, "
                               "Waveform 2, Sub, Noize and Transient");

    updateAllButton.onClick = [this]
    {
        const int slot = activeSlot;

        // Read the name before the copy, not after: the slot itself is not moved
        // and keeps its name, but reading it first makes that independent of what
        // the copy does.
        const auto name = library.bank().slot (group, slot).name;

        juce::String error;

        if (! library.copySlotToAllGroups (group, slot, error))
            setStatus (error, true);
        else
            setStatus ("\"" + name + "\" is now slot " + juce::String (slot + 1)
                       + " of Waveform 1, Waveform 2, Sub, Noize and Transient.", false);

        refresh();
    };

    singleCycleButton.onClick = [this] { applyMode (UserWave::Mode::SingleCycle); };
    fullSampleButton.onClick  = [this] { applyMode (UserWave::Mode::FullSample); };

    // The two mode buttons are one control with two positions. Drawn as two lit
    // or unlit toggles rather than joined into a single box: that is how every
    // pair of exclusive choices reads on the main panel, and joining them was
    // the one place in the plugin where a button had square edges.
    singleCycleButton.setClickingTogglesState (true);
    fullSampleButton.setClickingTogglesState (true);
    singleCycleButton.setRadioGroupId (1);
    fullSampleButton.setRadioGroupId (1);

    nameLabel.setText ("Name", juce::dontSendNotification);
    nameLabel.setFont (lookAndFeel.getBodyFont (12.0f, true));
    nameLabel.setColour (juce::Label::textColourId, labelCyan);
    nameLabel.setJustificationType (juce::Justification::centredLeft);

    nameEditor.setFont (lookAndFeel.getBodyFont (13.0f, false));
    nameEditor.setTextToShowWhenEmpty ("waveform name", dimText);
    // The dropdowns' colours, from the LookAndFeel's constructor. A box you type
    // into and a box you choose from should not be two different greys.
    nameEditor.setColour (juce::TextEditor::backgroundColourId, toggleNavy);
    nameEditor.setColour (juce::TextEditor::textColourId, labelCyan);
    nameEditor.setColour (juce::TextEditor::outlineColourId, toggleBorder);
    nameEditor.setColour (juce::TextEditor::focusedOutlineColourId, knobArcCyan);

    nameEditor.onReturnKey = [this]
    {
        library.renameSlot (group, activeSlot, nameEditor.getText());
        refresh();
    };
    nameEditor.onFocusLost = [this]
    {
        library.renameSlot (group, activeSlot, nameEditor.getText());
        refresh();
    };

    // No colours set on the buttons. They paint themselves through the same
    // routine that draws every toggle on the main panel, so their navy, their
    // border and their bloom all come from that one place.

    refresh();
}

WaveformEditorComponent::~WaveformEditorComponent()
{
    setLookAndFeel (nullptr);
}

//==============================================================================
void WaveformEditorComponent::setTarget (juce::ComboBox* combo, int base, BuiltInKind kind,
                                         UserWave::Group groupToShow)
{
    targetCombo = combo;
    userBase = base;
    builtInKind = kind;
    group = groupToShow;

    // Named, because the five lists are separate and what is on screen is one of
    // them. Without this the window looks the same whichever button opened it,
    // and a slot loaded into the wrong list is invisible until it is played.
    //
    // The name alone, with no "Waveforms" after it: three of the five are called
    // Waveform 1, Waveform 2 and Sub, and appending the word gave "Waveform 2
    // Waveforms". The rows are plainly waveforms without being told.
    listGroup.setText (UserWave::groupName (group));

    // A list with more built-ins needs more room for them. The window follows
    // this; see WaveformEditorWindow::showFor.
    setSize (preferredWidth(), preferredHeight (userBase));

    refresh();
}

void WaveformEditorComponent::selectSlot (int slotIndex)
{
    activeSlot = juce::jlimit (0, UserWave::numSlots - 1, slotIndex);

    // Only move the selection onto the slot if there is something in it. Choosing
    // an empty slot would silence the oscillator, and opening a window must never
    // change the sound by itself.
    if (library.bank().slot (group, activeSlot).isPlayable() && targetCombo != nullptr)
        targetCombo->setSelectedId (userBase + activeSlot + 1, juce::sendNotificationSync);

    refresh();
}

//==============================================================================
int WaveformEditorComponent::rowForSelection() const
{
    if (targetCombo == nullptr)
        return 0;

    const int selectedRow = targetCombo->getSelectedId() - 1;

    // A row exists for every built-in and every FILLED slot. Empty slots are not
    // drawn, so a selection pointing at one has no row to highlight.
    if (selectedRow < 0 || selectedRow >= numRows())
        return -1;

    const int slot = slotForRow (selectedRow);

    if (slot >= 0 && ! library.bank().slot (group, slot).isPlayable())
        return -1;

    return selectedRow;
}

void WaveformEditorComponent::selectRow (int row)
{
    if (row < 0 || row >= numRows() || targetCombo == nullptr)
        return;

    // Choosing a row IS choosing it in the dropdown. Driving the ComboBox rather
    // than the parameter keeps the attachment, the automation and the undo
    // history behaving exactly as if the user had used the menu.
    // Sync, not async: the point of clicking a row is to hear it at once.
    targetCombo->setSelectedId (row + 1, juce::sendNotificationSync);

    const int slot = slotForRow (row);

    if (slot >= 0)
        activeSlot = slot;

    refresh();
}

int WaveformEditorComponent::fallbackImportSlot() const
{
    for (int i = 0; i < UserWave::numSlots; ++i)
        if (! library.bank().slot (group, i).isPlayable())
            return i;

    // Every slot is full, so a drop replaces whichever one is on screen.
    return activeSlot;
}

//==============================================================================
juce::String WaveformEditorComponent::rowName (int row) const
{
    const int slot = slotForRow (row);

    if (slot < 0)
    {
        // The built-in names come from the dropdown itself, so this window can
        // never disagree with the menu about what anything is called. Looked up
        // by ID, not by position: the menu leaves empty slots out, so an entry's
        // position there is not its position here.
        if (targetCombo != nullptr)
        {
            const int index = targetCombo->indexOfItemId (row + 1);

            if (index >= 0)
                return targetCombo->getItemText (index);
        }

        return {};
    }

    return library.choiceNameForSlot (group, slot);
}

juce::String WaveformEditorComponent::rowDetail (int row) const
{
    const int slot = slotForRow (row);

    if (slot < 0)
        return "Built in";

    const auto& entry = library.bank().slot (group, slot);

    if (! entry.isPlayable())
        return "empty";

    juce::String detail = modeName (entry.mode);

    if (entry.pitchLabel.isNotEmpty())
        detail += "   " + entry.pitchLabel;

    if (entry.mode == UserWave::Mode::FullSample && ! entry.retuned)
        detail += "   not tuned";

    return detail;
}

//==============================================================================
void WaveformEditorComponent::refresh()
{
    const int row = rowForSelection();
    const int slot = row >= 0 ? slotForRow (row) : -1;

    if (slot >= 0)
        activeSlot = slot;

    const auto& entry = library.bank().slot (group, activeSlot);

    // A built-in has nothing to rename, clear, re-import or switch modes on, so
    // those controls go dead while one is shown. It is the only way the two kinds
    // of row differ.
    const bool showingSlot = (slot >= 0);
    const bool editable = showingSlot && entry.isPlayable();

    nameEditor.setText (editable ? entry.name : juce::String(), juce::dontSendNotification);
    nameEditor.setEnabled (editable);
    nameLabel.setEnabled (editable);
    clearButton.setEnabled (editable);
    updateAllButton.setEnabled (editable);
    singleCycleButton.setEnabled (editable);
    fullSampleButton.setEnabled (editable);

    singleCycleButton.setToggleState (editable && entry.mode == UserWave::Mode::SingleCycle,
                                      juce::dontSendNotification);
    fullSampleButton.setToggleState (editable && entry.mode == UserWave::Mode::FullSample,
                                     juce::dontSendNotification);

    detailGroup.setText (row >= 0 ? rowName (row) : juce::String ("Detail"));

    repaint();
}

//==============================================================================
void WaveformEditorComponent::setStatus (const juce::String& message, bool isError)
{
    statusMessage = message;
    statusIsError = isError;
    repaint();
}

void WaveformEditorComponent::importInto (int slotIndex, const juce::File& file)
{
    juce::String error;

    // Keep whichever mode the slot is already in, so replacing the sample in a
    // Full Sample slot does not quietly turn it back into a single cycle.
    const auto& existing = library.bank().slot (group, slotIndex);
    const auto mode = existing.isPlayable() ? existing.mode : UserWave::Mode::SingleCycle;

    // Reading the file and building eleven tables takes long enough on a long
    // sample to look like a hang without this.
    juce::MouseCursor::showWaitCursor();
    const bool ok = library.importFile (file, group, slotIndex, mode, error);
    juce::MouseCursor::hideWaitCursor();

    if (! ok)
    {
        setStatus (error, true);
        refresh();
        return;
    }

    activeSlot = slotIndex;

    // Play it at once. Importing a sample and then having to go and find it in a
    // menu before hearing it is a step with no purpose.
    if (targetCombo != nullptr)
        targetCombo->setSelectedId (userBase + slotIndex + 1, juce::sendNotificationSync);

    const auto& slot = library.bank().slot (group, slotIndex);
    juce::String message = "Loaded \"" + slot.name + "\", " + slot.pitchLabel;

    if (slot.mode == UserWave::Mode::FullSample && ! slot.retuned)
        message += ". No clear pitch found, so middle C plays it at its recorded speed.";
    else
        message += ". Middle C now plays it in tune.";

    setStatus (message, false);
    refresh();
}

void WaveformEditorComponent::browseForFile()
{
    const int slot = fallbackImportSlot();

    fileChooser = std::make_unique<juce::FileChooser> (
        "Choose an audio file for slot " + juce::String (slot + 1),
        juce::File::getSpecialLocation (juce::File::userMusicDirectory),
        "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3");

    fileChooser->launchAsync (juce::FileBrowserComponent::openMode
                              | juce::FileBrowserComponent::canSelectFiles,
        [this, slot] (const juce::FileChooser& chooser)
        {
            const auto file = chooser.getResult();

            if (file.existsAsFile())
                importInto (slot, file);
        });
}

void WaveformEditorComponent::applyMode (UserWave::Mode mode)
{
    juce::String error;

    if (! library.setSlotMode (group, activeSlot, mode, error))
        setStatus (error, true);
    else
        setStatus (modeName (mode) + ": " + (mode == UserWave::Mode::SingleCycle
                       ? juce::String ("one period of the sample, played as an oscillator.")
                       : juce::String ("the whole file, looped, tuned to the note you play.")),
                   false);

    refresh();
}

//==============================================================================
bool WaveformEditorComponent::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (const auto& path : files)
    {
        const auto extension = juce::File (path).getFileExtension().toLowerCase();

        if (extension == ".wav" || extension == ".aif" || extension == ".aiff"
            || extension == ".flac" || extension == ".ogg" || extension == ".mp3")
            return true;
    }

    return false;
}

void WaveformEditorComponent::fileDragEnter (const juce::StringArray&, int x, int y)
{
    dragActive = true;
    dragTargetRow = rowAt ({ x, y });
    repaint();
}

void WaveformEditorComponent::fileDragMove (const juce::StringArray&, int x, int y)
{
    const int row = rowAt ({ x, y });

    if (row != dragTargetRow)
    {
        dragTargetRow = row;
        repaint();
    }
}

void WaveformEditorComponent::fileDragExit (const juce::StringArray&)
{
    dragActive = false;
    dragTargetRow = -1;
    repaint();
}

void WaveformEditorComponent::filesDropped (const juce::StringArray& files, int x, int y)
{
    const int row = rowAt ({ x, y });

    dragActive = false;
    dragTargetRow = -1;

    if (files.isEmpty())
    {
        repaint();
        return;
    }

    // A drop on an existing sample replaces it. A drop anywhere else -- the empty
    // space under the list, the detail panel, a built-in row -- adds it to the
    // first free slot, which is what the drop zone says it will do.
    const int slot = slotForRow (row);

    importInto (slot >= 0 ? slot : fallbackImportSlot(), juce::File (files[0]));
}

//==============================================================================
void WaveformEditorComponent::mouseDown (const juce::MouseEvent& event)
{
    const int row = rowAt (event.getPosition());

    if (row >= 0)
        selectRow (row);
}

//==============================================================================
juce::Rectangle<int> WaveformEditorComponent::listBounds() const
{
    return { margin, margin, listWidth,
             getHeight() - margin - statusHeight - 2 * margin };
}

juce::Rectangle<int> WaveformEditorComponent::detailBounds() const
{
    auto list = listBounds();
    return { list.getRight() + margin, list.getY(), detailWidth, list.getHeight() };
}

bool WaveformEditorComponent::isRowVisible (int row) const
{
    const int slot = slotForRow (row);

    return slot < 0 || library.bank().slot (group, slot).isPlayable();
}

int WaveformEditorComponent::displayPositionForRow (int row) const
{
    if (row < 0 || row >= numRows() || ! isRowVisible (row))
        return -1;

    int position = 0;

    for (int r = 0; r < row; ++r)
        if (isRowVisible (r))
            ++position;

    return position;
}

int WaveformEditorComponent::numVisibleRows() const
{
    int count = 0;

    for (int row = 0; row < numRows(); ++row)
        if (isRowVisible (row))
            ++count;

    return count;
}

juce::Rectangle<int> WaveformEditorComponent::rowBounds (int row) const
{
    const int position = displayPositionForRow (row);

    if (position < 0)
        return {};

    auto list = listBounds().reduced (groupPadding, 0);

    return { list.getX(), list.getY() + groupTitleInset + position * rowHeight,
             list.getWidth(), rowHeight };
}

int WaveformEditorComponent::rowAt (juce::Point<int> position) const
{
    for (int row = 0; row < numRows(); ++row)
        if (isRowVisible (row) && rowBounds (row).contains (position))
            return row;

    return -1;
}

//==============================================================================
void WaveformEditorComponent::resized()
{
    listGroup.setBounds (listBounds());
    detailGroup.setBounds (detailBounds());

    auto detail = detailBounds().reduced (groupPadding, 0);
    detail.removeFromTop (groupTitleInset);
    detail.removeFromBottom (groupPadding);

    // Two rows of controls. One row could not hold the name box at a width worth
    // typing into once the four buttons had taken what they need.
    auto controls = detail.removeFromBottom (controlStripHeight);

    auto upper = controls.removeFromTop (buttonHeight);
    auto modes = upper.removeFromLeft (buttonWidth * 2 + buttonGap);
    singleCycleButton.setBounds (modes.removeFromLeft (buttonWidth));
    modes.removeFromLeft (buttonGap);  // the two are separate toggles now, not one joined box
    fullSampleButton.setBounds (modes);

    clearButton.setBounds (upper.removeFromRight (buttonWidth));
    upper.removeFromRight (buttonGap);
    loadButton.setBounds (upper.removeFromRight (buttonWidth));

    controls.removeFromTop (8);

    // Update All sits on the lower row rather than beside Clear Slot: the upper
    // row is already full, and the name box has width to spare. Same height as
    // every other button, so the five read as one set.
    auto lower = controls.removeFromTop (buttonHeight);
    updateAllButton.setBounds (lower.removeFromRight (buttonWidth));
    lower.removeFromRight (buttonGap);
    nameLabel.setBounds (lower.removeFromLeft (42));
    nameEditor.setBounds (lower);
}

//==============================================================================
void WaveformEditorComponent::paint (juce::Graphics& g)
{
    // The same background as the main panel, not a flat fill: the same navy, and
    // then the same sky over it, breathing with the output by the same law. This
    // window is part of the instrument and has to look like it.
    g.fillAll (backgroundNavy);
    SpaceDustLookAndFeel::drawStarfield (g, getWidth(), getHeight(),
                                         lookAndFeel.getOutputMeterLevel());

    paintList (g);
    paintDetail (g);

    auto status = juce::Rectangle<int> (margin + 4, getHeight() - statusHeight - margin,
                                        getWidth() - 2 * margin - 8, statusHeight);

    g.setFont (lookAndFeel.getBodyFont (12.0f, false));
    g.setColour (statusIsError ? errorRed : dimText);
    g.drawText (statusMessage, status, juce::Justification::centredLeft, true);
}

//==============================================================================
void WaveformEditorComponent::paintRowWaveform (juce::Graphics& g, juce::Rectangle<int> area,
                                                int row, juce::Colour colour, float thickness,
                                                int repeats, Bloom bloom) const
{
    if (area.getWidth() < 4 || area.getHeight() < 4)
        return;

    const float centreY = (float) area.getCentreY();
    const float halfHeight = (float) area.getHeight() * 0.42f;
    const int slot = slotForRow (row);

    // Every picture is built as a PATH and then stroked once at the end, rather
    // than each kind drawing itself. That is what lets all three carry the same
    // bloom: glowTrace widens a path, and there is nothing to widen behind a
    // drawLine. It is also why the scatter and the drum burst are subpaths.
    juce::Path path;
    float strokeThickness = thickness;

    //==========================================================================
    // Noise has no repeating shape. Drawing one would be a lie, so it gets a
    // scatter that reads as noise at a glance instead.
    if (slot < 0 && builtInKind == BuiltInKind::Noise)
    {
        juce::Random random (row == 0 ? 1234 : 5678);

        for (int x = 0; x < area.getWidth(); x += 2)
        {
            // Pink noise holds more of its energy low down, so its scatter is
            // drawn shorter -- the same difference a scope would show.
            const float spread = (row == 0) ? 1.0f : 0.5f;
            const float value = (random.nextFloat() * 2.0f - 1.0f) * spread;
            const float px = (float) (area.getX() + x);

            path.startNewSubPath (px, centreY);
            path.lineTo (px, centreY - value * halfHeight);
        }

        // Always tight, whatever the caller asked for: this is a field of short
        // separate strokes, and the wide spread turns it into one lit block.
        strokeWaveformPath (g, path, colour, strokeThickness, Bloom::Tight);
        return;
    }

    //==========================================================================
    // A drum has no repeating shape either. It gets the outline of a hit: full
    // at the front, gone by the end. See drumEnvelopeValue.
    if (slot < 0 && builtInKind == BuiltInKind::Drums)
    {
        const int width = area.getWidth();
        strokeThickness = thickness * 0.7f;

        for (int x = 0; x < width; ++x)
        {
            const double across = (double) x / (double) juce::jmax (1, width - 1);
            const float value = drumEnvelopeValue (across) * halfHeight;
            const float px = (float) (area.getX() + x);

            path.startNewSubPath (px, centreY - value);
            path.lineTo (px, centreY + value);
        }

        // Tight for the same reason as the noise scatter above: a column per
        // pixel, so a wide halo fills the gaps between them rather than ringing
        // the shape.
        strokeWaveformPath (g, path, colour, strokeThickness, Bloom::Tight);
        return;
    }

    const UserWaveSlot* entry = slot >= 0 ? &library.bank().slot (group, slot) : nullptr;

    if (entry != nullptr && ! entry->isPlayable())
        return;

    const int width = area.getWidth();

    for (int x = 0; x < width; ++x)
    {
        const double across = (double) x / (double) (width - 1);
        float value = 0.0f;

        if (entry == nullptr)
        {
            value = builtInShapeValue (row, across * repeats);
        }
        else if (entry->mode == UserWave::Mode::SingleCycle)
        {
            // Drawn at full bandwidth, so the shape on screen is the shape that
            // was imported and not the reduced version a high note would play.
            if (! entry->tables.empty())
            {
                double phase = across * repeats;
                phase -= std::floor (phase);

                const int index = juce::jlimit (0, WaveAnalysis::tableSize - 1,
                                                (int) (phase * WaveAnalysis::tableSize));
                value = entry->tables[(std::size_t) index];
            }
        }
        else if (! entry->sample.empty())
        {
            // A whole file at one pixel per column would alias badly, so each
            // column shows the loudest point in the span it covers. That is what
            // makes the envelope of a sample readable at this size.
            const double total = (double) entry->sample.size();
            const int from = juce::jlimit (0, (int) entry->sample.size() - 1, (int) (across * total));
            const int to = juce::jlimit (from + 1, (int) entry->sample.size(),
                                         (int) ((across + 1.0 / width) * total));

            for (int i = from; i < to; ++i)
                if (std::abs (entry->sample[(std::size_t) i]) > std::abs (value))
                    value = entry->sample[(std::size_t) i];
        }

        const float y = centreY - value * halfHeight;
        const float px = (float) (area.getX() + x);

        if (x == 0)
            path.startNewSubPath (px, y);
        else
            path.lineTo (px, y);
    }

    strokeWaveformPath (g, path, colour, strokeThickness, bloom);
}

juce::Colour WaveformEditorComponent::traceColour() const
{
    // The knob-arc cyan, which is the Final EQ curve's colour -- not the scopes'
    // trace blue. The two are close but not the same, and the EQ curve is the
    // line this window is meant to match.
    return lookAndFeel.getMeterResponsiveKnobArcColour();
}

void WaveformEditorComponent::strokeWaveformPath (juce::Graphics& g, const juce::Path& path,
                                                  juce::Colour colour, float thickness,
                                                  Bloom bloom) const
{
    if (path.isEmpty())
        return;

    // Drawn as the Final EQ's response curve is drawn, because that is the line
    // in this plugin that lights up (Giuseppe, 2026-08-12), and a waveform here
    // should behave the same way.
    //
    // Two things make that curve what it is, and both are copied:
    //
    //   no halo at rest      the bloom is the meter's and nothing else, so the
    //                        line genuinely lights UP as sound arrives instead
    //                        of sitting permanently lit. An earlier cut of this
    //                        had an always-on halo underneath; it swamped the
    //                        part that moves.
    //
    //   curved and rounded   the same stroke type, so corners in a sampled
    //                        waveform read like the EQ's curve rather than like
    //                        mitred spikes.
    //
    // The spread is the one thing NOT copied everywhere -- see Bloom. The EQ is
    // a single curve in a large box, and this window can have nineteen pictures
    // on screen at once; all of them at the EQ's width was more light than the
    // EQ has ever thrown.
    if (bloom == Bloom::Wide)
        lookAndFeel.glowPath (g, path, colour, thickness);
    else
        lookAndFeel.glowTrace (g, path, colour, thickness);

    g.setColour (colour.withMultipliedAlpha (0.9f));
    g.strokePath (path, juce::PathStrokeType (thickness, juce::PathStrokeType::curved,
                                              juce::PathStrokeType::rounded));
}

//==============================================================================
void WaveformEditorComponent::paintList (juce::Graphics& g)
{
    const int selected = rowForSelection();

    for (int row = 0; row < numRows(); ++row)
    {
        // Empty slots are not shown at all. There is nothing in them to choose,
        // and eight rows of the word "empty" told the user nothing.
        if (! isRowVisible (row))
            continue;

        const int slot = slotForRow (row);
        auto bounds = rowBounds (row).reduced (0, 2);
        const bool isDropTarget = dragActive && dragTargetRow == row && slot >= 0;
        const bool isSelected = (row == selected);

        // A row is drawn as a toggle, because that is what it is: a choice that
        // is either the one in force or not. Same navy, same border, same lit
        // fill and same meter-driven bloom as every toggle on the main panel --
        // which is why the colours below are the toggle's, not this window's.
        const auto rowCorner = 4.0f;
        const bool lit = isSelected || isDropTarget;

        if (lit)
            lookAndFeel.glowAround (g, bounds.toFloat().expanded (2.0f), rowCorner + 1.0f,
                                    isDropTarget ? knobArcCyan
                                                 : lookAndFeel.getMeterResponsiveKnobArcColour());

        g.setColour (isDropTarget ? rowDropTarget : (isSelected ? toggleLitNavy : toggleNavy));
        g.fillRoundedRectangle (bounds.toFloat(), rowCorner);

        g.setColour (isDropTarget ? knobArcCyan
                                  : (isSelected ? toggleLitBorder : toggleBorder));
        g.drawRoundedRectangle (bounds.toFloat().reduced (0.5f), rowCorner,
                                lit ? 1.5f : 1.0f);

        auto content = bounds.reduced (8, 4);

        // The same picture beside every entry, built-in or imported, because the
        // whole point of the list is that they are the same kind of thing.
        //
        // One hue for every row, at two weights: the scopes' blue, dropped back
        // on the rows that are not selected. It used to be cyan when selected
        // and a flat grey otherwise, which read as two different kinds of line.
        // Tight: eighteen of these can be on screen together, and the wide spread
        // that suits the single big picture below adds up to a glare here.
        paintRowWaveform (g, content.removeFromLeft (38), row,
                          isSelected ? traceColour() : traceColour().withAlpha (0.45f),
                          1.2f, 2, Bloom::Tight);

        content.removeFromLeft (10);

        auto nameRow = content.removeFromTop (content.getHeight() / 2 + 1);

        g.setColour (isSelected ? juce::Colours::white : labelCyan);
        g.setFont (lookAndFeel.getBodyFont (12.0f, true));
        g.drawText (rowName (row), nameRow, juce::Justification::bottomLeft, true);

        g.setColour (isSelected ? valueCyan : dimText);
        g.setFont (lookAndFeel.getBodyFont (10.5f, false));
        g.drawText (rowDetail (row), content, juce::Justification::topLeft, true);
    }

    //==========================================================================
    // Everything below the last row is the drop zone. Hiding the empty slots left
    // this space behind, and it is exactly the affordance their absence removed:
    // somewhere to aim a file, that says where the file will land.
    const bool hasRoom = ! library.bank().slot (group, fallbackImportSlot()).isPlayable();

    auto list = listBounds().reduced (groupPadding, 0);

    auto zone = juce::Rectangle<int> (list.getX(),
                                      list.getY() + groupTitleInset + numVisibleRows() * rowHeight,
                                      list.getWidth(), rowHeight * 2).reduced (0, 4);

    if (zone.getBottom() > listBounds().getBottom() - groupPadding)
        zone.setBottom (listBounds().getBottom() - groupPadding);

    if (zone.getHeight() < 28)
        return;

    const bool zoneIsTarget = dragActive && dragTargetRow < 0;

    juce::Path dashed;
    dashed.addRoundedRectangle (zone.toFloat().reduced (1.0f), 4.0f);

    const float dashes[] = { 4.0f, 4.0f };
    juce::Path strokedDashes;
    juce::PathStrokeType (zoneIsTarget ? 2.0f : 1.0f).createDashedStroke (strokedDashes, dashed,
                                                                          dashes, 2);

    g.setColour (zoneIsTarget ? knobArcCyan : toggleBorder);
    g.fillPath (strokedDashes);

    g.setColour (zoneIsTarget ? knobArcCyan : dimText);
    g.setFont (lookAndFeel.getBodyFont (11.5f, false));
    g.drawFittedText (hasRoom ? "Drop an audio file here to add a waveform"
                              : "All eight slots are full. Drop on one to replace it.",
                      zone.reduced (8, 4), juce::Justification::centred, 2);
}

//==============================================================================
void WaveformEditorComponent::paintDetail (juce::Graphics& g)
{
    auto content = detailBounds().reduced (groupPadding, 0);
    content.removeFromTop (groupTitleInset);
    content.removeFromBottom (groupPadding);
    content.removeFromBottom (controlStripHeight);   // the strip laid out in resized()

    const int row = rowForSelection();

    if (row < 0)
    {
        g.setColour (dimText);
        g.setFont (lookAndFeel.getBodyFont (13.0f, false));
        g.drawFittedText ("Choose a waveform from the list.\n\n"
                          "Drop an audio file to add your own.",
                          content, juce::Justification::centred, 4);
        return;
    }

    const int slot = slotForRow (row);
    const UserWaveSlot* entry = slot >= 0 ? &library.bank().slot (group, slot) : nullptr;

    //==========================================================================
    auto readout = content.removeFromTop (24);

    juce::String detail;
    juce::String note;
    juce::Colour noteColour = dimText;

    if (entry == nullptr)
    {
        detail = "Built in, always available";

        switch (builtInKind)
        {
            case BuiltInKind::Noise:
                note = "One of the two noise colours. Nothing to import or change here.";
                break;

            case BuiltInKind::Drums:
                note = "One of the ten synthesised drums. Nothing to import or change here.";
                break;

            case BuiltInKind::Shapes:
            default:
                note = "One of the four basic shapes. Always in tune, at every pitch.";
                break;
        }
    }
    else
    {
        detail << juce::String (entry->fundamentalHz, 2) << " Hz";

        if (entry->pitchLabel.isNotEmpty())
            detail << "   (" << entry->pitchLabel << ")";

        detail << "   " << modeName (entry->mode);

        // A low-confidence pitch reading is the one thing the player must be told
        // about, so it gets a line of its own rather than a number to interpret.
        if (entry->mode == UserWave::Mode::SingleCycle)
        {
            note = "One period of the sample. Every key plays it at its own pitch.";
        }
        else if (entry->retuned)
        {
            note = "Tuned: middle C plays this at concert pitch, so it sits in the "
                   "song without transposing anything.";
        }
        else
        {
            note = "No clear pitch was found, so this is NOT re-tuned. Middle C "
                   "plays it at its recorded speed.";
            noteColour = warningAmber;
        }
    }

    g.setColour (valueCyan);
    g.setFont (lookAndFeel.getBodyFont (12.5f, false));
    g.drawText (detail, readout, juce::Justification::centredLeft, true);

    content.removeFromTop (4);

    auto noteArea = content.removeFromBottom (32);

    g.setColour (noteColour);
    g.setFont (lookAndFeel.getBodyFont (11.5f, false));
    g.drawFittedText (note, noteArea, juce::Justification::centredLeft, 2);

    //==========================================================================
    // The oscilloscope's own background and the panel's border, so the box a
    // waveform is drawn in here and the box one is drawn in on the Spectral page
    // are the same box.
    g.setColour (backgroundNavy);
    g.fillRoundedRectangle (content.toFloat(), 4.0f);

    g.setColour (toggleBorder);
    g.drawRoundedRectangle (content.toFloat(), 4.0f, 1.0f);
    g.drawHorizontalLine (content.getCentreY(), (float) content.getX() + 2.0f,
                          (float) content.getRight() - 2.0f);

    // Two periods for a cycle so it reads as repeating; one pass for a whole
    // sample, because that is the thing itself.
    const bool wholeSample = (entry != nullptr && entry->mode == UserWave::Mode::FullSample);

    // The EQ curve's own weight and its own spread -- one open curve alone in a
    // box, which is exactly what that treatment was drawn for.
    paintRowWaveform (g, content.reduced (6, 8), row, traceColour(), 1.8f,
                      wholeSample ? 1 : 2, Bloom::Wide);

    //==========================================================================
    // Mark the loop: the ends of a file are reserved for the crossfade, so what
    // plays is not quite all of what was imported.
    if (entry != nullptr && wholeSample && entry->loopLength > 0 && ! entry->sample.empty())
    {
        auto area = content.reduced (6, 8);
        const double total = (double) entry->sample.size();

        const auto markerX = [&] (int position)
        {
            return (int) ((float) area.getX()
                        + (float) ((double) position / total * area.getWidth()));
        };

        g.setColour (warningAmber.withAlpha (0.45f));
        g.drawVerticalLine (markerX (entry->loopStart), (float) area.getY(), (float) area.getBottom());
        g.drawVerticalLine (markerX (entry->loopStart + entry->loopLength),
                            (float) area.getY(), (float) area.getBottom());
    }
}

//==============================================================================
WaveformEditorWindow::WaveformEditorWindow (UserWaveLibrary& library,
                                            SpaceDustLookAndFeel& lookAndFeel)
    : DocumentWindow ("Space Dust - Waveforms", juce::Colour (0xff0a0a1f),
                      DocumentWindow::closeButton)
{
    // setContentOwned takes ownership immediately, so the raw pointer never
    // outlives this statement unowned. content is only a borrowed handle for
    // showFor() and refreshContent(); the window frees the component.
    auto* editor = new WaveformEditorComponent (library, lookAndFeel);
    setContentOwned (editor, true);
    content = editor;

    setUsingNativeTitleBar (true);
    setResizable (false, false);
    centreWithSize (editor->getWidth(), editor->getHeight());
    setVisible (true);

    allowFileDropsFromLowerPrivilege();
}

void WaveformEditorWindow::allowFileDropsFromLowerPrivilege()
{
#if JUCE_WINDOWS
    if (auto* peer = getPeer())
    {
        if (auto handle = (HWND) peer->getNativeHandle())
        {
            // WM_COPYGLOBALDATA has no name in the Windows headers, but a file
            // drop cannot cross the privilege boundary without it: the drop
            // message itself carries a handle to memory in the other process.
            constexpr UINT wmCopyGlobalData = 0x0049;

            ChangeWindowMessageFilterEx (handle, WM_DROPFILES, MSGFLT_ALLOW, nullptr);
            ChangeWindowMessageFilterEx (handle, WM_COPYDATA, MSGFLT_ALLOW, nullptr);
            ChangeWindowMessageFilterEx (handle, wmCopyGlobalData, MSGFLT_ALLOW, nullptr);
        }
    }
#endif
}

void WaveformEditorWindow::showFor (juce::ComboBox* targetCombo, int userBase,
                                    WaveformEditorComponent::BuiltInKind kind,
                                    UserWave::Group group, int slotIndex)
{
    // The five lists are separate, so the title bar says which one is open. The
    // window is reused, and without this a second Edit button would raise a
    // window that looks identical to the one just closed.
    setName ("Space Dust - " + juce::String (UserWave::groupName (group)));

    if (content != nullptr)
    {
        content->setTarget (targetCombo, userBase, kind, group);

        // The list that opened this may be longer or shorter than the last one,
        // and the content has just resized itself to suit. The window has to
        // follow, or the rows past the old bottom would have nowhere to be drawn.
        // Size only: where the player put the window is theirs to keep.
        setContentComponentSize (content->getWidth(), content->getHeight());

        // Only jump to a slot that has something in it. Opening the window must
        // never change the sound by itself.
        if (slotIndex >= 0)
            content->selectSlot (slotIndex);

        content->refresh();
    }

    setVisible (true);
    toFront (true);

    // The peer is recreated whenever the window is hidden and shown again, and
    // the filter is a property of the peer, not of the window.
    allowFileDropsFromLowerPrivilege();
}

void WaveformEditorWindow::refreshContent()
{
    if (content != nullptr)
        content->refresh();
}

void WaveformEditorWindow::repaintContent()
{
    if (content != nullptr)
        content->repaint();
}

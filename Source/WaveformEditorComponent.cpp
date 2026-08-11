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
    const juce::Colour panelNavy      { 0xff10102a };
    const juce::Colour rowNavy        { 0xff1a1a30 };
    const juce::Colour rowSelected    { 0xff21365c };
    const juce::Colour rowDropTarget  { 0xff1e4a5e };
    const juce::Colour labelCyan      { 0xffa0d8ff };
    const juce::Colour valueCyan      { 0xff6dd5fa };
    const juce::Colour knobArcCyan    { 0xff00d4ff };
    const juce::Colour dimText        { 0xff6a7a99 };
    const juce::Colour warningAmber   { 0xffffc266 };
    const juce::Colour errorRed       { 0xffff8080 };

    constexpr int margin = 12;
    constexpr int listWidth = 268;
    constexpr int detailWidth = 470;
    constexpr int rowHeight = 38;
    constexpr int groupTitleInset = 26;
    constexpr int groupPadding = 10;
    constexpr int statusHeight = 22;

    /** The list area is always tall enough for every row it could ever hold, so
        the window does not change size as waveforms are imported and cleared. The
        space under the last row is not wasted -- it is the drop zone. */
    constexpr int maxRows = UserWave::oscUserBase + UserWave::numSlots;

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
}

//==============================================================================
int WaveformEditorComponent::preferredWidth()
{
    return margin + listWidth + margin + detailWidth + margin;
}

int WaveformEditorComponent::preferredHeight()
{
    return margin + groupTitleInset + maxRows * rowHeight + groupPadding
         + margin + statusHeight + margin;
}

//==============================================================================
WaveformEditorComponent::WaveformEditorComponent (UserWaveLibrary& libraryToUse,
                                                  SpaceDustLookAndFeel& lookAndFeelToUse)
    : library (libraryToUse), lookAndFeel (lookAndFeelToUse)
{
    setSize (preferredWidth(), preferredHeight());

    // One call and every button, editor and group in here is drawn by the same
    // code that draws the main panel -- same fonts, same cyan, same rounded
    // boxes. Anything styled by hand below is only what the plugin styles by hand
    // on its own panel too.
    setLookAndFeel (&lookAndFeel);

    listGroup.setText ("Waveforms");
    detailGroup.setText ("Detail");
    addAndMakeVisible (listGroup);
    addAndMakeVisible (detailGroup);

    addAndMakeVisible (loadButton);
    addAndMakeVisible (clearButton);
    addAndMakeVisible (singleCycleButton);
    addAndMakeVisible (fullSampleButton);
    addAndMakeVisible (nameEditor);
    addAndMakeVisible (nameLabel);

    loadButton.onClick = [this] { browseForFile(); };

    clearButton.onClick = [this]
    {
        const int slot = activeSlot;
        library.clearSlot (slot);
        setStatus ("Slot " + juce::String (slot + 1) + " cleared.", false);
        refresh();
    };

    singleCycleButton.onClick = [this] { applyMode (UserWave::Mode::SingleCycle); };
    fullSampleButton.onClick  = [this] { applyMode (UserWave::Mode::FullSample); };

    // The two mode buttons are one control with two positions.
    singleCycleButton.setClickingTogglesState (true);
    fullSampleButton.setClickingTogglesState (true);
    singleCycleButton.setRadioGroupId (1);
    fullSampleButton.setRadioGroupId (1);
    singleCycleButton.setConnectedEdges (juce::Button::ConnectedOnRight);
    fullSampleButton.setConnectedEdges (juce::Button::ConnectedOnLeft);

    nameLabel.setText ("Name", juce::dontSendNotification);
    nameLabel.setFont (lookAndFeel.getBodyFont (12.0f, true));
    nameLabel.setColour (juce::Label::textColourId, labelCyan);
    nameLabel.setJustificationType (juce::Justification::centredLeft);

    nameEditor.setFont (lookAndFeel.getBodyFont (13.0f, false));
    nameEditor.setTextToShowWhenEmpty ("waveform name", dimText);
    nameEditor.setColour (juce::TextEditor::backgroundColourId, rowNavy);
    nameEditor.setColour (juce::TextEditor::textColourId, labelCyan);
    nameEditor.setColour (juce::TextEditor::outlineColourId, juce::Colour (0xff303050));
    nameEditor.setColour (juce::TextEditor::focusedOutlineColourId, knobArcCyan);

    nameEditor.onReturnKey = [this]
    {
        library.renameSlot (activeSlot, nameEditor.getText());
        refresh();
    };
    nameEditor.onFocusLost = [this]
    {
        library.renameSlot (activeSlot, nameEditor.getText());
        refresh();
    };

    for (auto* button : { &loadButton, &clearButton, &singleCycleButton, &fullSampleButton })
    {
        button->setColour (juce::TextButton::buttonColourId, juce::Colour (0xff1a1a30));
        button->setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff1e5f7a));
        button->setColour (juce::TextButton::textColourOffId, labelCyan);
        button->setColour (juce::TextButton::textColourOnId, juce::Colours::white);
    }

    refresh();
}

WaveformEditorComponent::~WaveformEditorComponent()
{
    setLookAndFeel (nullptr);
}

//==============================================================================
void WaveformEditorComponent::setTarget (juce::ComboBox* combo, int base)
{
    targetCombo = combo;
    userBase = base;
    refresh();
}

void WaveformEditorComponent::selectSlot (int slotIndex)
{
    activeSlot = juce::jlimit (0, UserWave::numSlots - 1, slotIndex);

    // Only move the selection onto the slot if there is something in it. Choosing
    // an empty slot would silence the oscillator, and opening a window must never
    // change the sound by itself.
    if (library.bank().slot (activeSlot).isPlayable() && targetCombo != nullptr)
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

    if (slot >= 0 && ! library.bank().slot (slot).isPlayable())
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
        if (! library.bank().slot (i).isPlayable())
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

    return library.choiceNameForSlot (slot);
}

juce::String WaveformEditorComponent::rowDetail (int row) const
{
    const int slot = slotForRow (row);

    if (slot < 0)
        return "Built in";

    const auto& entry = library.bank().slot (slot);

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

    const auto& entry = library.bank().slot (activeSlot);

    // A built-in has nothing to rename, clear, re-import or switch modes on, so
    // those controls go dead while one is shown. It is the only way the two kinds
    // of row differ.
    const bool showingSlot = (slot >= 0);
    const bool editable = showingSlot && entry.isPlayable();

    nameEditor.setText (editable ? entry.name : juce::String(), juce::dontSendNotification);
    nameEditor.setEnabled (editable);
    nameLabel.setEnabled (editable);
    clearButton.setEnabled (editable);
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
    const auto& existing = library.bank().slot (slotIndex);
    const auto mode = existing.isPlayable() ? existing.mode : UserWave::Mode::SingleCycle;

    // Reading the file and building eleven tables takes long enough on a long
    // sample to look like a hang without this.
    juce::MouseCursor::showWaitCursor();
    const bool ok = library.importFile (file, slotIndex, mode, error);
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

    const auto& slot = library.bank().slot (slotIndex);
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

    if (! library.setSlotMode (activeSlot, mode, error))
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

    return slot < 0 || library.bank().slot (slot).isPlayable();
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
    auto controls = detail.removeFromBottom (72);

    auto upper = controls.removeFromTop (30);
    auto modes = upper.removeFromLeft (200);
    singleCycleButton.setBounds (modes.removeFromLeft (100));
    fullSampleButton.setBounds (modes);

    clearButton.setBounds (upper.removeFromRight (92));
    upper.removeFromRight (6);
    loadButton.setBounds (upper.removeFromRight (100));

    controls.removeFromTop (8);

    auto lower = controls.removeFromTop (28);
    nameLabel.setBounds (lower.removeFromLeft (42));
    nameEditor.setBounds (lower);
}

//==============================================================================
void WaveformEditorComponent::paint (juce::Graphics& g)
{
    g.fillAll (backgroundNavy);

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
                                                int repeats) const
{
    if (area.getWidth() < 4 || area.getHeight() < 4)
        return;

    g.setColour (colour);

    const float centreY = (float) area.getCentreY();
    const float halfHeight = (float) area.getHeight() * 0.42f;
    const int slot = slotForRow (row);

    //==========================================================================
    // Noise has no repeating shape. Drawing one would be a lie, so it gets a
    // scatter that reads as noise at a glance instead.
    if (slot < 0 && userBase == UserWave::noiseUserBase)
    {
        juce::Random random (row == 0 ? 1234 : 5678);

        for (int x = 0; x < area.getWidth(); x += 2)
        {
            // Pink noise holds more of its energy low down, so its scatter is
            // drawn shorter -- the same difference a scope would show.
            const float spread = (row == 0) ? 1.0f : 0.5f;
            const float value = (random.nextFloat() * 2.0f - 1.0f) * spread;
            const float px = (float) (area.getX() + x);

            g.drawLine (px, centreY, px, centreY - value * halfHeight, thickness);
        }

        return;
    }

    const UserWaveSlot* entry = slot >= 0 ? &library.bank().slot (slot) : nullptr;

    if (entry != nullptr && ! entry->isPlayable())
        return;

    juce::Path path;
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

    g.strokePath (path, juce::PathStrokeType (thickness));
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

        g.setColour (isDropTarget ? rowDropTarget : (isSelected ? rowSelected : rowNavy));
        g.fillRoundedRectangle (bounds.toFloat(), 4.0f);

        if (isDropTarget)
        {
            g.setColour (knobArcCyan);
            g.drawRoundedRectangle (bounds.toFloat().reduced (1.0f), 4.0f, 2.0f);
        }
        else if (isSelected)
        {
            g.setColour (knobArcCyan.withAlpha (0.85f));
            g.drawRoundedRectangle (bounds.toFloat().reduced (1.0f), 4.0f, 1.4f);
        }

        auto content = bounds.reduced (8, 4);

        // The same picture beside every entry, built-in or imported, because the
        // whole point of the list is that they are the same kind of thing.
        paintRowWaveform (g, content.removeFromLeft (38), row,
                          isSelected ? knobArcCyan : dimText, 1.2f, 2);

        content.removeFromLeft (10);

        auto nameRow = content.removeFromTop (content.getHeight() / 2 + 1);

        g.setColour (isSelected ? juce::Colours::white : labelCyan);
        g.setFont (lookAndFeel.getBodyFont (13.0f, true));
        g.drawText (rowName (row), nameRow, juce::Justification::bottomLeft, true);

        g.setColour (isSelected ? valueCyan : dimText);
        g.setFont (lookAndFeel.getBodyFont (11.0f, false));
        g.drawText (rowDetail (row), content, juce::Justification::topLeft, true);
    }

    //==========================================================================
    // Everything below the last row is the drop zone. Hiding the empty slots left
    // this space behind, and it is exactly the affordance their absence removed:
    // somewhere to aim a file, that says where the file will land.
    const bool hasRoom = ! library.bank().slot (fallbackImportSlot()).isPlayable();

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

    g.setColour (zoneIsTarget ? knobArcCyan : juce::Colour (0xff303050));
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
    content.removeFromBottom (72);   // the control strip laid out in resized()

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
    const UserWaveSlot* entry = slot >= 0 ? &library.bank().slot (slot) : nullptr;

    //==========================================================================
    auto readout = content.removeFromTop (24);

    juce::String detail;
    juce::String note;
    juce::Colour noteColour = dimText;

    if (entry == nullptr)
    {
        detail = "Built in, always available";
        note = (userBase == UserWave::noiseUserBase)
             ? "One of the two noise colours. Nothing to import or change here."
             : "One of the four basic shapes. Always in tune, at every pitch.";
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
    g.setColour (juce::Colour (0xff08081a));
    g.fillRoundedRectangle (content.toFloat(), 4.0f);

    g.setColour (juce::Colour (0xff2a2a50));
    g.drawRoundedRectangle (content.toFloat(), 4.0f, 1.0f);
    g.drawHorizontalLine (content.getCentreY(), (float) content.getX() + 2.0f,
                          (float) content.getRight() - 2.0f);

    // Two periods for a cycle so it reads as repeating; one pass for a whole
    // sample, because that is the thing itself.
    const bool wholeSample = (entry != nullptr && entry->mode == UserWave::Mode::FullSample);

    paintRowWaveform (g, content.reduced (6, 8), row, knobArcCyan, 1.5f, wholeSample ? 1 : 2);

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

void WaveformEditorWindow::showFor (juce::ComboBox* targetCombo, int userBase, int slotIndex)
{
    if (content != nullptr)
    {
        content->setTarget (targetCombo, userBase);

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

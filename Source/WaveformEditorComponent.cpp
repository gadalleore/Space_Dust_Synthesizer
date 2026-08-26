#include "WaveformEditorComponent.h"

#include "OscillatorShapes.h"
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

    /** A button wide enough for a two-word name. The main panel has toggles from
        86 to 130 wide, so this is not a size of this window's own invention. */
    constexpr int wideButtonWidth = 130;

    /** Height of the three control rows plus the gaps between them. */
    constexpr int controlStripHeight = buttonHeight * 3 + 8 + 8 + 4;

    /** How often the window asks the synth whether its recording has finished.
        Often enough to feel immediate; the recording itself is timed on the audio
        thread, so nothing about its length depends on this. */
    constexpr int resamplePollMs = 60;

    /** The two clocks. Only one of them is ever running: the poll while a
        recording is being waited for, the playhead while the window is open. */
    constexpr int resampleTimerId = 0;
    constexpr int playheadTimerId = 1;
    constexpr int trimTimerId = 2;

    /** How often a drag is put into the slot and published: about thirty times a
        second.

        Fast enough that the sound follows the hand, and slow enough to be work
        the message thread can carry. In Full Sample mode a rebuild builds no
        mipmap tables -- buildSlot clears them and fills the sample buffer only --
        so one update is a copy of the sample, at most about 2.6 MB at the
        fifteen second limit. */
    constexpr int trimIntervalMs = 33;

    /** How often the playhead moves. Sixty a second, because it is a line moving
        across a picture and anything slower is seen as steps rather than as
        movement -- and because the panel's own redraw cannot be used for it: that
        one is driven by the output METER changing, so it stops entirely on a
        steady note and stutters on a changing one (Giuseppe, 2026-08-14). */
    constexpr int playheadIntervalMs = 16;

    /** How long to wait before deciding a recording will never finish.

        Comfortably past the longest one there can be -- two seconds of held note
        and then a tail capped at the length of a waveform slot -- so this only
        ever fires when the audio thread has stopped running altogether. */
    constexpr int resampleGiveUpMs = 30000;

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
        // The same function the oscillator reads, so the row draws the shape that
        // will actually sound. It used to be a second copy of the maths, which was
        // survivable at four shapes and would not be at twenty-one -- the picture
        // is how the player picks a shape, so a picture that lies is worse than no
        // picture at all.
        return OscShape::shapeValue (shape, phase01);
    }

    /** How near a marker the pointer has to be to pick it up, in pixels. A line
        is one pixel wide and a mouse is not that accurate; this is the same grab
        distance the panel's own draggable things use. */
    constexpr int handleGrabPixels = 6;

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

int WaveformEditorComponent::preferredHeight (int userBase, bool withShaping)
{
    const int wanted = margin + groupTitleInset + maxRowsFor (userBase) * rowHeight
                     + groupPadding + margin + statusHeight + margin
                     + (withShaping ? shapingStripHeight : 0);

    // Capped, and the list scrolls past the cap.
    //
    // Height used to follow the list exactly, which was right while the longest
    // list was the Transient's ten drums plus eight slots. The oscillators now
    // offer twenty-one built-in shapes, so their list can reach twenty-nine rows
    // and would ask for a panel taller than the plugin it sits in -- and a panel
    // clamped to fit would simply lose its bottom rows and its status line off
    // the edge, with no way to reach them.
    return juce::jmin (wanted, maxPanelHeight);
}

int WaveformEditorComponent::listContentHeight() const
{
    return numVisibleRows() * rowHeight;
}

int WaveformEditorComponent::listViewHeight() const
{
    return juce::jmax (0, listBounds().getHeight() - groupTitleInset - groupPadding);
}

int WaveformEditorComponent::maxListScroll() const
{
    return juce::jmax (0, listContentHeight() - listViewHeight());
}

void WaveformEditorComponent::adoptShapingControls (const ShapingControls* shaping)
{
    if (shaping == shapingControls)
        return;

    // Give the last set back first. They belong to the editor and are shared
    // between the two oscillators' panels, so leaving them parented here would
    // take them off the screen the next time the other oscillator was opened.
    if (shapingControls != nullptr)
    {
        for (int i = 0; i < ShapingControls::numKnobs; ++i)
        {
            if (shapingControls->knobs[i] != nullptr)
                removeChildComponent (shapingControls->knobs[i]);

            if (shapingControls->labels[i] != nullptr)
                removeChildComponent (shapingControls->labels[i]);
        }
    }

    shapingControls = shaping;

    if (shapingControls == nullptr)
        return;

    for (int i = 0; i < ShapingControls::numKnobs; ++i)
    {
        if (shapingControls->labels[i] != nullptr)
            addAndMakeVisible (shapingControls->labels[i]);

        if (shapingControls->knobs[i] != nullptr)
            addAndMakeVisible (shapingControls->knobs[i]);
    }
}

void WaveformEditorComponent::setListScroll (int newScroll)
{
    const int clamped = juce::jlimit (0, maxListScroll(), newScroll);

    if (clamped == listScroll)
        return;

    listScroll = clamped;
    repaint();
}

void WaveformEditorComponent::scrollRowIntoView (int row)
{
    const int position = displayPositionForRow (row);

    if (position < 0)
        return;

    // In the list's own coordinates, before the scroll is taken off.
    const int top = position * rowHeight;
    const int bottom = top + rowHeight;
    const int view = listViewHeight();

    if (top < listScroll)
        setListScroll (top);
    else if (bottom > listScroll + view)
        setListScroll (bottom - view);
}

//==============================================================================
WaveformEditorComponent::WaveformEditorComponent (UserWaveLibrary& libraryToUse,
                                                  SpaceDustLookAndFeel& lookAndFeelToUse)
    : library (libraryToUse), lookAndFeel (lookAndFeelToUse)
{
    setSize (preferredWidth(), preferredHeight (userBase, shapingControls != nullptr));

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
    resampleButton.setButtonText ("Resample");
    resampleInitButton.setButtonText ("Resample + Init");
    loopButton.setButtonText ("Loop");

    addAndMakeVisible (loadButton);
    addAndMakeVisible (clearButton);
    addAndMakeVisible (updateAllButton);
    addAndMakeVisible (singleCycleButton);
    addAndMakeVisible (fullSampleButton);
    addAndMakeVisible (resampleButton);
    addAndMakeVisible (resampleInitButton);
    addAndMakeVisible (loopButton);
    addAndMakeVisible (nameEditor);
    addAndMakeVisible (nameLabel);

    // The whole-file picture and its playhead. Positioned by refresh(), and told
    // here how to draw itself -- every rule about what a waveform looks like in
    // this window stays in this class.
    sampleStrip.drawPicture = [this] (juce::Graphics& g, juce::Rectangle<int> area)
    {
        paintWholeFilePicture (g, area);
    };

    addChildComponent (sampleStrip);

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

    resampleButton.setTooltip ("Record a middle C, silently, and put the sound that "
                               "comes out into a waveform slot");
    resampleInitButton.setTooltip ("The same, and then turn everything else off so "
                                  "that waveform is all you hear");

    // The same slot rule as Load File, and for the same reason: a resample is an
    // import that happens to come from inside the plugin, so it must not land
    // anywhere a dropped file would not.
    resampleButton.onClick     = [this] { beginResample (fallbackImportSlot(), false); };
    resampleInitButton.onClick = [this] { beginResample (fallbackImportSlot(), true); };

    loopButton.setTooltip ("On: the sample repeats while the key is held. "
                           "Off: it plays once and stops.");
    loopButton.setClickingTogglesState (true);

    loopButton.onClick = [this]
    {
        const bool shouldLoop = loopButton.getToggleState();

        library.setSlotLoop (group, activeSlot, shouldLoop);

        setStatus (shouldLoop ? "Loop on: a held key plays the sample over and over."
                              : "Loop off: the sample plays once, all the way through, "
                                "and stops.", false);
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
                                         UserWave::Group groupToShow,
                                         const ShapingControls* shaping)
{
    targetCombo = combo;
    userBase = base;
    builtInKind = kind;
    group = groupToShow;

    // Before the size is worked out below: whether there is a shaping strip is
    // part of how tall the panel needs to be.
    adoptShapingControls (shaping);

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
    setSize (preferredWidth(), preferredHeight (userBase, shapingControls != nullptr));

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

    // The import slots sit below twenty-one built-in shapes, so on an oscillator
    // list they are past the bottom of the box every time. After refresh(), which
    // is what settles which rows are visible at all.
    scrollRowIntoView (rowForSelection());
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

    // "not tuned" is a warning: the pitch could not be read, so the sample plays
    // at its recorded speed. A resample is at its recorded speed BY DESIGN -- it
    // was recorded from middle C -- so the same words would read as a fault.
    if (entry.mode == UserWave::Mode::FullSample && ! entry.retuned)
        detail += entry.allowRetune ? "   not tuned" : "   from middle C";

    // Said on the row, because it is the difference between a key that holds a
    // sound and a key that fires one off, and the player should not have to
    // select a row to find out which they have.
    if (entry.mode == UserWave::Mode::FullSample && ! entry.loop)
        detail += "   one shot";

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

    // Everything that acts on a slot goes dead while a recording is running. It
    // is only a few seconds, and every one of these would be acting on the slot
    // that the recording is about to overwrite.
    const bool busy = isResampling();
    const bool editable = showingSlot && entry.isPlayable() && ! busy;

    nameEditor.setText (showingSlot && entry.isPlayable() ? entry.name : juce::String(),
                        juce::dontSendNotification);
    nameEditor.setEnabled (editable);
    nameLabel.setEnabled (editable);
    clearButton.setEnabled (editable);
    updateAllButton.setEnabled (editable);
    singleCycleButton.setEnabled (editable);
    fullSampleButton.setEnabled (editable);
    loadButton.setEnabled (! busy);

    // The two Resample buttons are NOT tied to the selected row. A resample lands
    // in the slot a dropped file would land in, so it is as available while a
    // built-in is on screen as Load File is. All they need is a synth to record,
    // which is what the editor gives them.
    resampleButton.setEnabled (resampleHost != nullptr && ! busy);
    resampleInitButton.setEnabled (resampleHost != nullptr && ! busy);

    singleCycleButton.setToggleState (editable && entry.mode == UserWave::Mode::SingleCycle,
                                      juce::dontSendNotification);
    fullSampleButton.setToggleState (editable && entry.mode == UserWave::Mode::FullSample,
                                     juce::dontSendNotification);

    // A single cycle has nowhere to go but round, so the loop is not a choice
    // there and the button says so by going dead rather than by lying about
    // being off.
    const bool wholeSample = editable && entry.mode == UserWave::Mode::FullSample;
    loopButton.setEnabled (wholeSample);
    loopButton.setToggleState (wholeSample ? entry.loop : true, juce::dontSendNotification);

    detailGroup.setText (row >= 0 ? rowName (row) : juce::String ("Detail"));

    // The whole-file picture belongs to whichever slot is on screen, so it is
    // moved, shown, hidden and redrawn here rather than only when the window is
    // resized -- selecting a different row is what changes it, not the size.
    positionSampleStrip();

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

void WaveformEditorComponent::setResampleHost (ResampleHost* host)
{
    resampleHost = host;
    refresh();      // the two buttons take their enabled state from this
}

void WaveformEditorComponent::beginResample (int slotIndex, bool alsoInitialise)
{
    if (resampleHost == nullptr || isResampling())
        return;

    juce::String error;

    if (! resampleHost->startCapture (error))
    {
        setStatus (error, true);
        return;
    }

    pendingSlot = slotIndex;
    pendingInitialise = alsoInitialise;
    pendingTicks = 0;

    // Every line this panel shows has to fit the one status row across the bottom
    // of the window, which is about a hundred characters wide. Anything longer is
    // silently cut off in the middle of a word.
    setStatus ("Recording a middle C. Nothing is heard while it runs.", false);

    // Often enough to feel immediate, seldom enough to cost nothing. The recording
    // itself is timed by the audio thread, not by this.
    startTimer (resampleTimerId, resamplePollMs);
    refresh();
}

WaveformEditorComponent::SampleStrip::SampleStrip()
{
    // Both of these are the point of this class -- see the note on it.
    setOpaque (true);
    setInterceptsMouseClicks (false, false);
}

void WaveformEditorComponent::SampleStrip::rebuild()
{
    if (getWidth() <= 0 || getHeight() <= 0 || ! drawPicture)
    {
        picture = {};
        repaint();
        return;
    }

    // RGB, not ARGB: this component is opaque, so nothing is drawn underneath it
    // and every pixel here has to come from somewhere.
    picture = juce::Image (juce::Image::RGB, getWidth(), getHeight(), false);

    juce::Graphics g (picture);
    drawPicture (g, getLocalBounds());

    repaint();
}

void WaveformEditorComponent::SampleStrip::setPosition (float newPosition, juce::Colour newColour)
{
    // A line is one pixel wide, so a move it cannot show is a repaint that costs
    // everything and changes nothing. Sixty times a second, that is the whole
    // difference between a playhead and a busy loop.
    const auto area = pictureArea();
    const bool moved = std::abs (newPosition - position) * (float) juce::jmax (1, area.getWidth()) >= 0.5f;
    const bool appeared = (newPosition < 0.0f) != (position < 0.0f);

    if (! moved && ! appeared && newColour == colour)
        return;

    position = newPosition;
    colour = newColour;
    repaint();
}

void WaveformEditorComponent::SampleStrip::paint (juce::Graphics& g)
{
    if (picture.isValid())
        g.drawImageAt (picture, 0, 0);
    else
        g.fillAll (backgroundNavy);

    if (position < 0.0f || position > 1.0f)
        return;

    // Drawn as a thin filled rectangle at a fractional position rather than as a
    // line on a whole pixel: the edges are shaded in, so the line slides instead
    // of stepping from one pixel to the next.
    const auto area = pictureArea();
    const float x = (float) area.getX() + position * (float) area.getWidth();

    g.setColour (colour.withAlpha (0.9f));
    g.fillRect (juce::Rectangle<float> (x - 0.5f, (float) area.getY(),
                                        1.0f, (float) area.getHeight()));
}

//==============================================================================
void WaveformEditorComponent::setPlayheadActive (bool shouldFollow)
{
    if (playheadActive == shouldFollow)
        return;

    playheadActive = shouldFollow;

    // The audio thread is told too. Nothing is published while no window is
    // watching, so a closed window costs nothing at all -- not a repaint here and
    // not a store per source per block over there.
    if (resampleHost != nullptr)
        resampleHost->setPlaybackPhaseWanted (shouldFollow);

    if (shouldFollow)
    {
        startTimer (playheadTimerId, playheadIntervalMs);
    }
    else
    {
        stopTimer (playheadTimerId);
        sampleStrip.setPosition (-1.0f, traceColour());
    }
}

void WaveformEditorComponent::positionSampleStrip()
{
    const auto area = wholeFileBounds();

    sampleStrip.setVisible (! area.isEmpty());

    if (area.isEmpty())
        return;

    // setBounds only rebuilds the picture when the size actually changed (it goes
    // through resized), so this is safe to call from refresh().
    const bool sameSize = (sampleStrip.getBounds() == area);
    sampleStrip.setBounds (area);

    if (sameSize)
        sampleStrip.rebuild();   // same shape, different contents
}

void WaveformEditorComponent::updatePlayhead()
{
    const auto* entry = shownSlot();

    if (! sampleStrip.isVisible() || entry == nullptr || resampleHost == nullptr
        || entry->loopLength <= 0 || entry->sample.empty())
    {
        sampleStrip.setPosition (-1.0f, traceColour());
        return;
    }

    const float phase = resampleHost->playbackPhase (group);

    if (phase < 0.0f || phase > 1.0f)
    {
        sampleStrip.setPosition (-1.0f, traceColour());
        return;
    }

    // A phase runs 0 to 1 across the part that PLAYS, which begins a crossfade
    // past the start marker when the slot loops -- so it is placed against the
    // loop, not against the trimmed region, or the line would lag its own sound
    // by ten milliseconds.
    const double from = entry->loop ? (double) entry->loopStart : (double) entry->playStart;

    sampleStrip.setPosition (
        (float) juce::jlimit (0.0, 1.0,
                              axisPosition (*entry,
                                            from + (double) phase * (double) entry->playSpan())),
        traceColour());
}

void WaveformEditorComponent::timerCallback (int timerID)
{
    if (timerID == playheadTimerId)
    {
        updatePlayhead();
        return;
    }

    if (timerID == trimTimerId)
    {
        // Nothing to do when the mouse has stopped: updateTrimSession compares
        // the numbers and returns at once if they have not moved.
        juce::String error;

        // Exactly the end of the file is stored as a ZERO, not as the length --
        // so that a slot whose sample is later replaced by a longer one still
        // plays all of it. The same conversion commitTrim does, done here too, so
        // the drag and the mouse-up never write two different numbers for the
        // same marker position.
        const auto* entry = shownSlot();
        const int end = (entry != nullptr && dragTrimEnd == entry->fileLength)
                            ? 0 : dragTrimEnd;

        if (! library.updateTrimSession (dragTrimStart, end, error))
        {
            // The drag has reached somewhere the slot cannot go. Stop it here
            // rather than let it fight the clamps for the rest of the gesture.
            stopTimer (trimTimerId);
            trimDrag = TrimHandle::None;

            juce::String ignored;
            library.endTrimSession (ignored);

            setStatus (error, true);
            refresh();
        }

        return;
    }

    if (! isResampling() || resampleHost == nullptr)
    {
        stopTimer (resampleTimerId);
        return;
    }

    if (resampleHost->captureIsRunning())
    {
        // A recording that never ends means the audio thread has stopped running
        // it -- a device that has gone away, or a host that has stopped calling
        // the plugin. Give up, rather than leave the buttons dead for good.
        if (++pendingTicks * resamplePollMs > resampleGiveUpMs)
        {
            resampleHost->abandonCapture();
            failResample ("The synth is not running, so there was nothing to record.");
            return;
        }

        // Nothing is heard while a recording runs, so the bar is the only sign
        // that anything is happening. It has to be redrawn to move.
        repaint();
        return;
    }

    completeResample();
}

void WaveformEditorComponent::failResample (const juce::String& message)
{
    stopTimer (resampleTimerId);
    pendingSlot = -1;
    setStatus (message, true);
    refresh();
}

void WaveformEditorComponent::completeResample()
{
    stopTimer (resampleTimerId);

    const int slotIndex = pendingSlot;
    const bool alsoInitialise = pendingInitialise;
    pendingSlot = -1;

    Capture capture;
    juce::String error;

    if (resampleHost == nullptr || ! resampleHost->collectCapture (capture, error))
    {
        failResample (error);
        return;
    }

    // Analysing the recording and building the slot takes long enough on a long
    // tail to look like a hang, exactly as importing a file does.
    juce::MouseCursor::showWaitCursor();
    const bool ok = library.importAudio (std::move (capture.mono), capture.sampleRate,
                                         capture.name, group, slotIndex,
                                         UserWave::Mode::FullSample, error);
    juce::MouseCursor::hideWaitCursor();

    if (! ok)
    {
        failResample (error);
        return;
    }

    activeSlot = slotIndex;

    const int choiceIndex = userBase + slotIndex;
    const auto seconds = juce::String (library.bank().slot (group, slotIndex).sample.size()
                                         / juce::jmax (1.0, capture.sampleRate), 1);

    // The tail outlasting the slot is the one thing the player has to act on --
    // only they can shorten a reverb -- so it takes the whole line when it
    // happens, in the amber the panel warns in.
    if (capture.cutShort)
    {
        if (alsoInitialise)
            resampleHost->initialiseAroundWaveform (group, choiceIndex, capture.peak);
        else if (targetCombo != nullptr)
            targetCombo->setSelectedId (choiceIndex + 1, juce::sendNotificationSync);

        setStatus ("Slot " + juce::String (slotIndex + 1) + " holds " + seconds
                   + " s -- all a slot can. The tail was still sounding, so it is cut.",
                   true);
        refresh();
        return;
    }

    if (alsoInitialise)
    {
        // Stripping the patch back selects the waveform as well -- it has to, or
        // the reset that turns the effects off would leave the dropdown on Sine.
        // See the note on ResampleHost::initialiseAroundWaveform.
        resampleHost->initialiseAroundWaveform (group, choiceIndex, capture.peak);

        setStatus ("Slot " + juce::String (slotIndex + 1) + " holds " + seconds
                   + " s of that sound, and it is all you hear now: no effects, "
                     "filter open, no envelope.", false);
    }
    else
    {
        if (targetCombo != nullptr)
            targetCombo->setSelectedId (choiceIndex + 1, juce::sendNotificationSync);

        setStatus ("Slot " + juce::String (slotIndex + 1) + " of "
                   + UserWave::groupName (group) + " holds " + seconds
                   + " s of that sound. The effects are still on, so it goes through "
                     "them again.", false);
    }

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
WaveformEditorComponent::TrimHandle
WaveformEditorComponent::handleAt (juce::Point<int> position) const
{
    const auto* entry = shownSlot();
    const auto area = wholeFileBounds();

    if (entry == nullptr || area.isEmpty() || isResampling()
        || entry->mode != UserWave::Mode::FullSample || entry->sample.empty())
        return TrimHandle::None;

    // The whole height of the strip, not only the tab at the top of the line: the
    // strip is fifty pixels tall under a pitched sample, and asking for the top
    // five of them would be asking for accuracy nobody has.
    if (! area.contains (position))
        return TrimHandle::None;

    const auto picture = area.reduced (5, 5);

    int start = 0;
    int end = 0;
    trimPoints (*entry, start, end);

    const auto pixelsFrom = [&] (int filePosition)
    {
        const double at = axisPosition (*entry, (double) (entry->fileOffset() + filePosition));
        const double x = (double) picture.getX() + at * (double) picture.getWidth();
        return std::abs (x - (double) position.getX());
    };

    const double toStart = pixelsFrom (start);
    const double toEnd = pixelsFrom (end);

    if (toStart > (double) handleGrabPixels && toEnd > (double) handleGrabPixels)
        return TrimHandle::None;

    // The nearer of the two when both are in reach, which is what happens on a
    // sample trimmed down to almost nothing.
    return toStart <= toEnd ? TrimHandle::Start : TrimHandle::End;
}

void WaveformEditorComponent::mouseDown (const juce::MouseEvent& event)
{
    // A press on a marker is the start of a drag, and nothing else. It has to be
    // asked first: the markers are drawn on the detail panel, well away from the
    // list, but a press that reached selectRow() would rebuild the panel out from
    // under the drag.
    if (const auto handle = handleAt (event.getPosition()); handle != TrimHandle::None)
    {
        if (const auto* entry = shownSlot())
        {
            // Where the markers stand READ FIRST, and the drag begun after.
            // trimPoints() answers with the drag in progress once there is one,
            // so setting the handle first made it seed the drag from the last
            // drag's leftovers -- which on the first press of all is 0 and 0, and
            // both marks shut against the left wall (Giuseppe, 2026-08-16).
            trimPoints (*entry, dragTrimStart, dragTrimEnd);

            // Decode the slot's audio once, here, so that every rebuild for the
            // rest of this gesture is free of it. A slot that cannot be opened is
            // a slot that cannot be dragged: say so and do not begin.
            juce::String error;

            if (! library.beginTrimSession (group, activeSlot, error))
            {
                setStatus (error, true);
                return;
            }

            trimDrag = handle;
        }

        return;
    }

    const int row = rowAt (event.getPosition());

    if (row >= 0)
        selectRow (row);
}

void WaveformEditorComponent::mouseDrag (const juce::MouseEvent& event)
{
    if (trimDrag == TrimHandle::None)
        return;

    const auto* entry = shownSlot();

    if (entry == nullptr)
        return;

    // Where the pointer is, in samples of the FILE. Negative is in front of it,
    // which is the silence this whole gesture exists for.
    const int at = (int) std::lround (bufferIndexAt (*entry, event.x)
                                      - (double) entry->fileOffset());

    // Each marker reaches as far off its own end of the file as the gutter drawn
    // there, and no nearer the other marker than there is sound to leave between
    // them.
    const int gutter = padGutter (*entry);

    if (trimDrag == TrimHandle::Start)
    {
        dragTrimStart = juce::jlimit (-(entry->padStart() + gutter),
                                      juce::jmin (dragTrimEnd, entry->fileLength)
                                          - UserWave::minPlayLength,
                                      at);
    }
    else
    {
        dragTrimEnd = juce::jlimit (juce::jmax (0, dragTrimStart) + UserWave::minPlayLength,
                                    entry->fileLength + entry->padEnd() + gutter,
                                    at);
    }

    // The picture is cached, so it has to be told the marks moved. One pass over
    // the sample per frame of the drag, which is what it costs to have the shaded
    // region follow the pointer instead of jumping when the mouse comes up.
    sampleStrip.rebuild();
    repaint();

    // And the SOUND follows the same way. Not from here -- a rebuild per mouse
    // move is far more than the ear needs -- but from a timer that reads where
    // the drag has got to.
    if (! isTimerRunning (trimTimerId))
        startTimer (trimTimerId, trimIntervalMs);
}

void WaveformEditorComponent::mouseUp (const juce::MouseEvent&)
{
    if (trimDrag == TrimHandle::None)
        return;

    trimDrag = TrimHandle::None;
    stopTimer (trimTimerId);
    commitTrim();
}

void WaveformEditorComponent::mouseWheelMove (const juce::MouseEvent& event,
                                              const juce::MouseWheelDetails& wheel)
{
    // Only over the list. The picture beside it has its own gestures, and a wheel
    // there scrolling the list behind the pointer would be a surprise.
    if (! listBounds().contains (event.getPosition()) || maxListScroll() == 0)
        return;

    // deltaY is positive when the wheel turns away from the user, which should
    // move the list UP -- so it is subtracted. Scaled by a row so one notch moves
    // about three rows, the same feel as a list anywhere else.
    setListScroll (listScroll - juce::roundToInt (wheel.deltaY * rowHeight * 3.0f));
}

void WaveformEditorComponent::mouseMove (const juce::MouseEvent& event)
{
    setMouseCursor (handleAt (event.getPosition()) != TrimHandle::None
                        ? juce::MouseCursor::LeftRightResizeCursor
                        : juce::MouseCursor::NormalCursor);
}

void WaveformEditorComponent::mouseDoubleClick (const juce::MouseEvent& event)
{
    const auto* entry = shownSlot();
    const auto area = wholeFileBounds();

    if (entry == nullptr || area.isEmpty() || ! area.contains (event.getPosition())
        || isResampling())
        return;

    juce::String error;

    if (! library.setSlotTrim (group, activeSlot, 0, 0, error))
    {
        setStatus (error, true);
    }
    else
    {
        setStatus ("Start and end put back: the whole sample plays again.", false);
        positionSampleStrip();
    }

    refresh();
}

void WaveformEditorComponent::commitTrim()
{
    const auto* entry = shownSlot();

    if (entry == nullptr)
        return;

    const int start = dragTrimStart;

    // Exactly the end of the file is stored as a zero, so that a slot whose
    // sample is later replaced by a longer one still plays all of it. Past the
    // end is silence, and that is kept as the number it is.
    const int end = dragTrimEnd == entry->fileLength ? 0 : dragTrimEnd;

    juce::String error;

    // The session has been putting this into the slot all through the drag. This
    // applies wherever the mouse got to after the last timer tick, then writes
    // the index file once.
    //
    // setSlotTrim is untouched and is still what a double-click reset uses: one
    // finished decision, decode and write and all.
    if (! library.updateTrimSession (start, end, error)
        || ! library.endTrimSession (error))
    {
        setStatus (error, true);
        refresh();
        return;
    }

    // Read back from the slot, not from the drag: what the marker asked for and
    // what the slot could give it are not always the same number.
    const auto& updated = library.bank().slot (group, activeSlot);

    juce::String message;

    if (updated.padStart() > 0)
        message << timeLabel (updated, (double) updated.padStart()) << " of silence first. ";

    if (updated.padEnd() > 0)
        message << timeLabel (updated, (double) updated.padEnd()) << " of silence after. ";

    if (updated.padStart() == 0 && updated.padEnd() == 0)
        message << "Plays from " << timeLabel (updated, (double) updated.trimStart) << " to "
                << timeLabel (updated, (double) (updated.trimEnd > 0 ? updated.trimEnd
                                                                     : updated.fileLength))
                << " of the sample. ";

    message << "A key now sounds for " << timeLabel (updated, (double) updated.playLength) << ".";

    setStatus (message, false);

    // The buffer may have grown or shrunk with the silence in front of it, so the
    // picture is redrawn from scratch rather than merely repainted.
    positionSampleStrip();
    refresh();
}

juce::String WaveformEditorComponent::timeLabel (const UserWaveSlot& entry, double samples) const
{
    if (entry.fileSampleRate <= 0.0)
        return "0 ms";

    const double seconds = samples / entry.fileSampleRate;

    if (std::abs (seconds) < 1.0)
        return juce::String (juce::roundToInt (seconds * 1000.0)) + " ms";

    return juce::String (seconds, 2) + " s";
}

//==============================================================================
juce::Rectangle<int> WaveformEditorComponent::listBounds() const
{
    // The shaping strip lives between the boxes and the status line, so both
    // boxes give up its height. Asked here, in the one place their height is
    // worked out, so the list, the detail panel, the picture and the rows all
    // follow from it.
    const int strip = shapingControls != nullptr ? shapingStripHeight : 0;

    return { margin, margin, listWidth,
             getHeight() - margin - statusHeight - 2 * margin - strip };
}

juce::Rectangle<int> WaveformEditorComponent::detailBounds() const
{
    auto list = listBounds();
    return { list.getRight() + margin, list.getY(), detailWidth, list.getHeight() };
}

//==============================================================================
// The picture geometry lives here, not in paintDetail, because the playhead is a
// component that has to be POSITIONED over the same rectangles the picture is
// drawn in. Worked out twice, in two places, they would drift apart the first
// time either was adjusted.
juce::Rectangle<int> WaveformEditorComponent::pictureBounds() const
{
    auto content = detailBounds().reduced (groupPadding, 0);
    content.removeFromTop (groupTitleInset);
    content.removeFromBottom (groupPadding);
    content.removeFromBottom (controlStripHeight);   // the strip laid out in resized()
    content.removeFromTop (24 + 4);                  // the readout line
    content.removeFromBottom (32);                   // the note under the picture

    return content;
}

const UserWaveSlot* WaveformEditorComponent::shownSlot() const
{
    const int row = rowForSelection();

    if (row < 0)
        return nullptr;

    const int slot = slotForRow (row);

    return slot >= 0 ? &library.bank().slot (group, slot) : nullptr;
}

juce::Rectangle<int> WaveformEditorComponent::wholeFileBounds() const
{
    const auto* entry = shownSlot();

    if (entry == nullptr || ! entry->isPlayable()
        || entry->mode != UserWave::Mode::FullSample)
        return {};

    auto content = pictureBounds();

    if (content.getHeight() < 40)
        return {};

    // An unpitched sample shows its outline across the whole box; a pitched one
    // shows its shape there and the whole file in the strip beneath. Either way
    // this is the picture a position along the file belongs on.
    //
    // Inset by one, so the box's own border and its rounded corners stay the
    // panel's to draw -- this is an opaque component and it would otherwise paint
    // over them with square ones.
    if (! drawsCycles (*entry))
        return content.reduced (1, 1);

    return content.removeFromBottom (juce::jmin (54, content.getHeight() / 3)).reduced (1, 1);
}

//==============================================================================
// -- Where things are along the picture --
//
// The picture's axis is NOT the sample. It is the buffer with a gutter at each
// end of it:
//
//   [ gutter ][ guard ][ silence ][ the whole file ][ silence ][ guard ][ gutter ]
//   ^                                                                          ^
//   axis 0                                                                 axis 1
//
// A gutter is empty room outside the sound with nothing in it to draw. It is
// there so a marker has somewhere to go when it is dragged off the end of the
// file, which is the gesture that adds silence -- with the file drawn edge to
// edge, both markers would already be hard against the walls.
//
// Everything that has a position -- the two markers, the dimming outside them,
// the playhead, and the mouse coming the other way -- goes through the two
// functions below, so none of them can disagree about where anything is.

int WaveformEditorComponent::padGutter (const UserWaveSlot& entry) const
{
    if (entry.fileLength <= 0)
        return 0;

    // What is left of the slot's allowance for silence, which the two ends share.
    const int room = UserWave::maxPadSamples (entry.fileLength, entry.fileSampleRate)
                   - entry.padStart() - entry.padEnd();

    // A quarter of the file at each end, so the gutters are a visible share of
    // the picture whatever the sample is -- and half the remaining room apiece
    // when there is less than that left, so neither end can eat the other's.
    return juce::jlimit (0, juce::jmax (0, room) / 2, entry.fileLength / 4);
}

void WaveformEditorComponent::trimPoints (const UserWaveSlot& entry, int& start, int& end) const
{
    if (trimDrag != TrimHandle::None)
    {
        start = dragTrimStart;
        end = dragTrimEnd;
        return;
    }

    start = entry.trimStart;
    end = entry.trimEnd > 0 ? entry.trimEnd : entry.fileLength;
}

double WaveformEditorComponent::axisPosition (const UserWaveSlot& entry,
                                              double bufferIndex) const
{
    const double gutter = (double) padGutter (entry);
    const double span = 2.0 * gutter + (double) entry.sample.size();

    if (span <= 0.0)
        return 0.0;

    return (bufferIndex + gutter) / span;
}

double WaveformEditorComponent::bufferIndexAt (const UserWaveSlot& entry, int x) const
{
    const auto area = wholeFileBounds();

    if (area.isEmpty())
        return 0.0;

    // The picture sits inside the strip by the same inset the strip draws it at.
    const auto picture = area.reduced (5, 5);
    const double width = (double) juce::jmax (1, picture.getWidth());
    const double gutter = (double) padGutter (entry);
    const double span = 2.0 * gutter + (double) entry.sample.size();

    return ((double) (x - picture.getX()) / width) * span - gutter;
}

//==============================================================================
void WaveformEditorComponent::paintWholeFilePicture (juce::Graphics& g,
                                                     juce::Rectangle<int> area) const
{
    const auto* entry = shownSlot();

    // Opaque: every pixel of this comes from here, including the background the
    // panel would otherwise have drawn underneath.
    g.fillAll (backgroundNavy);

    if (entry == nullptr || entry->sample.empty())
        return;

    auto picture = area.reduced (5, 5);

    g.setColour (toggleBorder.withAlpha (0.5f));
    g.drawHorizontalLine (picture.getCentreY(), (float) picture.getX(),
                          (float) picture.getRight());

    // Dropped back when it is the strip under a shape, because it is the context
    // and the shape is the subject -- and at full weight when it IS the picture,
    // which is what an unpitched sample gets.
    const bool isStrip = drawsCycles (*entry);

    const double gutter = (double) padGutter (*entry);

    paintSampleOutline (g, picture, *entry, -gutter, (double) entry->sample.size() + gutter,
                        traceColour().withAlpha (isStrip ? 0.5f : 0.9f),
                        isStrip ? 1.0f : 1.8f,
                        isStrip ? Bloom::Tight : Bloom::Wide);

    //==========================================================================
    // The two markers, and everything outside them dimmed.
    //
    // Dimmed rather than hidden, because it is still there: the slot keeps the
    // whole file, so what is under the shading is exactly what dragging the
    // marker back out would bring back. Hiding it would say it had gone.
    int start = 0;
    int end = 0;
    trimPoints (*entry, start, end);

    const auto markerX = [&] (int filePosition)
    {
        const double position = axisPosition (*entry, (double) (entry->fileOffset()
                                                                 + filePosition));
        return (float) picture.getX() + (float) (position * (double) picture.getWidth());
    };

    const float startX = markerX (start);
    const float endX = markerX (end);

    g.setColour (backgroundNavy.withAlpha (0.72f));

    if (startX > (float) picture.getX())
        g.fillRect (juce::Rectangle<float> ((float) picture.getX(), (float) picture.getY(),
                                            startX - (float) picture.getX(),
                                            (float) picture.getHeight()));

    if (endX < (float) picture.getRight())
        g.fillRect (juce::Rectangle<float> (endX, (float) picture.getY(),
                                            (float) picture.getRight() - endX,
                                            (float) picture.getHeight()));

    // The line, and a tab at the top of it to say it can be taken hold of. The
    // tab points INTO the part that plays, so the two markers face each other and
    // the sound is plainly the thing between them.
    const auto drawMarker = [&] (float x, bool pointsRight)
    {
        const float top = (float) picture.getY();
        const float bottom = (float) picture.getBottom();
        const float tab = 5.0f;

        g.setColour (knobArcCyan.withAlpha (0.9f));
        g.fillRect (juce::Rectangle<float> (x - 0.5f, top, 1.0f, bottom - top));

        juce::Path grip;
        grip.startNewSubPath (x, top);
        grip.lineTo (x + (pointsRight ? tab : -tab), top);
        grip.lineTo (x, top + tab);
        grip.closeSubPath();

        g.fillPath (grip);
    };

    drawMarker (startX, true);
    drawMarker (endX, false);

    // Where the recording itself begins and ends, when there is silence outside
    // it. Faint, and only marks: they cannot be dragged and they are not the
    // start or the end of anything -- they simply say which part of the run-in
    // and the run-out is silence the player asked for and which part is the
    // recording.
    g.setColour (warningAmber.withAlpha (0.35f));

    if (start < 0)
        g.drawVerticalLine ((int) markerX (0), (float) picture.getY(),
                            (float) picture.getBottom());

    if (end > entry->fileLength)
        g.drawVerticalLine ((int) markerX (entry->fileLength), (float) picture.getY(),
                            (float) picture.getBottom());

    //==========================================================================
    // What to do with them, said once, on a sample nobody has trimmed yet -- and
    // gone for good on that sample the moment they have. A tab on a line is a
    // small thing to notice in a strip fifty pixels tall, and the alternative to
    // this is a sentence taking up room on the panel for ever to teach something
    // that only needs teaching once.
    if (start == 0 && end == entry->fileLength && trimDrag == TrimHandle::None)
    {
        auto hint = picture.removeFromBottom (14).reduced (4, 0);

        g.setColour (dimText.withAlpha (0.75f));
        g.setFont (lookAndFeel.getBodyFont (10.5f, false));
        g.drawText ("drag the marks to set start and end -- past either end, for silence",
                    hint, juce::Justification::centredRight, true);
    }
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

    // listScroll comes off here, in the ONE place a row's position is worked out.
    // rowAt() finds a row by asking every row whether it contains a point, so
    // hit-testing follows the scroll for free and cannot disagree with what is
    // drawn -- which is exactly the sort of pair that drifts apart when the
    // offset is applied in two places.
    return { list.getX(),
             list.getY() + groupTitleInset + position * rowHeight - listScroll,
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

    // The shaping strip, across the foot of the whole panel and under BOTH boxes.
    // It is taken from the bottom before anything else, so the picture and the
    // slot controls above it lay out into whatever is left.
    //
    // Full width and not inside the detail box, because these five do not act on
    // the slot on screen: they act on the oscillator, and every waveform it plays
    // goes through them. Sitting them among the Load and Clear buttons would say
    // the opposite.
    if (shapingControls != nullptr)
    {
        // listBounds() has already given up this height, so the strip goes in the
        // gap that leaves rather than being taken out of the detail box again.
        auto strip = getLocalBounds().withTrimmedBottom (statusHeight + margin)
                                     .removeFromBottom (shapingStripHeight)
                                     .reduced (margin, 0);

        const int cell = strip.getWidth() / ShapingControls::numKnobs;

        for (int i = 0; i < ShapingControls::numKnobs; ++i)
        {
            auto column = strip.removeFromLeft (cell);

            if (shapingControls->labels[i] != nullptr)
                shapingControls->labels[i]->setBounds (column.removeFromTop (shapingLabelHeight));
            else
                column.removeFromTop (shapingLabelHeight);

            column.removeFromTop (2);

            if (shapingControls->knobs[i] != nullptr)
            {
                const int x = column.getX() + (column.getWidth() - shapingKnobSize) / 2;
                shapingControls->knobs[i]->setBounds (x, column.getY(), shapingKnobSize,
                                                      shapingKnobSize + shapingValueHeight);
            }
        }
    }

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

    // Update All sits on the middle row rather than beside Clear Slot: the upper
    // row is already full, and the name box has width to spare. Same height as
    // every other button, so they read as one set.
    auto middle = controls.removeFromTop (buttonHeight);
    updateAllButton.setBounds (middle.removeFromRight (buttonWidth));
    middle.removeFromRight (buttonGap);
    nameLabel.setBounds (middle.removeFromLeft (42));
    nameEditor.setBounds (middle);

    controls.removeFromTop (8);

    // The two ways of taking the sound back in, on a row of their own at the left
    // edge. They are the only controls here that do not act on the slot on screen
    // -- they MAKE one -- so they are not mixed in among the ones that do.
    auto lower = controls.removeFromTop (buttonHeight);
    resampleButton.setBounds (lower.removeFromLeft (buttonWidth));
    lower.removeFromLeft (buttonGap);
    resampleInitButton.setBounds (lower.removeFromLeft (wideButtonWidth));

    // Loop goes to the far right of this row rather than beside the mode buttons
    // it belongs with: that row is full, and this is the only space left at the
    // height of the controls it answers to.
    loopButton.setBounds (lower.removeFromRight (buttonWidth));

    // Last, because where the picture goes is worked out from the boxes above it.
    positionSampleStrip();
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
void WaveformEditorComponent::paintSampleOutline (juce::Graphics& g, juce::Rectangle<int> area,
                                                  const UserWaveSlot& entry, double fromIndex,
                                                  double toIndex, juce::Colour colour,
                                                  float thickness, Bloom bloom) const
{
    //==========================================================================
    // There are far more samples than there are pixels, so a column cannot show a
    // value -- it can only show a range, and the picture is the shape of the two
    // edges of that range. Both edges: this used to plot the loudest point in each
    // column as a single line, which put a sustained note hard against the top of
    // the box and never brought it back down.
    //
    // Drawn out along the tops and back along the bottoms as one closed path, so
    // it is stroked and bloomed exactly like every other picture here.
    const int columns = area.getWidth();
    const double span = toIndex - fromIndex;

    if (columns < 2 || span <= 0.0 || entry.sample.empty())
        return;

    const float centreY = (float) area.getCentreY();
    const float halfHeight = (float) area.getHeight() * 0.42f;
    const int last = (int) entry.sample.size() - 1;

    juce::Path path;
    std::vector<float> bottoms ((std::size_t) columns, centreY);

    for (int x = 0; x < columns; ++x)
    {
        const double across = (double) x / (double) (columns - 1);
        const int from = (int) std::floor (fromIndex + across * span);
        const int to = juce::jmax (from + 1,
                                   (int) std::floor (fromIndex
                                                     + (across + 1.0 / columns) * span));

        // Anything outside the buffer reads as silence, which is what the gutter
        // in front of the file is: room the marker can be dragged into, drawn as
        // the flat line it will sound like.
        float low = 0.0f;
        float high = 0.0f;

        for (int i = juce::jmax (0, from); i < juce::jmin (to, last + 1); ++i)
        {
            const float value = entry.sample[(std::size_t) i];
            low = juce::jmin (low, value);
            high = juce::jmax (high, value);
        }

        const float px = (float) (area.getX() + x);

        if (x == 0)
            path.startNewSubPath (px, centreY - high * halfHeight);
        else
            path.lineTo (px, centreY - high * halfHeight);

        bottoms[(std::size_t) x] = centreY - low * halfHeight;
    }

    for (int x = columns - 1; x >= 0; --x)
        path.lineTo ((float) (area.getX() + x), bottoms[(std::size_t) x]);

    path.closeSubPath();

    strokeWaveformPath (g, path, colour, thickness, bloom);
}

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

    //==========================================================================
    // A pitched whole sample is drawn as its SHAPE: a few cycles of it, taken from
    // a settled part of the note and scaled to fill the box.
    //
    // Which is what the window is for. Drawn over its whole length instead, a
    // two-second note is one filled rectangle -- there are five hundred cycles
    // behind every pixel, so the only thing a column can show is that the sound
    // was loud, which the player already knew (Giuseppe, 2026-08-13).
    //
    // Scaled to its own loudest point rather than to the file's, so the shape is
    // just as readable taken from the quiet tail of a pluck as from the front of
    // it. This is a picture of a SHAPE; how loud the sound is at that moment is
    // not what it is being asked.
    if (entry != nullptr && entry->mode == UserWave::Mode::FullSample && drawsCycles (*entry))
    {
        const double period = entry->fileSampleRate / entry->fundamentalHz;
        const int span = juce::jmax (2, (int) (period * juce::jmax (1, repeats)));
        const int total = (int) entry->sample.size();

        // Taken from the part that PLAYS, and from the recorded part of it -- not
        // from a fifth of the way into the buffer. A trimmed sample would
        // otherwise be drawn as a stretch of itself the player has cut out, and
        // one with silence in front of it would be drawn as the silence.
        const int from = juce::jmax (entry->playStart, entry->fileOffset());
        const int to = juce::jmin (entry->playStart + entry->playLength,
                                   entry->fileOffset() + entry->fileLength);

        // A fifth of the way in: past the attack of anything that has one, and
        // still well inside the shortest sample worth drawing.
        int start = juce::jlimit (0, juce::jmax (0, total - span - 1),
                                  from + juce::jmax (0, to - from) / 5);

        float loudest = 0.0f;

        for (int i = start; i < start + span && i < total; ++i)
            loudest = juce::jmax (loudest, std::abs (entry->sample[(std::size_t) i]));

        // Nothing there to draw -- a silent stretch. Fall through to the outline,
        // which at least shows where the sound is.
        if (loudest > 1.0e-5f)
        {
            const float scale = 1.0f / loudest;
            const int width = area.getWidth();

            for (int x = 0; x < width; ++x)
            {
                const double across = (double) x / (double) juce::jmax (1, width - 1);
                const int index = juce::jlimit (0, total - 1, start + (int) (across * span));

                const float y = centreY - entry->sample[(std::size_t) index] * scale * halfHeight;
                const float px = (float) (area.getX() + x);

                if (x == 0)
                    path.startNewSubPath (px, y);
                else
                    path.lineTo (px, y);
            }

            strokeWaveformPath (g, path, colour, strokeThickness, bloom);
            return;
        }
    }

    //==========================================================================
    // An unpitched whole sample is drawn as its OUTLINE -- the whole buffer, end
    // to end, with no gutter. The gutter belongs to the picture the markers are
    // dragged on; a thumbnail in the list has no markers on it.
    if (entry != nullptr && entry->mode == UserWave::Mode::FullSample)
    {
        paintSampleOutline (g, area, *entry, 0.0, (double) entry->sample.size(),
                            colour, strokeThickness, bloom);
        return;
    }

    const int width = area.getWidth();

    for (int x = 0; x < width; ++x)
    {
        const double across = (double) x / (double) (width - 1);
        float value = 0.0f;

        // Everything that is left is a single cycle: a built-in shape, or a slot
        // holding one period. A whole sample never reaches here -- see the outline
        // above -- because a cycle has one value per column and a sample has not.
        if (entry == nullptr)
        {
            value = builtInShapeValue (row, across * repeats);
        }
        else if (! entry->tables.empty())
        {
            // Drawn at full bandwidth, so the shape on screen is the shape that
            // was imported and not the reduced version a high note would play.
            double phase = across * repeats;
            phase -= std::floor (phase);

            const int index = juce::jlimit (0, WaveAnalysis::tableSize - 1,
                                            (int) (phase * WaveAnalysis::tableSize));
            value = entry->tables[(std::size_t) index];
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

bool WaveformEditorComponent::drawsCycles (const UserWaveSlot& slot)
{
    if (slot.sample.empty() || slot.fileSampleRate <= 0.0 || slot.fundamentalHz <= 20.0)
        return false;

    // Trusted by the detector, or known because the plugin played the note itself.
    if (! (slot.retuned || ! slot.allowRetune))
        return false;

    // And long enough to hold a cycle worth looking at. Four samples is the floor
    // below which a period is a zigzag rather than a shape.
    return slot.fileSampleRate / slot.fundamentalHz >= 4.0;
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

    // Rows are drawn at a scrolled offset, so one at either end is partly outside
    // the list box. Clipped to the box, or a half-scrolled row would spill over
    // the group's border and into the panel around it.
    auto clip = listBounds().reduced (groupPadding, 0);
    clip.removeFromTop (groupTitleInset);
    clip.removeFromBottom (groupPadding);

    juce::Graphics::ScopedSaveState saved (g);
    g.reduceClipRegion (clip);

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
void WaveformEditorComponent::paintRecording (juce::Graphics& g, juce::Rectangle<int> area)
{
    const float done = juce::jlimit (0.0f, 1.0f,
                                     resampleHost != nullptr ? resampleHost->captureProgress()
                                                             : 0.0f);

    //==========================================================================
    // The oscilloscope's box, the same one the waveform is drawn in, because this
    // IS that waveform being made.
    g.setColour (backgroundNavy);
    g.fillRoundedRectangle (area.toFloat(), 4.0f);

    g.setColour (toggleBorder);
    g.drawRoundedRectangle (area.toFloat(), 4.0f, 1.0f);

    auto middle = area.withSizeKeepingCentre (juce::jmin (area.getWidth() - 40, 300), 60);

    g.setColour (labelCyan);
    g.setFont (lookAndFeel.getBodyFont (13.0f, true));
    g.drawText ("Recording " + juce::String (UserWave::groupName (group)) + "...",
                middle.removeFromTop (20), juce::Justification::centred, false);

    middle.removeFromTop (6);

    //==========================================================================
    // The bar itself: the same navy trough, border and cyan fill as a toggle, so
    // it belongs to this panel rather than being a progress bar from elsewhere.
    auto trough = middle.removeFromTop (14).toFloat();
    constexpr float corner = 4.0f;

    g.setColour (toggleNavy);
    g.fillRoundedRectangle (trough, corner);

    if (done > 0.0f)
    {
        auto filled = trough.withWidth (juce::jmax (corner * 2.0f, trough.getWidth() * done));

        g.setColour (toggleLitNavy);
        g.fillRoundedRectangle (filled, corner);

        g.setColour (knobArcCyan.withMultipliedAlpha (0.9f));
        g.fillRoundedRectangle (filled.reduced (2.0f), corner - 1.0f);

        // The bloom every lit control on this panel carries, so the bar lights up
        // as it fills instead of being a flat block of colour. AFTER the fill, not
        // before -- a glow drawn first is painted straight over. And the meter is
        // read before the output is silenced, so this still breathes with the
        // sound being recorded even though none of it is heard.
        lookAndFeel.glowAround (g, filled.expanded (2.0f), corner + 1.0f,
                                lookAndFeel.getMeterResponsiveKnobArcColour());
    }

    g.setColour (toggleBorder);
    g.drawRoundedRectangle (trough.reduced (0.5f), corner, 1.0f);

    middle.removeFromTop (8);

    // Why the speakers are silent, said before the player has time to wonder.
    g.setColour (dimText);
    g.setFont (lookAndFeel.getBodyFont (11.5f, false));
    g.drawText ("Silent while it records. It stops when the sound has died away.",
                middle.removeFromTop (16), juce::Justification::centred, true);
}

//==============================================================================
void WaveformEditorComponent::paintDetail (juce::Graphics& g)
{
    auto content = detailBounds().reduced (groupPadding, 0);
    content.removeFromTop (groupTitleInset);
    content.removeFromBottom (groupPadding);
    content.removeFromBottom (controlStripHeight);   // the strip laid out in resized()

    // A recording takes the whole panel over while it runs. It is going to
    // overwrite whatever is on screen here in a moment, so showing that thing
    // meanwhile would only be showing the player what they are about to lose.
    if (isResampling())
    {
        paintRecording (g, content);
        return;
    }

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
        else if (! entry->allowRetune)
        {
            // A resample. It is not re-tuned, but that is not the same fault the
            // amber line below reports -- it is how a resample is made to come
            // back out exactly as it went in.
            note = "Recorded from the synth on middle C. Middle C plays it back "
                   "exactly as it was; the keys either side transpose it.";
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

    // Where the markers stand, in the time the player reads off the picture, at
    // the other end of the same line. The picture says WHERE they are; this says
    // how far in, which is the number they need to match a sample to a bar.
    if (entry != nullptr && entry->mode == UserWave::Mode::FullSample && entry->fileLength > 0)
    {
        int start = 0;
        int end = 0;
        trimPoints (*entry, start, end);

        juce::String marks;

        if (start < 0)
            marks << "+" << timeLabel (*entry, (double) -start) << " silence   ";

        marks << timeLabel (*entry, (double) juce::jmax (0, start)) << " to "
              << timeLabel (*entry, (double) juce::jmin (end, entry->fileLength));

        if (end > entry->fileLength)
            marks << "   silence +" << timeLabel (*entry, (double) (end - entry->fileLength));

        g.setColour (trimDrag != TrimHandle::None ? knobArcCyan : dimText);
        g.setFont (lookAndFeel.getBodyFont (11.5f, false));
        g.drawText (marks, readout, juce::Justification::centredRight, true);
    }

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

    // Two periods wherever a period is being drawn -- a built-in shape, a single
    // cycle, or a pitched sample -- so every one of them reads as repeating. One
    // pass for a sample drawn as its outline, because that is the thing itself.
    const bool wholeSample = (entry != nullptr && entry->mode == UserWave::Mode::FullSample);
    const bool asOutline = wholeSample && ! drawsCycles (*entry);

    //==========================================================================
    // A pitched sample gets BOTH pictures, because they answer different
    // questions and the player has both: the shape, large, which is what the
    // oscillator sounds like -- and under it a strip showing the whole recording
    // end to end, which is where its attack, its tail and its silence are. One
    // without the other is what made this window feel wrong twice over: the
    // outline alone is a featureless block, and the shape alone hides whether the
    // sound ever died away (Giuseppe, 2026-08-14).
    // Everything showing the WHOLE file -- the strip under a shape, or the single
    // outline an unpitched sample gets -- now belongs to the SampleStrip. It is a
    // component of its own so that the playhead can move without dragging this
    // paint along behind it, and it draws its own background, its own loop marks
    // and the line. All that is left here is the shape above it.
    const auto stripArea = wholeFileBounds();

    if (asOutline && ! stripArea.isEmpty())
        return;                     // the strip IS the picture: nothing above it

    auto shape = content;

    if (! stripArea.isEmpty())
    {
        shape.setBottom (stripArea.getY() - 1);

        g.setColour (toggleBorder.withAlpha (0.7f));
        g.drawHorizontalLine (stripArea.getY() - 1, (float) content.getX() + 2.0f,
                              (float) content.getRight() - 2.0f);
    }

    g.setColour (toggleBorder);
    g.drawHorizontalLine (shape.getCentreY(), (float) content.getX() + 2.0f,
                          (float) content.getRight() - 2.0f);

    // The EQ curve's own weight and its own spread -- one open curve alone in a
    // box, which is exactly what that treatment was drawn for.
    paintRowWaveform (g, shape.reduced (6, 8), row, traceColour(), 1.8f,
                      asOutline ? 1 : 2, Bloom::Wide);
}

//==============================================================================
WaveformEditorPanel::WaveformEditorPanel (UserWaveLibrary& libraryToUse,
                                          SpaceDustLookAndFeel& lookAndFeelToUse)
    : library (libraryToUse), lookAndFeel (lookAndFeelToUse)
{
    // Opaque so JUCE paints this rectangle alone. A non-opaque child makes its
    // parent repaint first, which here means the whole plugin behind it -- the
    // same reason SampleStrip is opaque, for the same cost.
    setOpaque (true);

    content = std::make_unique<WaveformEditorComponent> (library, lookAndFeel);
    addAndMakeVisible (*content);

    closeButton.setLookAndFeel (&lookAndFeel);
    closeButton.onClick = [this] { hidePanel(); };
    addAndMakeVisible (closeButton);

    setVisible (false);
}

WaveformEditorPanel::~WaveformEditorPanel()
{
    // A panel destroyed while open must leave no listener behind on a parent
    // that outlives it.
    if (auto* parent = getParentComponent(); parent != nullptr && watchingOutsideClicks)
        parent->removeMouseListener (&outsideClicks);

    // The audio thread must stop being asked for a position nobody will read.
    // Before the content goes, because it is the content that carries the word.
    if (content != nullptr)
        content->setPlayheadActive (false);

    closeButton.setLookAndFeel (nullptr);
}

void WaveformEditorPanel::paint (juce::Graphics& g)
{
    auto area = getLocalBounds().toFloat();

    g.setColour (juce::Colour (0xff0a0a1f));
    g.fillRoundedRectangle (area, 6.0f);

    g.setColour (juce::Colour (0xff00d4ff));
    g.drawRoundedRectangle (area.reduced (0.5f), 6.0f, 1.5f);

    g.setColour (juce::Colour (0xffa0d8ff));
    g.setFont (lookAndFeel.getBodyFont (13.0f, true));
    g.drawText ("Waveforms",
                getLocalBounds().removeFromTop (titleHeight).reduced (frameInset, 0),
                juce::Justification::centredLeft, false);
}

void WaveformEditorPanel::resized()
{
    auto area = getLocalBounds();
    auto titleRow = area.removeFromTop (titleHeight);

    closeButton.setBounds (titleRow.removeFromRight (titleHeight + frameInset)
                                   .reduced (4, 3));

    if (content != nullptr)
        content->setBounds (area.reduced (frameInset, 0)
                                .withTrimmedBottom (frameInset));
}

void WaveformEditorPanel::mouseDown (const juce::MouseEvent&)
{
    // Deliberately empty. A press that reached the panel is a press inside it,
    // and swallowing it here is what stops OutsideClickWatcher reading it as a
    // press somewhere else.
}

bool WaveformEditorPanel::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress::escapeKey)
    {
        hidePanel();
        return true;
    }

    return false;
}

void WaveformEditorPanel::OutsideClickWatcher::mouseDown (const juce::MouseEvent& event)
{
    if (! owner.isVisible())
        return;

    // Anything that happened inside the panel, or inside anything the panel owns,
    // belongs to the panel.
    if (event.eventComponent == &owner || owner.isParentOf (event.eventComponent))
        return;

    owner.hidePanel();
}

void WaveformEditorPanel::allowFileDropsFromLowerPrivilege()
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

void WaveformEditorPanel::showFor (juce::Component* anchorButton,
                                   juce::ComboBox* targetCombo, int userBase,
                                   WaveformEditorComponent::BuiltInKind kind,
                                   UserWave::Group group, int slotIndex,
                                   const WaveformEditorComponent::ShapingControls* shaping)
{
    if (content == nullptr)
        return;

    content->setTarget (targetCombo, userBase, kind, group, shaping);

    // Only jump to a slot that has something in it. Opening the panel must never
    // change the sound by itself.
    if (slotIndex >= 0)
        content->selectSlot (slotIndex);

    // The list that opened this may be longer or shorter than the last one, so
    // the panel is sized from the list rather than left at whatever the previous
    // one needed.
    const int width = WaveformEditorComponent::preferredWidth() + frameInset * 2;
    const int height = WaveformEditorComponent::preferredHeight (userBase, shaping != nullptr)
                     + titleHeight + frameInset;

    setSize (width, height);

    // Placed against the button that opened it, then pushed back inside. The
    // clamp is not optional: the Sub Oscillator button sits low and right in the
    // layout, and a panel hung off it would run off two edges at once.
    if (auto* parent = getParentComponent())
    {
        juce::Point<int> topLeft (0, 0);

        if (anchorButton != nullptr)
        {
            const auto anchor = parent->getLocalArea (anchorButton,
                                                      anchorButton->getLocalBounds());
            topLeft = { anchor.getX(), anchor.getBottom() + 4 };
        }

        const int maxX = juce::jmax (0, parent->getWidth() - width);
        const int maxY = juce::jmax (0, parent->getHeight() - height);

        setTopLeftPosition (juce::jlimit (0, maxX, topLeft.x),
                            juce::jlimit (0, maxY, topLeft.y));
    }

    setVisible (true);
    toFront (true);

    content->refresh();

    // Open, so it follows what is playing again. Paired with hidePanel and the
    // destructor; between them the synth is only ever asked for a position while
    // there is a panel on screen to draw it on.
    content->setPlayheadActive (true);

    // Listen for presses anywhere else in the plugin, so one puts the panel away.
    if (auto* parent = getParentComponent(); parent != nullptr && ! watchingOutsideClicks)
    {
        parent->addMouseListener (&outsideClicks, true);
        watchingOutsideClicks = true;
    }

    setWantsKeyboardFocus (true);
    grabKeyboardFocus();

    // The panel no longer owns a window, so the drop lands on whatever window the
    // host gave us. Ask that window to let it through. Here rather than in the
    // constructor: a panel that has never been shown has no peer to reach, and a
    // host may hand us a different window between one opening and the next.
    allowFileDropsFromLowerPrivilege();
}

void WaveformEditorPanel::hidePanel()
{
    if (! isVisible())
        return;

    if (content != nullptr)
        content->setPlayheadActive (false);

    // A gesture that never got its mouse-up -- the panel closed mid-drag -- would
    // otherwise hold the slot audio and never write the index file.
    juce::String ignored;
    library.endTrimSession (ignored);

    if (auto* parent = getParentComponent(); parent != nullptr && watchingOutsideClicks)
    {
        parent->removeMouseListener (&outsideClicks);
        watchingOutsideClicks = false;
    }

    setVisible (false);
}

void WaveformEditorPanel::setResampleHost (WaveformEditorComponent::ResampleHost* host)
{
    if (content != nullptr)
        content->setResampleHost (host);
}

void WaveformEditorPanel::refreshContent()
{
    if (content != nullptr)
        content->refresh();
}

void WaveformEditorPanel::repaintContent()
{
    if (content != nullptr)
        content->repaint();
}

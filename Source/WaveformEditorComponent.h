#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "PhaseShaper.h"
#include "SpaceDustLookAndFeel.h"
#include "UserWavetable.h"

#include <functional>
#include <vector>

/**
    The Waveforms window -- the waveform list, and where a sample joins it.

    Opened by the small button beside each of the five waveform dropdowns. The
    list it shows IS that dropdown's list, in the same order: the built-in shapes
    first, then that dropdown's own eight import slots. Clicking any row selects
    it, exactly as choosing it from the dropdown would.

    Each of the five dropdowns keeps its own eight slots, so loading a sample
    here changes one waveform and no other. Update All is the one control that
    reaches across: it puts the slot on screen into the same numbered slot of all
    five lists.

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
                                public juce::FileDragAndDropTarget,
                                private juce::MultiTimer
{
public:
    /** What sits above the import slots in the list that opened this window.

        Only drawing depends on it. An imported row is drawn from what was
        imported, but a built-in row has no data behind it, so its picture has to
        be known in advance -- and the four oscillator shapes, the two noise
        colours and the ten drums are three different kinds of picture.

        Told to the window rather than worked out from userBase, so that a fourth
        list can never quietly inherit the wrong drawing by happening to have the
        same number of built-ins as one of these. */
    enum class BuiltInKind
    {
        Shapes,   // Sine, Triangle, Saw, Square -- the oscillators and the sub
        Noise,    // White and Pink
        Drums     // the ten 808 and 909 hits
    };

    /** The shaping knobs for the oscillator whose list is on screen.

        Bend +, Bend -, Bend +/- , Spectrum and Sync. They act on the OSCILLATOR,
        not on the slot, so turning one changes every waveform that oscillator
        plays and not just the one selected -- which is why they sit in a strip of
        their own at the foot of the panel rather than among the controls that
        edit a slot.

        Owned by the editor, like targetCombo, because they are attached to its
        parameters and this window knows nothing about a processor. Borrowed here
        and given back when the panel is pointed at another list.

        Null for a list that has no shaping: the noise source and the Transient,
        neither of which has a repeating cycle to bend. The strip then takes no
        room at all. */
    struct ShapingControls
    {
        static constexpr int numKnobs = 5;

        juce::Slider* knobs[numKnobs] = {};
        juce::Label* labels[numKnobs] = {};

        /** Voices, Detune and Width -- the unison box beside the shaping one.

            A second group rather than three more knobs in the same box: unison
            makes SEVERAL of the oscillator, where Bend and Spectrum change the
            one. Kept in the same struct because they are borrowed and given back
            together, on the same gesture, for the same oscillator. */
        static constexpr int numUnisonKnobs = 4;

        juce::Slider* unisonKnobs[numUnisonKnobs] = {};
        juce::Label* unisonLabels[numUnisonKnobs] = {};
    };

    WaveformEditorComponent (UserWaveLibrary& library, SpaceDustLookAndFeel& lookAndFeel);
    ~WaveformEditorComponent() override;

    //==========================================================================
    void paint (juce::Graphics&) override;
    void resized() override;

    /** Selecting a row, and grabbing one of the two markers on the whole-file
        picture. The markers come first: a press that lands on one is a drag, not
        a choice of waveform. */
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;

    /** Scroll the list. Twenty-one built-in shapes plus eight import slots is
        more rows than the panel can show at once, so the list scrolls past its
        box rather than the panel growing taller than the plugin. */
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    /** Both markers back to the ends of the file. The one gesture that undoes a
        trim without having to drag each marker back by hand. */
    void mouseDoubleClick (const juce::MouseEvent&) override;

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
        many built-in entries it has in front of them, kind says what those
        entries are so they can be drawn, and group says whose eight import slots
        are being shown -- every import made here goes into that list alone. */
    void setTarget (juce::ComboBox* targetCombo, int userBase, BuiltInKind kind,
                    UserWave::Group group, const ShapingControls* shaping = nullptr);

    /** Show and select the given import slot. */
    void selectSlot (int slotIndex);

    /** Start or stop following what the synth is playing.

        Called by the window as it is shown and hidden, and nothing else here
        depends on it -- so a closed window costs exactly nothing: no timer, no
        repaint, and nothing published from the audio thread either. */
    void setPlayheadActive (bool shouldFollow);

    //==========================================================================
    // -- Resample --
    //
    // The window still decides nothing about audio. It knows that a resample is a
    // lump of mono samples with a rate and a level, that asking for one takes a
    // few seconds, and that a patch can be stripped back around the result. WHERE
    // the samples come from and WHAT stripping a patch back means both live in
    // the editor, next to the processor and the parameters.

    /** What one press of Resample brings back. */
    struct Capture
    {
        /** Both channels, at sampleRate, and the same length as each other.

            Still called `mono` on the left because everything downstream that
            only wants one channel wants that one, and a resample of a patch with
            no width in it gives two identical channels. */
        std::vector<float> mono;
        std::vector<float> right;
        double sampleRate = 0.0;

        /** How loud it was BEFORE it was normalised on the way into the slot.
            Resample + Init needs it to put the level back. */
        float peak = 0.0f;

        /** What to call the slot -- the patch the sound came out of. */
        juce::String name;

        /** Whether the tail was still sounding when the recording ran out of
            room. The player is the only one who can shorten a reverb, so they
            are told rather than left with a sample that ends in mid-air. */
        bool cutShort = false;
    };

    /** Everything the window needs from the synth: to record it, to strip it back
        around what it recorded, and to know where it has got to in a sample.

        An interface rather than a handful of callbacks, because they are one job
        and are all answered by the same object. Implemented by the editor, which
        owns this window and so outlives it. */
    struct ResampleHost
    {
        virtual ~ResampleHost() = default;

        /** Ask the synth to play a middle C and record what comes out. It runs on
            the audio thread and takes seconds, so this returns at once. False,
            with a line for the player, when it cannot be started. */
        virtual bool startCapture (juce::String& errorMessage) = 0;

        /** Whether that recording is still running. */
        virtual bool captureIsRunning() const = 0;

        /** How far it has got, 0 to 1, for the bar the player watches. */
        virtual float captureProgress() const = 0;

        /** Take the finished recording. False when it caught no sound. */
        virtual bool collectCapture (Capture& capture, juce::String& errorMessage) = 0;

        /** Give up on a recording that can never finish. */
        virtual void abandonCapture() = 0;

        /** How far through this list's sample the synth has got, 0 to 1, or a
            negative number when nothing is playing one. Drives the playhead.

            Whatever is being played right now, not what is selected in the
            window: the two are the same while the player is auditioning, and
            when they are not, what is sounding is the truthful answer. */
        virtual float playbackPhase (UserWave::Group group) const = 0;

        /** Whether anybody is watching that. False while the window is shut, and
            the synth then publishes nothing at all -- the point of asking. */
        virtual void setPlaybackPhaseWanted (bool wanted) = 0;

        /** Strip the patch back to nothing but the waveform just made: every
            effect off, the filter open, the envelope out of the way, and this one
            waveform selected in the list it went into.

            Given the choice index rather than being left to move the dropdown, so
            the selection is made through the PARAMETER -- the same reset that
            turns the effects off puts every waveform dropdown back to its
            default, and a selection made any other way would be undone by it. */
        virtual void initialiseAroundWaveform (UserWave::Group group, int choiceIndex,
                                               float peak) = 0;
    };

    /** Point the window at the synth it is to resample. Null in any host that has
        not set one, which is what greys the two buttons out. */
    void setResampleHost (ResampleHost* host);

    /** Rebuild every control from the library and the target dropdown. */
    void refresh();

    /** Widest the window ever needs to be, so it does not change width when
        opened from a dropdown with a different number of built-in entries. */
    static int preferredWidth();

    /** Tall enough for every row this list could ever hold: its built-ins plus
        all eight import slots.

        Height DOES follow the list, unlike width. The oscillators have four
        built-in shapes and the Transient has ten, and a window sized for ten
        would stand two thirds empty every time it was opened on an oscillator.
        The space under the last row is not wasted -- it is the drop zone. */
    static int preferredHeight (int userBase, bool withShaping = false);

private:
    //==========================================================================
    void importInto (int slotIndex, const juce::File& file);
    void browseForFile();
    void applyMode (UserWave::Mode mode);

    /** Both Resample buttons. alsoInitialise is the only difference between them:
        the sound is recorded, stored and selected either way, and stripping the
        patch back happens in between -- after the slot exists, before it is
        chosen.

        The recording takes seconds, so this only starts it. The rest happens in
        timerCallback when the synth says it has finished. */
    void beginResample (int slotIndex, bool alsoInitialise);

    /** Which clock has ticked. The two run at very different rates and only one
        of them ever runs at all: see resampleTimerId and playheadTimerId. */
    void timerCallback (int timerID) override;

    /** Build the finished recording into the waiting slot.

        Always Full Sample. A resample is a whole note -- attack, tail, effects
        and all -- and one period out of the middle of that is not what the player
        pressed the button for. The Single Cycle button is right there
        afterwards. */
    void completeResample();

    /** End a resample that cannot go on, and say why. */
    void failResample (const juce::String& message);

    /** Whether a recording is running, which is what disables most of the panel
        while one is. */
    bool isResampling() const noexcept { return pendingSlot >= 0; }

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

    //==========================================================================
    // -- Scrolling the list --
    //
    // The list used to fit whatever it held, because the longest one was the
    // Transient's ten drums and eight slots. An oscillator list now holds
    // twenty-one built-in shapes and can reach twenty-nine rows, which would ask
    // for a panel taller than the plugin -- and clamping such a panel to fit
    // would push its bottom rows and its status line off the edge for good.
    //
    // So the panel stops growing at maxPanelHeight and the list scrolls inside
    // it. The offset is applied in rowBounds() alone, which is what keeps
    // hit-testing and drawing in step.

    /** As tall as the panel is ever allowed to be, in design pixels.

        The plugin's controls occupy 857 design pixels and the standalone adds a
        keyboard strip below that. The panel is placed against the Edit button and
        then clamped inside its parent -- and its parent includes the keyboard --
        so a panel tall enough could have its foot pushed down among the keys.

        The shaping knobs live in that foot. 690 leaves the whole panel, frame and
        all, inside the controls above the keyboard however low the button that
        opened it sits. */
    static constexpr int maxPanelHeight = 690;

    /** How tall the rows are all together, and how much of that can be seen. */
    int listContentHeight() const;
    int listViewHeight() const;
    int maxListScroll() const;

    /** Set the scroll, clamped, and repaint if it moved. */
    void setListScroll (int newScroll);

    /** Pull the scroll back inside what the list can actually scroll.

        This panel is ONE component that all five Edit buttons re-point, and
        listScroll is its member. Every route that changes how much there is to
        scroll has to come through here afterwards, or a scroll set for one list
        outlives it into the next:

          * a different source, with a different number of built-in rows and a
            different height,
          * a slot filled or cleared, which adds or removes a row,
          * a resize, which changes how much of the list can be seen.

        The stuck scroll cannot free itself, which is what made this worth its own
        function rather than a line in one place. mouseWheelMove gives up early
        when there is nothing to scroll, so a short list holding a tall list's
        scroll offset can never be scrolled back -- its rows are simply above the
        top of the box with no way to reach them (Giuseppe, 2026-08-26: the
        Transient list, which is the shortest and so the one where it bites). */
    void clampListScroll();

    /** Bring a row into view. Called when the selection moves by any route other
        than a click -- opening the panel on a slot, or stepping off one that was
        cleared -- because a selected row nobody can see reads as no selection. */
    void scrollRowIntoView (int row);

    /** How far the list is scrolled, in pixels. */
    int listScroll = 0;

    /** How tall the dashed drop box at the foot of the list is.

        PINNED, not scrolled. The rows scroll in the space above it and it never
        moves, so the invitation to drag a sample in is on screen whatever the
        list is showing -- which it was not when it lived under the last row,
        twenty-one shapes down. */
    static constexpr int dropGhostHeight = 62;

    /** Where the rows are allowed to be: the list box less the pinned drop box.

        One place, so the clip that draws them, the hit test that finds them and
        the scroll that limits them all agree about where the rows end. */
    juce::Rectangle<int> rowsViewArea() const;

    /** An arrow coming down into a waveform, drawn inside the drop box.

        The gesture and its result in one picture, because the box has to say
        what it is for before anyone reads it -- and there is room beside it for
        one short line of words and no more. */
    void paintDropIcon (juce::Graphics&, juce::Rectangle<int> area, juce::Colour) const;

    //==========================================================================
    // -- Making room for a dropped file --
    //
    // While a file is dragged over the list, the rows from the landing place
    // downwards slide out of the way and leave a gap the width of a row. The gap
    // is not decoration: it opens at the slot the file will ACTUALLY go into, so
    // the panel answers "where will this land" before the button is let go.
    //
    // Hovering an import slot means that slot -- a drop there replaces it.
    // Hovering anywhere else means the first free slot, which is where a drop
    // outside the slots goes. See filesDropped.

    /** How far open the gap is, 0 to 1. Eased on a timer so the rows slide
        rather than jump. */
    float dragGap = 0.0f;

    /** The display position the gap opens above, or -1 for none. A position, not
        a row, because that is what the geometry below is written in. */
    int dragInsertPosition = -1;

    /** Where a file would land if it were dropped at this point: the display
        position of the slot that would take it.

        Worked out WITHOUT the gap. Asking rowAt() would read the rows where the
        gap has already pushed them, so the answer would move the gap, which
        would move the answer. */
    int dropPositionAt (juce::Point<int> position) const;

    /** Open or close the gap for a drag at this point. */
    void updateDragGap (juce::Point<int> position, bool dragging);

    //==========================================================================
    // -- The shaping strip --

    /** How tall the strip of shaping knobs is: a label, a knob and its value
        box. Zero when the list on screen has no shaping. */
    static constexpr int shapingKnobSize = 40;
    static constexpr int shapingValueHeight = 16;
    static constexpr int shapingLabelHeight = 14;
    static constexpr int shapingStripPad = 6;

    /** How far a strip label may spill either side of its own knob column.

        Measured against the widest label there is: "Random Phase" is 79 px in the
        panel's own Arial Bold 12, and a unison column is 66. Twelve a side takes
        the label box to 90, which fits it with room to spare, and still leaves a
        clear gap to the neighbouring label because both are centred and the
        neighbours are short (measured, tools/unisonaudit). */
    static constexpr int labelOverhang = 12;

    /** Room for the group box's own title above the knobs. */
    static constexpr int shapingTitleInset = 22;

    static constexpr int shapingStripHeight = shapingTitleInset + shapingLabelHeight + 2
                                            + shapingKnobSize + shapingValueHeight
                                            + shapingStripPad;

    /** The box around the five shaping knobs.

        They are the one group of controls here that does not act on the slot on
        screen -- they act on the oscillator, and every waveform it plays goes
        through them. A box of their own says that, where a bare row of knobs
        under the list read as five more things belonging to the selected row. */
    juce::GroupComponent shapingGroup;

    /** The box around Voices, Detune and Width. Shown on exactly the lists that
        show the shaping box -- the two oscillators, and nothing else. */
    juce::GroupComponent unisonGroup;

    /** How the foot of the panel is split between the two boxes: five knobs for
        shaping and three for unison, so the shaping box takes the larger share
        and both hold knobs of the same size. */
    static constexpr int shapingKnobCount = ShapingControls::numKnobs;
    static constexpr int unisonKnobCount = ShapingControls::numUnisonKnobs;

    /** Take the borrowed knobs as children, or give the last set back. Called
        from setTarget, which is the only thing that changes which list is shown. */
    void adoptShapingControls (const ShapingControls* shaping);

    /** Whether the borrowed controls actually carry any of the five shaping
        knobs.

        The noise source borrows a ShapingControls that holds only the three
        unison knobs -- it has unison but nothing to bend -- so "there is a strip"
        and "there is a Wave Shaping box in it" stopped being the same question
        the moment that existed. */
    bool hasShapingKnobs() const noexcept;

    /** What the five knobs are set to right now.

        Read from the sliders rather than kept in a member, because the sliders are
        the truth: a parameter changed by automation, by a preset or by the host
        moves them, and nothing here would be told. All zero when this list has no
        shaping, so a picture drawn without knobs is the plain shape.

        The knobs' own order is fixed by the editor that builds them -- Bend +,
        Bend -, Bend +/-, Spectrum, Sync -- and this is the one place that
        knowledge is written down. */
    PhaseShaper::Amounts currentShaping() const;

    /** The knobs on screen, or null when this list has none. Borrowed, never
        owned -- see ShapingControls. */
    const ShapingControls* shapingControls = nullptr;

    /** Where a dropped file goes when it does not land on an import row: the slot
        being shown, or the first empty one. Never a built-in. */
    int fallbackImportSlot() const;

    juce::Rectangle<int> rowBounds (int row) const;
    juce::Rectangle<int> listBounds() const;
    juce::Rectangle<int> detailBounds() const;

    /** The box the big picture is drawn in. */
    juce::Rectangle<int> pictureBounds() const;

    /** Where the whole-file picture goes, which is the only place a playhead or a
        loop mark means anything: the strip under the shape when a pitched sample
        shows both pictures, the whole box when an unpitched one shows only its
        outline, and nothing at all for a single cycle or a built-in shape.

        This is the SampleStrip's bounds. Empty means there is no such picture and
        the strip is hidden. */
    juce::Rectangle<int> wholeFileBounds() const;

    /** Draw the whole recording end to end, with its start and end markers.
        Handed to the SampleStrip, which caches what it returns. */
    void paintWholeFilePicture (juce::Graphics&, juce::Rectangle<int> area) const;

    //==========================================================================
    // -- The start and end markers --
    //
    // Two vertical lines on the whole-file picture that say where the sound
    // begins and where it ends. Dragging them is how a sample is topped and
    // tailed: past the front of a hit, before a tail that runs on too long.
    //
    // The start marker also goes BACKWARDS, off the front of the file and into a
    // gutter kept clear in front of it, and there it means silence -- the sample
    // still starts when the marker says, there is simply nothing there until it
    // does. That gutter is the only reason the picture is not just the file: with
    // the file drawn edge to edge there is nowhere to drag the marker TO.
    //
    // Nothing is cut. The slot keeps the whole file whatever the markers say
    // (UserWaveSlot::sample), so the material outside them is still drawn, dimmed,
    // and dragging a marker back out brings it back.

    /** Which marker a drag is moving, if any. */
    enum class TrimHandle
    {
        None,
        Start,
        End
    };

    /** How much room the picture leaves in FRONT of the file, in file samples.

        A quarter of the file's own length, so the gutter is always a visible
        share of the picture whatever the sample is -- and no more than the slot
        has room left for, because silence is stored as samples like everything
        else and comes out of the same fifteen seconds. */
    int padGutter (const UserWaveSlot& entry) const;

    /** Where the two markers stand, in samples of the file, with the end resolved
        to a real position rather than the "to the end" zero the slot stores.

        Answers with the DRAG in progress when there is one. A drag moves a line
        on a picture and nothing else -- the slot is not rebuilt until the mouse
        comes up -- so every part of the drawing has to ask here rather than
        reading the slot. */
    void trimPoints (const UserWaveSlot& entry, int& start, int& end) const;

    /** Where a position in the buffer falls across the picture, 0 to 1.

        The picture's axis is the gutter and then the buffer, so this is not
        simply a fraction of the sample: it is the one place that geometry is
        written down, and the markers, the dimming, the playhead and the mouse all
        go through it. */
    double axisPosition (const UserWaveSlot& entry, double bufferIndex) const;

    /** And back again: the buffer position a point on screen refers to. */
    double bufferIndexAt (const UserWaveSlot& entry, int x) const;

    /** The marker under a point, or None. Within a few pixels either side, so a
        line one pixel wide can still be picked up. */
    TrimHandle handleAt (juce::Point<int> position) const;

    /** Take the drag in progress into the slot, and say what it did. */
    void commitTrim();

    /** A number of file samples as the player reads it: seconds, or milliseconds
        when there are too few seconds to show. */
    juce::String timeLabel (const UserWaveSlot& entry, double samples) const;

    /** Put the strip where it belongs for what is on screen, and redraw it. */
    void positionSampleStrip();

    /** The slot the detail panel is showing, or null for a built-in row. */
    const UserWaveSlot* shownSlot() const;

    /** Put the playhead where the synth has got to, and size it to the picture.
        Does nothing visible when there is no whole-file picture to draw it on. */
    void updatePlayhead();

    void paintList (juce::Graphics&);
    void paintDetail (juce::Graphics&);

    /** The bar a recording is watched by, drawn in the box the waveform is
        normally drawn in -- because it is that waveform being made, and because
        it is the one part of the panel the player is already looking at. */
    void paintRecording (juce::Graphics&, juce::Rectangle<int> area);

    /** How much halo a picture carries.

        Wide is the Final EQ curve's: one open curve, alone in a big box, with
        room for the light to fall off. Tight is the scopes': half the spread,
        for a figure made of many short strokes whose halos would otherwise run
        into each other and merge into a slab of light.

        The size of the picture decides it, not the kind. Eighteen thumbnails
        each wearing the EQ's halo add up to far more glow than the EQ itself
        ever shows, however right any one of them looks on its own. */
    enum class Bloom
    {
        Wide,
        Tight
    };

    /** Whether a whole-sample slot is drawn as its SHAPE or as its OUTLINE.

        A pitched sample has a period, so a few cycles of it can be drawn and the
        picture is a waveform -- the same kind of picture as a built-in shape or a
        single cycle, which is what the player came to this window to look at. A
        two-second note drawn as its outline instead is a filled rectangle, which
        tells them nothing at all (Giuseppe, 2026-08-13).

        An unpitched one -- a drum, a texture, a field recording -- has no period
        to cut at, and a few cycles of it would be a few hundred samples chosen at
        random. That one keeps the outline, where its shape over time is the only
        thing there is to see.

        A resample counts as pitched however the detector read it: the plugin
        played it a middle C itself. See UserWaveSlot::allowRetune. */
    static bool drawsCycles (const UserWaveSlot& slot);

    /** Draw any row's waveform, built-in or imported, into an area. One function
        for both so the list reads as one kind of thing. */
    void paintRowWaveform (juce::Graphics&, juce::Rectangle<int> area, int row,
                           juce::Colour colour, float thickness, int repeats,
                           Bloom bloom) const;

    /** Draw a stretch of a whole sample as its outline -- the shape of the two
        edges of its range, column by column.

        The stretch is given in buffer positions and may reach outside the buffer
        at either end, which is what draws the gutter in front of the file as the
        silence it is rather than leaving it blank. */
    void paintSampleOutline (juce::Graphics&, juce::Rectangle<int> area,
                             const UserWaveSlot& entry, double fromIndex, double toIndex,
                             juce::Colour colour, float thickness, Bloom bloom) const;

    /** Stroke a built picture, with the bloom the rest of the plugin carries.
        One place, so two pictures of the same size cannot end up glowing
        differently. */
    void strokeWaveformPath (juce::Graphics&, const juce::Path& path,
                             juce::Colour colour, float thickness, Bloom bloom) const;

    /** The colour a waveform is drawn in here: the Final EQ curve's cyan, going
        red while the output clips. Read from the LookAndFeel so the two lines
        move together. */
    juce::Colour traceColour() const;

    void setStatus (const juce::String& message, bool isError);

    juce::String rowName (int row) const;
    juce::String rowDetail (int row) const;

    UserWaveLibrary& library;
    SpaceDustLookAndFeel& lookAndFeel;

    juce::ComboBox* targetCombo = nullptr;

    /** Built-in entries in front of the import slots: 4 for the oscillators and
        the sub (Sine, Triangle, Saw, Square), 2 for the noise source (White,
        Pink), 10 for the Transient (the 808 and 909 drums). */
    int userBase = UserWave::oscUserBase;

    BuiltInKind builtInKind = BuiltInKind::Shapes;

    /** Whose eight import slots are on screen. Each of the five dropdowns has its
        own set, so everything this window loads, renames, re-modes or clears
        touches that one list -- except Update All, which is the deliberate way to
        put a slot into all five. */
    UserWave::Group group = UserWave::Group::Osc1;

    /** Import slot the detail panel acts on. Held apart from the row selection so
        that showing a built-in still leaves a sensible target for a dropped file. */
    int activeSlot = 0;

    int dragTargetRow = -1;
    bool dragActive = false;

    /** The marker being dragged, and where the two of them stand while it is.

        Held here rather than written into the slot on every mouse move because
        moving a marker rebuilds the slot, saves the index file and hands the
        audio thread a fresh bank -- once a gesture is right; sixty times a second
        is not. The slot learns about it when the mouse comes up. */
    TrimHandle trimDrag = TrimHandle::None;
    int dragTrimStart = 0;
    int dragTrimEnd = 0;

    /** The synth being resampled, or null where there is none. */
    ResampleHost* resampleHost = nullptr;

    //==========================================================================
    /** The picture of the whole recording, and the line moving across it.

        A component of its own, and an OPAQUE one, and one that keeps its picture
        in an image. All three of those are the same decision: the line has to be
        redrawn sixty times a second, and NOTHING ELSE MAY BE.

          a component   so a frame touches this rectangle instead of the panel.

          opaque        because a see-through one does not achieve that. JUCE has
                        to paint whatever is underneath before it can paint
                        through it -- "non-opaque children require their parent to
                        repaint", as its own test suite puts it -- so a
                        transparent overlay dragged the whole panel along behind
                        the line, nineteen waveform pictures and all, and the line
                        crawled (Giuseppe, 2026-08-14).

          cached        because the picture under the line costs a pass over the
                        entire sample to draw, and it does not change from one
                        frame to the next. It is drawn when the slot, the loop or
                        the size changes; a frame is then a blit and a line.

        Takes nothing from the mouse: a click here belongs to the panel. */
    class SampleStrip : public juce::Component
    {
    public:
        SampleStrip();

        /** What to draw behind the line, in this component's own coordinates.
            Set by the panel, which owns every drawing rule in this window. */
        std::function<void (juce::Graphics&, juce::Rectangle<int> area)> drawPicture;

        /** Draw the picture again. Called when what it shows changes -- never per
            frame. */
        void rebuild();

        /** Where the line goes, 0 to 1 across the picture, or negative for no
            line. Repaints only when it would actually land somewhere else. */
        void setPosition (float newPosition, juce::Colour newColour);

        /** The picture sits inside the component, so the box's border and its
            rounded corners are still the panel's to draw. */
        juce::Rectangle<int> pictureArea() const { return getLocalBounds().reduced (5, 5); }

        void paint (juce::Graphics&) override;
        void resized() override { rebuild(); }

    private:
        juce::Image picture;
        float position = -1.0f;
        juce::Colour colour { 0xff00d4ff };
    };

    SampleStrip sampleStrip;

    /** Whether the window is open and following the synth. */
    bool playheadActive = false;

    /** The slot a running recording is going into, and whether the patch is to be
        stripped back around it when it arrives. -1 when nothing is running. */
    int pendingSlot = -1;
    bool pendingInitialise = false;

    /** How long the recording has been running, in timer ticks, so one that can
        never finish is given up on rather than left waiting for good. */
    int pendingTicks = 0;

    juce::GroupComponent listGroup;
    juce::GroupComponent detailGroup;

    /** The panel's buttons, not JUCE's. Same navy, same border, same
        meter-driven bloom -- see SpaceDustToggleStyleButton. The two mode
        buttons are toggles and light when they are on; the other three are
        momentary and light under the pointer. */
    SpaceDustToggleStyleButton loadButton;
    SpaceDustToggleStyleButton clearButton;
    SpaceDustToggleStyleButton updateAllButton;
    SpaceDustToggleStyleButton resampleButton;
    SpaceDustToggleStyleButton resampleInitButton;

    /** On for a sample that repeats while the key is held, off for one that plays
        once and stops. A toggle, and lit when it is on, like every other pair of
        states on the main panel. Dead in Single Cycle mode, where a repeat is the
        whole idea. */
    SpaceDustToggleStyleButton loopButton;
    SpaceDustToggleStyleButton singleCycleButton;
    SpaceDustToggleStyleButton fullSampleButton;
    juce::TextEditor nameEditor;
    juce::Label nameLabel;

    juce::String statusMessage;
    bool statusIsError = false;

    std::unique_ptr<juce::FileChooser> fileChooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WaveformEditorComponent)
};

//==============================================================================
/**
    The panel the component lives in.

    A plain component, not a DocumentWindow. It is parented to the editor's
    mainView, which lays the whole plugin out in design coordinates and carries
    its single scale transform -- so the panel scales with the window, floats
    over the tab bar, and never wanders off onto the desktop the way a window of
    its own did.

    Hidden rather than deleted when closed, so the selection and the slot it was
    showing survive being shut and reopened. Its position does not survive,
    because it no longer has one of its own: it is placed against the Edit button
    that opened it, every time.

    Opaque. Nothing behind it dims -- that was asked for -- and that is exactly
    why it has to read as solid. A see-through panel over the oscillator section
    would be unreadable.
*/
class WaveformEditorPanel : public juce::Component
{
public:
    WaveformEditorPanel (UserWaveLibrary& library, SpaceDustLookAndFeel& lookAndFeel);
    ~WaveformEditorPanel() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    /** A press that reached the panel is a press INSIDE it. Swallowed so the
        outside-click watcher cannot read it as a press somewhere else. */
    void mouseDown (const juce::MouseEvent& event) override;

    bool keyPressed (const juce::KeyPress& key) override;

    /** Bring the panel up, pointed at the dropdown that asked for it and placed
        against the button that was pressed. */
    void showFor (juce::Component* anchorButton, juce::ComboBox* targetCombo,
                  int userBase, WaveformEditorComponent::BuiltInKind kind,
                  UserWave::Group group, int slotIndex,
                  const WaveformEditorComponent::ShapingControls* shaping = nullptr);

    /** Put the panel away: stop the playhead, and end any drag still open. */
    void hidePanel();

    void setResampleHost (WaveformEditorComponent::ResampleHost* host);

    /** How far down the parent the panel may reach, in the parent's coordinates.

        The standalone's parent runs on below the controls to hold the keyboard.
        Told rather than worked out, because the panel does not know a keyboard
        exists and should not have to. Zero means "the whole parent". */
    void setKeepAboveBottom (int y) { keepAboveBottom = y; }

    /** Redraw after the library changed under it. */
    void refreshContent();

    /** Redraw only, with no rebuild of the controls. The editor's meter timer
        calls this so the panel blooms with the output like the main panel. */
    void repaintContent();

private:
    /** Let a file dragged out of Explorer reach this panel even when the host is
        running with raised privileges.

        Windows blocks messages sent from a lower privilege level to a higher one,
        and a file drop is such a message. Without this, drag and drop silently
        does nothing in an elevated DAW -- no error, no cursor change, nothing --
        while the Load File button keeps working, which is a confusing pair of
        symptoms to be handed.

        When this lived on a window of our own it filtered that window. The panel
        has no window now, so getPeer() walks UP the parent chain to the HOST'S
        window and filters that instead -- which is the window the drop actually
        arrives at. Does nothing on any other platform, and nothing on Windows
        when the process is not elevated. */
    void allowFileDropsFromLowerPrivilege();

    /** Watches every press in the plugin while the panel is open, so one landing
        outside puts the panel away. The panel cannot see those presses itself:
        they land on whatever was clicked. */
    struct OutsideClickWatcher : public juce::MouseListener
    {
        explicit OutsideClickWatcher (WaveformEditorPanel& ownerToUse) : owner (ownerToUse) {}
        void mouseDown (const juce::MouseEvent& event) override;
        WaveformEditorPanel& owner;
    };

    /** How much room the frame takes around the component, in design pixels. */
    static constexpr int frameInset = 8;
    static constexpr int titleHeight = 24;

    UserWaveLibrary& library;
    SpaceDustLookAndFeel& lookAndFeel;

    std::unique_ptr<WaveformEditorComponent> content;
    juce::TextButton closeButton { "X" };

    OutsideClickWatcher outsideClicks { *this };
    bool watchingOutsideClicks = false;

    /** See setKeepAboveBottom. */
    int keepAboveBottom = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WaveformEditorPanel)
};

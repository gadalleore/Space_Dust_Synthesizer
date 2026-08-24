# Waveform editor: live trim, and a panel instead of a window

Date: 2026-08-24
Status: approved, ready for an implementation plan

## Why

Two complaints about the Waveforms editor.

1. Moving a start or end marker does not change what you hear. You must release
   the key and press it again before the new trim sounds.
2. The editor opens as a separate desktop window. It leaves the plugin, and in a
   DAW it floats away from the panel it belongs to.

This spec covers those two changes and nothing else.

## Not in this spec

The same conversation asked for three more things. Each one gets its own spec,
its own plan and its own build.

- 17 more built-in oscillator shapes, in four families.
- Massive-style oscillator modes: Bend +, Bend -, Bend -/+, Spectrum, Formant,
  an Intensity knob and a Sync knob.
- Nine more filter modes, four of them nonlinear.

Nothing in this spec may make those three harder. Nothing in this spec depends
on them either.

---

## Part A: the trim sounds while you drag it

### What happens now

`WaveformEditorComponent::mouseDrag` moves a line on a cached picture and
nothing else. `mouseUp` calls `commitTrim()`, which calls
`UserWaveLibrary::setSlotTrim()`.

`setSlotTrim` does three costly things (`Source/UserWavetable.cpp:702`):

1. `loadSlotAudio` — decodes the slot's audio again from its stored bytes.
2. `buildSlot` — rebuilds the padded buffer and the play and loop points.
3. `saveToDisk()` then `publish()` — writes the index file, then hands the audio
   thread a fresh bank.

Only step 2 has to happen while the mouse moves. Step 1 gives the same answer
every time inside one gesture. Step 3's disk write is worthless until the
gesture ends.

### What the audio thread already does

`publish()` is safe to call often, and always was
(`Source/UserWavetable.cpp:272`). It clones the bank, swaps it in with one
atomic exchange, and the old bank travels back through a 32-entry `retired`
array to be deleted on the message thread. The audio thread never allocates and
never frees. `collectRetired()` drains that array at the start of every publish.

`SynthVoice` re-reads its slot pointers once per block
(`Source/SynthVoice.cpp:780`). That is why lifting the key works today, and it
is why a live publish will be heard by a note that is already sounding.

### The change: a trim session

Add three methods to `UserWaveLibrary`.

```
bool beginTrimSession (UserWave::Group group, int slotIndex, juce::String& errorMessage);
bool updateTrimSession (int trimStart, int trimEnd, juce::String& errorMessage);
bool endTrimSession (juce::String& errorMessage);
```

`beginTrimSession` decodes the slot's audio once with `loadSlotAudio` and holds
the result, the group and the slot index for the length of the session. It
fails, and opens nothing, on the same conditions `setSlotTrim` rejects today: no
such slot, an empty slot, or a slot that is not in Full Sample mode.

`updateTrimSession` runs `buildSlot` on that one slot from the held audio, then
calls `publish()`. It does **not** call `saveToDisk()`. It keeps the existing
rollback: if `buildSlot` leaves the slot inactive, the previous trim numbers go
back and the slot is rebuilt from them.

`endTrimSession` runs one last `updateTrimSession`, then calls `saveToDisk()`,
then releases the held audio. Calling it without an open session does nothing
and reports success.

`setSlotTrim` stays exactly as it is. The double-click reset and any other
caller keep using it. A session is the drag path only.

Only one session may be open at a time. `beginTrimSession` on an already-open
session ends the old one first.

### Throttling

`WaveformEditorComponent` already owns a `juce::MultiTimer` with two timer ids.
Add a third, `trimTimerId`, at **33 ms** — about 30 updates a second.

- `mouseDown` on a marker calls `beginTrimSession`. On failure the drag is
  refused and the message goes to the status line, exactly as a failed
  `setSlotTrim` does now.
- `mouseDrag` keeps doing what it does today: it moves `dragTrimStart` and
  `dragTrimEnd`, rebuilds the strip picture and repaints. It does not publish.
  It starts `trimTimerId` if it is not already running.
- `trimTimerId` calls `updateTrimSession` with the current drag numbers, and
  only when they changed since the last tick. A still mouse costs nothing.
- `mouseUp` stops the timer, calls `endTrimSession`, and writes the status line
  from the slot as `commitTrim()` does now.

33 ms is a deliberate choice. An update in Full Sample mode builds no mipmap
tables — `buildSlot` clears `tables` and fills `sample` only — so it costs one
copy of the sample, at most about 2.6 MB at the fifteen-second limit. Thirty of
those a second is work the message thread can carry, and it is well inside the
32-entry `retired` array at any normal block rate.

### What a held note does

The note's phase keeps running. It is not restarted.

Dragging the start marker changes the buffer's length, so the phase lands
somewhere new on each update, and the sound moves under your hand like
scrubbing. That is the intended behaviour.

Restarting the phase at `playStart` on each update was considered and rejected:
thirty retriggers a second is a buzz, not a preview.

### Failure

If `updateTrimSession` fails mid-gesture, the drag stops, the status line says
why, and the session ends without a disk write. The slot keeps the last trim
that worked.

---

## Part B: a panel, not a window

### What happens now

`WaveformEditorWindow` is a `juce::DocumentWindow`
(`Source/WaveformEditorComponent.h:582`). The editor keeps one, hides it rather
than deletes it, and calls `showFor(...)` on it from
`SpaceDustAudioProcessorEditor::openWaveformWindow`
(`Source/PluginEditor.cpp:6706`).

### The change

`WaveformEditorComponent` does not change at all. Its size, its layout, its
mouse handling and its drawing all stay.

`WaveformEditorWindow` is deleted. A new `WaveformEditorPanel : juce::Component`
replaces it. It owns one `WaveformEditorComponent`, draws a frame, a title and a
close button around it. `setResampleHost`, `refreshContent` and
`repaintContent` keep their present signatures, so the editor's meter timer and
its refresh path do not change.

`showFor` gains one argument: the Edit button that was pressed, which is what
the panel is placed against.

```
void showFor (juce::Component* anchorButton,
              juce::ComboBox* targetCombo, int userBase,
              WaveformEditorComponent::BuiltInKind kind, UserWave::Group group,
              int slotIndex);
```

The button has to be threaded through to reach it. Today the `onClick` lambda
captures the combo, the base, the kind and the group, but not the button
(`Source/PluginEditor.cpp:4244`), and `openWaveformWindow` does not take one.
Two small changes: the lambda also captures `target.button`, and
`openWaveformWindow` gains a leading `juce::Component* anchorButton` argument
that it passes straight on. The `EditButtonTarget` table already holds the
button, so no new wiring is needed above that.

**Parent.** `mainView`. It lays out in design coordinates and carries the single
scale transform (`Source/PluginEditor.cpp:8050`), so a child of it scales with
the window and may float over the tab bar. The panel is added once, kept
hidden, and brought to the front when shown.

**Position.** `showFor` takes the Edit button that was pressed. The button's
bounds are converted into `mainView` coordinates with
`mainView.getLocalArea(button, button->getLocalBounds())`. The panel is placed
with its top-left just below and right of the button, then clamped so it stays
inside `mainView`.

Clamping is not optional. The panel is 670 wide and 470 tall for an oscillator
list. Anchored at the Osc 1 Edit button it fits with room to spare. Anchored at
the Sub Oscillator Edit button, low and right in the layout, it would run off
both the right edge and the bottom.

**Opacity.** Nothing behind the panel dims. That was asked for, and it is
exactly why the panel must read as solid: `setOpaque(true)`, its own
background, its own border, and a drop shadow. A see-through panel over this
layout would be unreadable.

**Closing.** Four ways, all doing the same thing:

- the close button,
- the Escape key,
- a mouse press outside the panel,
- a change of tab.

Closing calls `setPlayheadActive(false)`, as hiding the window does today, so a
panel nobody can see asks the audio thread for nothing. Closing must also end
any open trim session.

**What survives.** The panel is hidden, not deleted, so the selection and the
slot it was showing survive being closed and opened again. The window's screen
position does not survive, because the panel no longer has one — it is placed
from the button every time.

### The drag-and-drop risk

`WaveformEditorWindow` carries `allowFileDropsFromLowerPrivilege()`
(`Source/WaveformEditorComponent.h:620`). Windows blocks messages sent from a
lower privilege level to a higher one, and a file drop is such a message.
Without the filter, dragging a file out of Explorer into an elevated DAW does
nothing at all, with no error and no cursor change.

Once the panel is a child of the plugin editor, that drop no longer arrives at
a window we own. It arrives at the host's window.

The plan:

1. Move the filter call so it is applied to the top-level window that hosts the
   panel, found with `getTopLevelComponent()` at the moment the panel is shown.
2. Test a drop from Explorer in the Standalone and in Ableton Live 10.1.43,
   elevated and not elevated.
3. If a host refuses the drop, say so in this document and leave it. Load File
   still works, and no other path is affected.

This risk is accepted, not solved in advance. It cannot be settled by reading
code.

---

## Testing

**Automated, in `tools/wavetabletest`.** The trim session needs no window.

- A session on an empty slot fails and opens nothing.
- A session on a Single Cycle slot fails and opens nothing.
- Open, then several updates, then close. After each update the slot's
  `playStart`, `playLength`, `padStart()` and `padEnd()` match what
  `setSlotTrim` produces for the same numbers.
- The index file's modification time does not change across the updates, and
  does change on close.
- An update with numbers that leave too little sound rolls back, and the slot
  keeps its last good trim.
- Closing a session that was never opened succeeds and does nothing.

Note the build trap: MSBuild reads a line containing `error :` in a test's
output as a build error and fails the target with code -1, even when the
executable returns 0. Test failure messages must not contain that string.

**By hand, in the Standalone.**

- Hold a note, drag the start marker, and hear it change under the mouse.
- Open the panel from all five Edit buttons. Check that each one is fully
  inside the window, including the Sub Oscillator button.
- Close it all four ways.
- Resize the plugin to its smallest and largest and open the panel again.
- Drop a file from Explorer onto the panel.

Build the Standalone and the VST3 together, and copy the VST3 to Common Files,
or the DAWs load stale code.

---

## Risks

| Risk | Severity | Handling |
|---|---|---|
| A host blocks the file drop onto an in-editor panel | Medium | Test in Ableton and the Standalone. Load File is the fallback. |
| 30 publishes a second is heavier than measured with many large slots loaded | Low | `cloneBank` deep-copies every slot. If it is felt, lower the rate to 20 Hz before anything else. |
| A held note jumps oddly as the buffer length changes | Low | Intended. It is scrubbing. |
| The panel covers a control the user wants while it is open | Low | Accepted. It closes on a click outside. |

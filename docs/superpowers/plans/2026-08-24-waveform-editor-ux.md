# Waveform Editor UX Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make a waveform trim marker sound as you drag it, and open the Waveforms editor as a panel inside the plugin instead of a separate desktop window.

**Architecture:** A *trim session* holds a slot's decoded audio for the length of one mouse gesture, so the drag rebuilds and republishes the slot about thirty times a second without decoding or writing to disk; the index file is written once when the mouse comes up. The editor's `DocumentWindow` is replaced by a plain `juce::Component` parented to `mainView`, which carries the plugin's single scale transform, and is positioned against the Edit button that opened it.

**Tech Stack:** C++17, JUCE 7, CMake, MSVC Release, Windows.

**Spec:** `docs/superpowers/specs/2026-08-24-waveform-editor-ux-design.md`

## Global Constraints

- C++17. `target_compile_features(... cxx_std_17)`.
- Test targets are **JUCE-free**. Do not link JUCE into `trimsession_test`.
- Test output must never contain the string `error :` — MSBuild reads that as a build error and fails the target with exit code -1 even when the executable returns 0.
- Build the Standalone and the VST3 **together**. A VST3-only build leaves a stale Standalone on the desktop.
- After building, deploy the VST3 to Common Files or the DAWs load stale code.
- Never launch the Standalone from an elevated shell — it inherits admin and Explorer drag-and-drop dies silently. Launch it with `explorer.exe <path>`.
- The product name is read, never written out. Use `product-name.ps1`; a literal name resolves to the V1 install.
- Do not touch the `v1-maintenance` branch.
- `UserWave::Group` is an `enum class` with values `Osc1=0, Osc2, Sub, Noise, Transient`; `UserWave::numGroups == 5`, `UserWave::numSlots == 8`.

---

### Task 1: TrimSession, the JUCE-free sequencing

The geometry of a trim already lives in `WaveAnalysis::regionForTrim` and is already tested. This task adds only the part that is new and can be wrong: open/closed state, "has anything moved since the last apply", and "is a final apply still owed at close".

**Files:**
- Create: `Source/TrimSession.h`
- Create: `tools/trimsessiontest/trim_session_test.cpp`
- Modify: `CMakeLists.txt` (after the `resample-test` block, which ends at line 394)

**Interfaces:**
- Consumes: nothing.
- Produces: `class TrimSession` with `struct Points { int start; int end; }`, `struct Closing { bool wasOpen; Points last; bool applyOwed; }`, and methods `bool isOpen() const noexcept`, `int group() const noexcept`, `int slot() const noexcept`, `bool open(int group, int slot, Points initial) noexcept`, `void pending(Points) noexcept`, `bool wants() const noexcept`, `Points pendingPoints() const noexcept`, `void applied() noexcept`, `Closing close() noexcept`. Task 2 uses all of these.

- [ ] **Step 1: Write the failing test**

Create `tools/trimsessiontest/trim_session_test.cpp`:

```cpp
// =====================================================================
//  Trim session test
//  ---------------------------------------------------------------------
//  A trim session is what makes a marker sound while it is being dragged:
//  the slot's audio is decoded once, rebuilt many times, and written to
//  disk once at the end.
//
//  The GEOMETRY of a trim is WaveAnalysis::regionForTrim and is tested in
//  wavetable_test. What is tested here is the SEQUENCING, which is pure
//  state and needs no JUCE: what is legal when, whether anything actually
//  moved, and whether a last apply is still owed when the mouse comes up.
//
//  Build & run:
//      cmake --build build --config Release --target trimsession-test
//      ./build/trim_session_test/Release/trim_session_test.exe
// =====================================================================
#include "../../Source/TrimSession.h"

#include <cstdio>
#include <string>

namespace
{
    int failures = 0;

    void check(bool ok, const std::string& what)
    {
        if (!ok)
        {
            // Never the string "error :" -- MSBuild reads that as a build failure.
            std::printf("  FAIL  %s\n", what.c_str());
            ++failures;
        }
    }

    using Points = TrimSession::Points;
}

int main()
{
    std::printf("Trim session tests\n==================\n");

    std::printf("\nA new session is closed:\n");
    {
        TrimSession s;
        check(!s.isOpen(), "a new session claimed to be open");
        check(s.group() == -1, "a closed session named a group");
        check(s.slot() == -1, "a closed session named a slot");
        check(!s.wants(), "a closed session asked for work");
        std::printf("  closed, names nothing, asks for nothing\n");
    }

    std::printf("\nOpening records the slot:\n");
    {
        TrimSession s;
        check(s.open(3, 5, Points{100, 200}), "opening a fresh session was refused");
        check(s.isOpen(), "an opened session claimed to be closed");
        check(s.group() == 3, "the session forgot its group");
        check(s.slot() == 5, "the session forgot its slot");
        std::printf("  group 3, slot 5\n");
    }

    std::printf("\nThe opening numbers are already applied:\n");
    {
        // The slot on screen ALREADY plays with these numbers -- they are read
        // out of it. Republishing them would be a rebuild that changed nothing.
        TrimSession s;
        s.open(0, 0, Points{100, 200});
        check(!s.wants(), "opening asked for a rebuild that would change nothing");
        std::printf("  no rebuild asked for\n");
    }

    std::printf("\nA still mouse costs nothing:\n");
    {
        TrimSession s;
        s.open(0, 0, Points{100, 200});
        s.pending(Points{150, 200});
        check(s.wants(), "a moved marker did not ask for a rebuild");
        check(s.pendingPoints().start == 150, "the session lost the new start");
        check(s.pendingPoints().end == 200, "the session lost the end");
        s.applied();
        check(!s.wants(), "a marker that was applied asked to be applied again");
        s.pending(Points{150, 200});
        check(!s.wants(), "the same numbers asked for a second rebuild");
        std::printf("  one rebuild per move, none for a still mouse\n");
    }

    std::printf("\nClosing owes a last apply when the mouse outran the timer:\n");
    {
        TrimSession s;
        s.open(0, 0, Points{100, 200});
        s.pending(Points{150, 200});
        s.applied();
        s.pending(Points{180, 200});   // moved again, no tick before mouse-up

        const auto c = s.close();
        check(c.wasOpen, "closing an open session said it was closed");
        check(c.applyOwed, "the last position of the drag was dropped");
        check(c.last.start == 180, "closing reported the wrong last start");
        check(c.last.end == 200, "closing reported the wrong last end");
        check(!s.isOpen(), "the session stayed open after closing");
        std::printf("  last position 180 survives the mouse coming up\n");
    }

    std::printf("\nClosing owes nothing when the timer kept up:\n");
    {
        TrimSession s;
        s.open(0, 0, Points{100, 200});
        s.pending(Points{150, 200});
        s.applied();

        const auto c = s.close();
        check(c.wasOpen, "closing an open session said it was closed");
        check(!c.applyOwed, "closing asked for a rebuild that would change nothing");
        std::printf("  no needless rebuild\n");
    }

    std::printf("\nClosing a session that was never opened is safe:\n");
    {
        TrimSession s;
        const auto c = s.close();
        check(!c.wasOpen, "closing a closed session said it had been open");
        check(!c.applyOwed, "closing a closed session asked for work");
        check(!s.isOpen(), "closing a closed session opened it");
        std::printf("  nothing happens\n");
    }

    std::printf("\nTwo gestures cannot interleave:\n");
    {
        TrimSession s;
        s.open(0, 0, Points{100, 200});
        check(!s.open(1, 2, Points{0, 0}), "a second slot was opened over the first");
        check(s.group() == 0 && s.slot() == 0, "the refused open moved the session");
        check(s.open(0, 0, Points{100, 200}), "reopening the same slot was refused");
        std::printf("  a different slot is refused, the same slot is not\n");
    }

    std::printf("\n%s\n", failures == 0 ? "All trim session tests passed."
                                        : "Some trim session tests FAILED.");
    return failures == 0 ? 0 : 1;
}
```

- [ ] **Step 2: Run it to verify it fails**

```bash
cmake --build build --config Release --target trimsession-test
```

Expected: FAIL. The target does not exist yet, and `Source/TrimSession.h` does not exist.

- [ ] **Step 3: Write the implementation**

Create `Source/TrimSession.h`:

```cpp
#pragma once

/**
    One drag of a start or end marker, from the mouse going down to it coming up.

    Moving a marker used to be heard only after the key was released and pressed
    again, because putting a marker into effect decodes the slot's audio again,
    rebuilds it and writes the index file -- work that is right once a gesture is
    finished and wrong sixty times a second.

    A session is what lets the middle of that happen often and the ends happen
    once. The audio is decoded when the session opens and held; the rebuild runs
    on a timer while the mouse moves; the index file is written when it closes.

    This class holds none of that. It holds only WHEN each of them is allowed and
    whether anything has actually moved -- which is pure state, needs no JUCE, and
    is therefore the part that can be tested without a window or a disk. The
    geometry of a trim is WaveAnalysis::regionForTrim and is tested separately.
*/
class TrimSession
{
public:
    /** Where the two markers stand, in samples of the imported file. */
    struct Points
    {
        int start = 0;
        int end = 0;
    };

    /** What close() found. applyOwed is the one that matters: a fast drag can
        move the markers after the last timer tick, and without this that final
        position would be dropped on the floor. */
    struct Closing
    {
        bool wasOpen = false;
        Points last {};
        bool applyOwed = false;
    };

    bool isOpen() const noexcept { return openFlag; }

    /** The slot being dragged, or -1 for both when nothing is. */
    int group() const noexcept { return openFlag ? groupIndex : -1; }
    int slot() const noexcept { return openFlag ? slotIndex : -1; }

    /** Begin a gesture on a slot, with the markers where they already stand.

        Those opening numbers count as ALREADY APPLIED, because they were read
        out of the slot and the slot already plays with them. Treating them as
        pending would make every mouse-down rebuild a slot into exactly what it
        already was.

        Refused, changing nothing, while a DIFFERENT slot is open: two gestures
        must never interleave. Opening the same slot again is accepted and does
        nothing, so a stray second mouse-down cannot lose the drag. */
    bool open (int newGroup, int newSlot, Points initial) noexcept
    {
        if (openFlag && (groupIndex != newGroup || slotIndex != newSlot))
            return false;

        openFlag = true;
        groupIndex = newGroup;
        slotIndex = newSlot;
        pendingPos = initial;
        appliedPos = initial;
        return true;
    }

    /** Where the markers have been dragged to. Costs nothing and may be called
        on every mouse move. */
    void pending (Points p) noexcept { pendingPos = p; }

    /** Whether a rebuild would change anything. False for a still mouse, which
        is what keeps a paused drag free. */
    bool wants() const noexcept
    {
        return openFlag && (pendingPos.start != appliedPos.start
                            || pendingPos.end != appliedPos.end);
    }

    Points pendingPoints() const noexcept { return pendingPos; }

    /** Record that the pending numbers were put into the slot. */
    void applied() noexcept { appliedPos = pendingPos; }

    /** End the gesture, and say whether the last position still needs applying. */
    Closing close() noexcept
    {
        Closing result;
        result.wasOpen = openFlag;
        result.last = pendingPos;
        result.applyOwed = wants();

        openFlag = false;
        groupIndex = -1;
        slotIndex = -1;
        return result;
    }

private:
    bool openFlag = false;
    int groupIndex = -1;
    int slotIndex = -1;

    /** Where the mouse has dragged to, and where the slot was last rebuilt to.
        They differ exactly when a rebuild is owed. */
    Points pendingPos {};
    Points appliedPos {};
};
```

Add to `CMakeLists.txt`, immediately after the `resample-test` block (which ends with the `add_custom_target(resample-test ...)` at line 391-394):

```cmake
#==============================================================================
# trimsession-test — standalone check of the trim drag's sequencing.
# Dragging a marker decodes the slot's audio once, rebuilds it many times and
# writes the index file once. WHEN each of those is allowed, and whether a
# marker has actually moved, is pure state and needs no JUCE. The geometry of a
# trim is WaveAnalysis::regionForTrim and is checked in wavetable-test.
# Usage: cmake --build build --config Release --target trimsession-test
#==============================================================================
add_executable(trim_session_test
    tools/trimsessiontest/trim_session_test.cpp)
set_target_properties(trim_session_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/trim_session_test")
target_compile_features(trim_session_test PRIVATE cxx_std_17)
target_include_directories(trim_session_test PRIVATE Source)

add_custom_target(trimsession-test
    COMMAND $<TARGET_FILE:trim_session_test>
    DEPENDS trim_session_test
    COMMENT "Running trim session tests")
```

- [ ] **Step 4: Run it to verify it passes**

```bash
cmake --build build --config Release --target trimsession-test
```

Expected: PASS. Last line `All trim session tests passed.`

- [ ] **Step 5: Commit**

```bash
git add Source/TrimSession.h tools/trimsessiontest/trim_session_test.cpp CMakeLists.txt
git commit -m "feat(waves): hold one marker drag in a trim session"
```

---

### Task 2: The trim session in UserWaveLibrary

**Files:**
- Modify: `Source/UserWavetable.h` — add the public API after `setSlotTrim` (line 529) and the members before `formatManager` (line 696)
- Modify: `Source/UserWavetable.cpp` — add the three methods after `setSlotTrim`, which ends at line 761

**Interfaces:**
- Consumes: `TrimSession` from Task 1 — `open`, `pending`, `wants`, `pendingPoints`, `applied`, `close`, `isOpen`, `group`, `slot`.
- Produces: on `UserWaveLibrary`: `bool beginTrimSession(UserWave::Group, int slotIndex, juce::String& errorMessage)`, `bool updateTrimSession(int trimStart, int trimEnd, juce::String& errorMessage)`, `bool endTrimSession(juce::String& errorMessage)`, `bool trimSessionIsOpen() const noexcept`. Task 3 calls all four.

- [ ] **Step 1: Add the include and the declarations**

At the top of `Source/UserWavetable.h`, with the other project includes:

```cpp
#include "TrimSession.h"
```

In the public section of `UserWaveLibrary`, directly after the `setSlotTrim` declaration:

```cpp
    //==========================================================================
    // -- Dragging a marker --
    //
    // setSlotTrim above is one finished decision: it decodes the slot's audio
    // again, rebuilds it, writes the index file and publishes. That is right for
    // a double-click reset and wrong thirty times a second.
    //
    // A session splits it up. The decode happens once at the start and the write
    // once at the end; only the rebuild and the publish happen in between. The
    // slot ends up in exactly the state setSlotTrim would have left it in.

    /** Begin a drag on a slot: decode its audio once and hold it.

        Refused, with a line for the player, on the same slots setSlotTrim
        refuses -- one that does not exist, one that is empty, and one that is
        not in Full Sample mode -- and on a slot that cannot be decoded. */
    bool beginTrimSession (UserWave::Group group, int slotIndex, juce::String& errorMessage);

    /** Put the markers where the mouse has them, and let the audio thread hear
        it. Rebuilds from the held audio and publishes. Writes nothing to disk.

        Does nothing, successfully, when the markers have not moved since the
        last call -- so a paused drag costs nothing at all. */
    bool updateTrimSession (int trimStart, int trimEnd, juce::String& errorMessage);

    /** End the drag: apply the last position if the mouse outran the timer, then
        write the index file and release the held audio.

        Safe, and successful, when no session is open. */
    bool endTrimSession (juce::String& errorMessage);

    bool trimSessionIsOpen() const noexcept { return trimSession.isOpen(); }
```

In the private section, immediately before `juce::AudioFormatManager formatManager;`:

```cpp
    /** The drag in progress, and the audio it is rebuilding from.

        The audio is the whole imported file and is decoded once, when the
        session opens. Holding it is the entire point: decoding is what made a
        marker too expensive to move while a note was sounding. */
    TrimSession trimSession;
    std::vector<float> trimAudio;
    double trimAudioRate = 0.0;
```

- [ ] **Step 2: Write the three methods**

In `Source/UserWavetable.cpp`, directly after `setSlotTrim` ends (line 761, the closing brace before `renameSlot`):

```cpp
bool UserWaveLibrary::beginTrimSession (UserWave::Group group, int slotIndex,
                                        juce::String& errorMessage)
{
    // A session left open by a gesture that never finished would hold the wrong
    // slot's audio. Close it before opening another.
    if (trimSession.isOpen())
    {
        juce::String ignored;
        endTrimSession (ignored);
    }

    if (slotIndex < 0 || slotIndex >= UserWave::numSlots)
    {
        errorMessage = "That waveform slot does not exist.";
        return false;
    }

    auto& slot = editable->editableSlot (group, slotIndex);

    if (! slot.active)
    {
        errorMessage = "That slot is empty.";
        return false;
    }

    if (slot.mode != UserWave::Mode::FullSample)
    {
        errorMessage = "The start and end can only be moved on a whole sample.";
        return false;
    }

    // The one decode of the gesture.
    if (! loadSlotAudio (slot, trimAudio, trimAudioRate, errorMessage))
    {
        trimAudio.clear();
        trimAudioRate = 0.0;
        return false;
    }

    trimSession.open ((int) group, slotIndex,
                      TrimSession::Points { slot.trimStart, slot.trimEnd });
    return true;
}

bool UserWaveLibrary::updateTrimSession (int trimStart, int trimEnd,
                                         juce::String& errorMessage)
{
    if (! trimSession.isOpen())
    {
        errorMessage = "There is no drag in progress.";
        return false;
    }

    trimSession.pending (TrimSession::Points { trimStart, trimEnd });

    // A still mouse. Not a failure -- there is simply nothing to do.
    if (! trimSession.wants())
        return true;

    const auto group = (UserWave::Group) trimSession.group();
    auto& slot = editable->editableSlot (group, trimSession.slot());

    const int keptStart = slot.trimStart;
    const int keptEnd = slot.trimEnd;

    slot.trimStart = trimStart;
    slot.trimEnd = trimEnd;

    buildSlot (slot, trimAudio, trimAudioRate, UserWave::Mode::FullSample);

    if (! slot.active)
    {
        // Same rollback setSlotTrim does: a slot that fell out of the list would
        // take the player's waveform with it mid-drag.
        slot.trimStart = keptStart;
        slot.trimEnd = keptEnd;
        buildSlot (slot, trimAudio, trimAudioRate, UserWave::Mode::FullSample);

        errorMessage = "There is not enough of the sample left between those two points.";
        return false;
    }

    trimSession.applied();

    // No saveToDisk(). That is what the end of the gesture is for.
    publish();
    return true;
}

bool UserWaveLibrary::endTrimSession (juce::String& errorMessage)
{
    if (! trimSession.isOpen())
        return true;

    // The last position FIRST, while the session and the held audio are both
    // still alive. A fast drag moves the markers after the final timer tick, and
    // without this the position the player actually let go at would be lost.
    bool ok = true;

    if (trimSession.wants())
    {
        const auto last = trimSession.pendingPoints();
        ok = updateTrimSession (last.start, last.end, errorMessage);
    }

    trimSession.close();

    trimAudio.clear();
    trimAudio.shrink_to_fit();
    trimAudioRate = 0.0;

    // The one write of the gesture. Done even when the last apply was refused:
    // the slot still holds the last trim that worked, and that is worth keeping.
    saveToDisk();
    return ok;
}
```

- [ ] **Step 3: Build the plugin to verify it compiles**

```bash
cmake --build build --config Release --target SpaceDust_Standalone SpaceDust_VST3
```

Expected: builds clean. No behaviour has changed yet — nothing calls the new methods.

- [ ] **Step 4: Commit**

```bash
git add Source/UserWavetable.h Source/UserWavetable.cpp
git commit -m "feat(waves): decode once and save once around a marker drag"
```

---

### Task 3: The drag publishes on a timer

**Files:**
- Modify: `Source/WaveformEditorComponent.cpp` — timer id constants near line 65, `mouseDown` line 1025, `mouseDrag` line 1053, `mouseUp` line 1094, `timerCallback` line 768
- Modify: `Source/WaveformEditorComponent.h` — declarations near `commitTrim` (line 355)

**Interfaces:**
- Consumes: `UserWaveLibrary::beginTrimSession`, `updateTrimSession`, `endTrimSession` from Task 2.
- Produces: nothing new for later tasks. Task 5 calls `endTrimSession` through the existing `library` reference when the panel closes.

- [ ] **Step 1: Add the timer id and interval**

In the anonymous namespace at the top of `Source/WaveformEditorComponent.cpp`, beside `resampleTimerId` and `playheadTimerId`:

```cpp
    constexpr int trimTimerId = 2;

    /** How often a drag is put into the slot and published: about thirty times a
        second.

        Fast enough that the sound follows the hand, and slow enough to be work
        the message thread can carry. In Full Sample mode a rebuild builds no
        mipmap tables -- buildSlot clears them and fills the sample buffer only --
        so one update is a copy of the sample, at most about 2.6 MB at the
        fifteen second limit. */
    constexpr int trimIntervalMs = 33;
```

- [ ] **Step 2: Open the session on mouse-down**

In `mouseDown`, replace the marker branch (the `if (const auto handle = handleAt (...))` block) with:

```cpp
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
```

- [ ] **Step 3: Start the timer on drag, and serve it**

At the end of `mouseDrag`, replace the final two lines (`sampleStrip.rebuild(); repaint();`) with:

```cpp
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
```

In `timerCallback`, add a branch beside the playhead one:

```cpp
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
```

- [ ] **Step 4: Close the session on mouse-up**

Replace `mouseUp` with:

```cpp
void WaveformEditorComponent::mouseUp (const juce::MouseEvent&)
{
    if (trimDrag == TrimHandle::None)
        return;

    trimDrag = TrimHandle::None;
    stopTimer (trimTimerId);
    commitTrim();
}
```

Replace the body of `commitTrim` down to and including its `setSlotTrim` block. The old first half:

```cpp
    const int start = dragTrimStart;

    // Exactly the end of the file is stored as a zero, so that a slot whose
    // sample is later replaced by a longer one still plays all of it. Past the
    // end is silence, and that is kept as the number it is.
    const int end = dragTrimEnd == entry->fileLength ? 0 : dragTrimEnd;

    juce::String error;

    if (! library.setSlotTrim (group, activeSlot, start, end, error))
    {
        setStatus (error, true);
        refresh();
        return;
    }
```

becomes:

```cpp
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
```

- [ ] **Step 5: Build, then hear it**

```bash
cmake --build build --config Release --target SpaceDust_Standalone SpaceDust_VST3 trimsession-test
```

Launch the Standalone with `explorer.exe`, never from an elevated shell:

```bash
explorer.exe "build/SpaceDust_artefacts/Release/Standalone/Space Dust V2.exe"
```

Check by hand: import a sample into an Osc 1 slot, hold a note, drag the start
marker, and hear it change under the mouse without releasing the key. Drag the
end marker the same way. Double-click to reset and confirm that still works.

- [ ] **Step 6: Commit**

```bash
git add Source/WaveformEditorComponent.h Source/WaveformEditorComponent.cpp
git commit -m "feat(waves): a marker now sounds while you drag it"
```

---

### Task 4: The panel replaces the window

**Files:**
- Modify: `Source/WaveformEditorComponent.h` — replace the `WaveformEditorWindow` class at the end of the file (line 582 to the end)
- Modify: `Source/WaveformEditorComponent.cpp` — replace the `WaveformEditorWindow` methods
- Modify: `Source/PluginEditor.h` — the `waveformWindow` member declaration
- Modify: `Source/PluginEditor.cpp` — `openWaveformWindow` at line 6706, the `onClick` lambda at line 4244, `refreshUserWaveformNames` near line 4192

**Interfaces:**
- Consumes: `WaveformEditorComponent` unchanged.
- Produces: `class WaveformEditorPanel : public juce::Component` with `void showFor (juce::Component* anchorButton, juce::ComboBox* targetCombo, int userBase, WaveformEditorComponent::BuiltInKind kind, UserWave::Group group, int slotIndex)`, `void hidePanel()`, `void setResampleHost (WaveformEditorComponent::ResampleHost*)`, `void refreshContent()`, `void repaintContent()`. Task 5 calls `hidePanel`.

- [ ] **Step 1: Replace the window class with a panel class**

In `Source/WaveformEditorComponent.h`, replace the whole `WaveformEditorWindow`
class (from its doc comment at line 578 to the end of the file) with:

```cpp
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

    /** Bring the panel up, pointed at the dropdown that asked for it and placed
        against the button that was pressed. */
    void showFor (juce::Component* anchorButton, juce::ComboBox* targetCombo,
                  int userBase, WaveformEditorComponent::BuiltInKind kind,
                  UserWave::Group group, int slotIndex);

    /** Put the panel away: stop the playhead, and end any drag still open. */
    void hidePanel();

    void setResampleHost (WaveformEditorComponent::ResampleHost* host);

    /** Redraw after the library changed under it. */
    void refreshContent();

    /** Redraw only, with no rebuild of the controls. The editor's meter timer
        calls this so the panel blooms with the output like the main panel. */
    void repaintContent();

private:
    /** How much room the frame takes around the component, in design pixels. */
    static constexpr int frameInset = 8;
    static constexpr int titleHeight = 24;

    UserWaveLibrary& library;
    SpaceDustLookAndFeel& lookAndFeel;

    std::unique_ptr<WaveformEditorComponent> content;
    juce::TextButton closeButton { "X" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WaveformEditorPanel)
};
```

- [ ] **Step 2: Write the panel's methods**

In `Source/WaveformEditorComponent.cpp`, replace every `WaveformEditorWindow`
method with:

```cpp
//==============================================================================
WaveformEditorPanel::WaveformEditorPanel (UserWaveLibrary& libraryToUse,
                                          SpaceDustLookAndFeel& lookAndFeelToUse)
    : library (libraryToUse), lookAndFeel (lookAndFeelToUse)
{
    // Opaque so JUCE paints this rectangle alone. A non-opaque child makes its
    // parent repaint first, which here means the whole plugin behind it.
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
    closeButton.setLookAndFeel (nullptr);
}

void WaveformEditorPanel::paint (juce::Graphics& g)
{
    auto area = getLocalBounds().toFloat();

    g.setColour (juce::Colour (0xff0a0f1e));
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

void WaveformEditorPanel::showFor (juce::Component* anchorButton,
                                   juce::ComboBox* targetCombo, int userBase,
                                   WaveformEditorComponent::BuiltInKind kind,
                                   UserWave::Group group, int slotIndex)
{
    if (content == nullptr)
        return;

    content->setTarget (targetCombo, userBase, kind, group);

    if (slotIndex >= 0)
        content->selectSlot (slotIndex);

    const int width = WaveformEditorComponent::preferredWidth() + frameInset * 2;
    const int height = WaveformEditorComponent::preferredHeight (userBase)
                     + titleHeight + frameInset;

    setSize (width, height);

    // Placed against the button that opened it, then pushed back inside. The
    // clamp is not optional: the Sub Oscillator's button sits low and right in
    // the layout, and a panel hung off it would run off two edges at once.
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
    content->setPlayheadActive (true);
}

void WaveformEditorPanel::hidePanel()
{
    if (! isVisible())
        return;

    if (content != nullptr)
        content->setPlayheadActive (false);

    // A gesture that never got its mouse-up -- the panel closed mid-drag -- would
    // otherwise hold the slot's audio and never write the index file.
    juce::String ignored;
    library.endTrimSession (ignored);

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
```

- [ ] **Step 3: Point the editor at the panel**

In `Source/PluginEditor.h`, change the member's type:

```cpp
    std::unique_ptr<WaveformEditorPanel> waveformWindow;
```

and the method's signature:

```cpp
    void openWaveformWindow(juce::Component* anchorButton, juce::ComboBox* combo,
                            int userBase, WaveformEditorComponent::BuiltInKind kind,
                            UserWave::Group group);
```

In `Source/PluginEditor.cpp`, replace `openWaveformWindow`:

```cpp
void SpaceDustAudioProcessorEditor::openWaveformWindow(juce::Component* anchorButton,
                                                      juce::ComboBox* combo, int userBase,
                                                      WaveformEditorComponent::BuiltInKind kind,
                                                      UserWave::Group group)
{
    auto& library = audioProcessor.getUserWaveLibrary();

    if (waveformWindow == nullptr)
    {
        waveformWindow = std::make_unique<WaveformEditorPanel>(library, customLookAndFeel);

        // Parented to mainView, which carries the plugin's single scale
        // transform -- so the panel scales with the window and may float over
        // the tab bar.
        mainView.addChildComponent(*waveformWindow);

        // The synth the panel resamples. Safe to hand it `this`: the panel is
        // owned by this editor and is destroyed with it.
        waveformWindow->setResampleHost(this);
    }

    // The panel drives this dropdown directly, so its list and this menu are the
    // same list and cannot drift apart. -1 means "do not move the selection": the
    // dropdown is on a built-in shape, and opening a panel must never change the
    // sound by itself.
    const int slot = combo->getSelectedId() - 1 - userBase;

    waveformWindow->showFor(anchorButton, combo, userBase, kind, group,
                            (slot >= 0 && slot < UserWave::numSlots) ? slot : -1);
}
```

And the `onClick` lambda at line 4244:

```cpp
            auto* combo = target.combo;
            auto* button = target.button;
            const int userBase = target.userBase;
            const Kind kind = target.kind;
            const auto group = target.group;
            target.button->onClick = [this, button, combo, userBase, kind, group]
            {
                openWaveformWindow(button, combo, userBase, kind, group);
            };
```

- [ ] **Step 4: Build and check placement**

```bash
cmake --build build --config Release --target SpaceDust_Standalone SpaceDust_VST3
```

Launch with `explorer.exe` and open the panel from all five Edit buttons —
Waveform 1, Waveform 2, Noize Type, Sub Oscillator Wave and the Transient's.
Every one must be fully inside the window. The Sub Oscillator's is the one that
proves the clamp: it sits low and right, so its panel must be pushed back up
and left rather than run off the edge.

- [ ] **Step 5: Commit**

```bash
git add Source/WaveformEditorComponent.h Source/WaveformEditorComponent.cpp Source/PluginEditor.h Source/PluginEditor.cpp
git commit -m "feat(waves): open the editor as a panel, not a window of its own"
```

---

### Task 5: The four ways to close it

**Files:**
- Modify: `Source/WaveformEditorComponent.h` — add the key and focus overrides to `WaveformEditorPanel`
- Modify: `Source/WaveformEditorComponent.cpp` — the panel's methods
- Modify: `Source/PluginEditor.cpp` — the tab-change path

**Interfaces:**
- Consumes: `WaveformEditorPanel::hidePanel()` from Task 4.
- Produces: nothing for later tasks.

- [ ] **Step 1: Escape, and a click outside**

Add to `WaveformEditorPanel`'s public section in the header:

```cpp
    bool keyPressed (const juce::KeyPress& key) override;

    /** A press anywhere outside the panel puts it away.

        The panel cannot see those presses itself -- they land on whatever was
        clicked -- so it listens to the mouse for the whole component tree while
        it is open, and stops the moment it is not. */
    void mouseDown (const juce::MouseEvent& event) override;
```

Add a private member:

```cpp
    /** Watches every press in the plugin while the panel is open. */
    struct OutsideClickWatcher : public juce::MouseListener
    {
        explicit OutsideClickWatcher (WaveformEditorPanel& ownerToUse) : owner (ownerToUse) {}
        void mouseDown (const juce::MouseEvent& event) override;
        WaveformEditorPanel& owner;
    };

    OutsideClickWatcher outsideClicks { *this };
    bool watchingOutsideClicks = false;
```

- [ ] **Step 2: Implement them**

In `Source/WaveformEditorComponent.cpp`, add:

```cpp
bool WaveformEditorPanel::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress::escapeKey)
    {
        hidePanel();
        return true;
    }

    return false;
}

void WaveformEditorPanel::mouseDown (const juce::MouseEvent&)
{
    // A press that reached the panel is a press INSIDE it. Swallowed here so the
    // watcher below cannot read it as a press outside.
}

void WaveformEditorPanel::OutsideClickWatcher::mouseDown (const juce::MouseEvent& event)
{
    if (! owner.isVisible())
        return;

    // Anything that happened inside the panel, or inside anything the panel
    // owns, belongs to the panel.
    if (owner.isParentOf (event.eventComponent) || event.eventComponent == &owner)
        return;

    owner.hidePanel();
}
```

Extend `showFor`, at its end, after `setPlayheadActive (true)`:

```cpp
    // Listen for presses anywhere else in the plugin, so one puts the panel away.
    if (auto* parent = getParentComponent(); parent != nullptr && ! watchingOutsideClicks)
    {
        parent->addMouseListener (&outsideClicks, true);
        watchingOutsideClicks = true;
    }

    setWantsKeyboardFocus (true);
    grabKeyboardFocus();
```

And extend `hidePanel`, before `setVisible (false)`:

```cpp
    if (auto* parent = getParentComponent(); parent != nullptr && watchingOutsideClicks)
    {
        parent->removeMouseListener (&outsideClicks);
        watchingOutsideClicks = false;
    }
```

Add the same removal to the destructor, so a panel destroyed while open leaves
no listener behind:

```cpp
WaveformEditorPanel::~WaveformEditorPanel()
{
    if (auto* parent = getParentComponent(); parent != nullptr && watchingOutsideClicks)
        parent->removeMouseListener (&outsideClicks);

    closeButton.setLookAndFeel (nullptr);
}
```

- [ ] **Step 3: Confirm the tab change needs no code of its own**

The spec asked for four ways to close the panel, and the fourth was a change of
tab. Step 2 already delivered it, and no extra code is correct here.

`tabbedComponent` is a plain `juce::TabbedComponent` (`Source/PluginEditor.h:648`)
with no `currentTabChanged` override anywhere in the editor — the only uses are
reads of `getCurrentTabIndex()` and programmatic `setCurrentTabIndex()` calls.
Adding a hook would mean subclassing it purely for this.

There is no need. Changing tab means clicking the tab bar, the tab bar is
outside the panel, and the watcher from Step 2 closes the panel on any press
outside it. The tab change is covered by the rule that was already written.

The one case it does not cover is a tab changed *programmatically* — restoring
`lastActiveTabIndex` (`Source/PluginEditor.cpp:7408`), or the Cheeze Guy tab
being removed (`Source/PluginEditor.cpp:7270`). Neither can happen while the
player has a panel open and a hand on the mouse. Accepted, and not worth a
subclass.

**Do not add a tab hook.** Verify the behaviour in Step 4 instead.

- [ ] **Step 4: Build and check all four**

```bash
cmake --build build --config Release --target SpaceDust_Standalone SpaceDust_VST3
```

Open the panel and close it four ways in turn: the X, the Escape key, a click
on the plugin outside the panel, and a change of tab. Then check that a click
*inside* the panel — on a row, on a button, on the name box, on a trim marker —
does **not** close it. Dragging a marker from inside the panel out past its edge
and releasing must not close it either.

- [ ] **Step 5: Commit**

```bash
git add Source/WaveformEditorComponent.h Source/WaveformEditorComponent.cpp Source/PluginEditor.cpp
git commit -m "feat(waves): close the waveform panel four ways"
```

---

### Task 6: The file drop, then build and deploy

The editor window carried a Windows fix that let a file dragged out of Explorer
reach it inside an elevated DAW. Windows blocks messages sent from a lower
privilege level to a higher one, and a file drop is such a message; without the
filter the drop does nothing at all, with no error and no cursor change. Now
that the panel is inside the plugin, the drop arrives at the host's window.

This task is the only one whose outcome cannot be predicted by reading code.

**Files:**
- Modify: `Source/WaveformEditorComponent.cpp` — the panel's `showFor`
- Modify: `docs/superpowers/specs/2026-08-24-waveform-editor-ux-design.md` — record what the test found

**Interfaces:**
- Consumes: `WaveformEditorPanel::showFor` from Task 4.
- Produces: nothing.

- [ ] **Step 1: Apply the filter to whatever window now holds the panel**

The body needs no change at all, and this is worth understanding before
rewriting it by mistake. The old code reached its window through
`Component::getPeer()`, and JUCE's `getPeer()` walks UP the parent chain until
it finds a component that owns a heavyweight peer. Called on a child of
`mainView` it returns the host's window — which is exactly the window that now
receives the drop.

Add this to `WaveformEditorPanel` as a private method. It is the old body,
unchanged apart from the class name:

```cpp
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
```

Declare it in the header's private section:

```cpp
    /** Let a file dragged out of Explorer reach this panel even when the host is
        running with raised privileges.

        Windows blocks messages sent from a lower privilege level to a higher one,
        and a file drop is such a message. Without this, drag and drop silently
        does nothing in an elevated DAW -- no error, no cursor change, nothing --
        while the Load File button keeps working, which is a confusing pair of
        symptoms to be handed.

        When this lived on a window of our own it filtered that window. The panel
        has no window now, so getPeer() walks up to the HOST'S window and filters
        that instead -- which is the window the drop actually arrives at. Does
        nothing on any other platform, and nothing when the process is not
        elevated. */
    void allowFileDropsFromLowerPrivilege();
```

Call it at the very end of `showFor`, after `grabKeyboardFocus()`. It must be
called there and not in the constructor: a panel that has never been shown has
no peer to reach, and the host can hand us a different window between one
opening and the next.

```cpp
    // The panel no longer owns a window, so the drop lands on whatever window the
    // host gave us. Ask that window to let it through.
    allowFileDropsFromLowerPrivilege();
```

- [ ] **Step 2: Build both targets and the tests**

```bash
cmake --build build --config Release --target SpaceDust_Standalone SpaceDust_VST3 trimsession-test wavetable-test
```

Expected: all four succeed, and both test targets print their pass line.

- [ ] **Step 3: Deploy the VST3**

```bash
powershell -File build-and-launch.ps1 -NoLaunch
```

This reads the product name rather than hardcoding it, and mirrors the bundle
into the Common Files location. Without it, Ableton and FL load stale code.

- [ ] **Step 4: Test the drop in both hosts**

Standalone, launched through Explorer so it does not inherit admin:

```bash
explorer.exe "build/SpaceDust_artefacts/Release/Standalone/Space Dust V2.exe"
```

Drag a WAV out of Explorer onto the panel. Then open Ableton Live 10.1.43 and
do the same on the plugin's panel. `Log.txt` proves whether the build was really
loaded — DLL timestamps do not.

- [ ] **Step 5: Record what happened**

Write the result into the spec's "The drag-and-drop risk" section: which hosts
accepted the drop, which refused, and whether Load File covered the gap. Say
what actually happened, including a failure.

- [ ] **Step 6: Commit**

```bash
git add Source/WaveformEditorComponent.cpp docs/superpowers/specs/2026-08-24-waveform-editor-ux-design.md
git commit -m "fix(waves): let a dropped file reach the panel in an elevated host"
```

---

## Finishing

Both pieces are on `feat/waveform-editor-ux`. This is a solo project with nobody
to review a PR, so once it is tested: fast-forward `main` to the branch, delete
the branch, and push. Do not touch `v1-maintenance` — V1 is live.

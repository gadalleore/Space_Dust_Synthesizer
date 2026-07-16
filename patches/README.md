# Local JUCE patches

Space Dust needs a couple of small modifications to its JUCE copy. They live in the
JUCE tree (outside this repo), so they are **lost on a JUCE re-clone or update**. This
folder captures them so you can re-apply them.

There are two independent patches:

1. **MPE without the phantom-parameter flood** — `apply-juce-mpe-patch.ps1`
2. **FL Studio typing-keyboard fix** — `apply-juce-keyboard-focus-patch.ps1`

## TL;DR — re-applying after a fresh JUCE clone/update

```powershell
./patches/apply-juce-mpe-patch.ps1
./patches/apply-juce-keyboard-focus-patch.ps1
# each also accepts -JucePath C:\path\to\JUCE
```

---

# Patch 1 — MPE without the phantom-parameter flood

The script is idempotent (skips hunks already present) and string-based (line-number
independent). If a hunk reports `[FAIL]` after a JUCE upgrade, the surrounding code
changed — apply the three edits below by hand.

Validated against **JUCE 8.0.12**. Patched file:
`modules/juce_audio_plugin_client/juce_audio_plugin_client_VST3.cpp`

## Why

In VST3 there is no native MIDI-CC event type. A JUCE VST3 plugin receives **all**
controller data — MIDI CC, **pitch-bend, and channel pressure** — through JUCE's
`getMidiControllerAssignment` (IMidiMapping) mechanism, which exposes each controller
as a parameter. **MPE rides on this**: per-note pitch-bend, channel pressure, and
CC74 (timbre).

- Turning it **off** (`JUCE_VST3_EMULATE_MIDI_CC_WITH_PARAMETERS=0`) removes the
  clutter but **silently kills MPE** — `getMidiControllerAssignment` returns
  `kResultFalse`, so the host delivers no controllers (this JUCE has no
  Note-Expression path). Only notes play.
- Turning it **on** (the default) restores MPE but exposes **16 × 130 = 2080**
  phantom "MIDI CC" parameters that bloat the host automation list.

This patch keeps emulation **ON** but, when `JUCE_VST3_LIMIT_MIDI_CC_PARAMS=1` is
defined (Space Dust's `CMakeLists.txt` does this), exposes/maps **only** an allowlist
of controllers — mod wheel (CC1), expression (CC11), sustain (CC64), CC74 (timbre),
channel pressure, and pitch-bend — i.e. **~96 params instead of 2080**. The paramID
index space is left intact so JUCE's reverse decode still works.

> Note: the heap-corruption crash once blamed on the 2286-param surface was actually
> a separate block-size buffer overflow, fixed independently by grow-guards in
> `PluginProcessor.cpp` / `SynthVoice.cpp`. Re-validated clean with the
> `tools/stress` ASan harness after this patch.

## Build flags (already set in `CMakeLists.txt`, tracked in this repo)

```cmake
JUCE_VST3_EMULATE_MIDI_CC_WITH_PARAMETERS=1   # restores MPE
JUCE_VST3_LIMIT_MIDI_CC_PARAMS=1              # activates this patch (trims to ~96 params)
```

If the JUCE patch is missing but these flags are set, the build still produces
**working MPE** — just with the 2080-param clutter back. MPE cannot silently break.

## The three edits (manual fallback)

All in `modules/juce_audio_plugin_client/juce_audio_plugin_client_VST3.cpp`:

1. **Opt-in macro default** — after the `JUCE_VST3_EMULATE_MIDI_CC_WITH_PARAMETERS`
   default block, add a `#ifndef JUCE_VST3_LIMIT_MIDI_CC_PARAMS / #define ... 0 / #endif`
   block (defaults the feature OFF so sibling plugins sharing this JUCE are unaffected).

2. **`getMidiControllerAssignment`** — add a `static bool isAllowlistedMidiController
   (int)` helper (guarded by the macro) returning true for CC1/CC11/CC64/CC74/
   `kAfterTouch`/`kPitchBend`, and at the top of the function return `kResultFalse`
   for non-allowlisted controllers.

3. **`initialiseMidiControllerMappings`** — inside the channel/controller loop, after
   setting the `midiControllerToParameter` / `parameterToMidiController` entries, add a
   guarded `if (! isAllowlistedMidiController (i)) continue;` **before** the
   `parameters.addParameter(...)` call, so the index space keeps advancing but only
   allowlisted controllers become host parameters.

See `apply-juce-mpe-patch.ps1` for the exact before/after text.

---

# Patch 2 — FL Studio typing-keyboard fix

Re-apply with `./patches/apply-juce-keyboard-focus-patch.ps1`. Idempotent and
string-based. Validated against **JUCE 8.0.12**. Patched file:
`modules/juce_gui_basics/native/juce_Windowing_windows.cpp`

## Why

In FL Studio, "Typing keyboard to piano" (computer-keyboard MIDI) stops working the
moment the plugin editor gains keyboard focus — e.g. after clicking a tab, knob, or
anywhere in the window. Minimising the window (dropping its focus) makes it work
again; a hardware MIDI controller is never affected.

Cause: when the editor has keyboard focus and receives a key it does not handle, JUCE
forwards that Windows key message to the editor's **direct parent** window
(`GetParent`). In FL the direct parent is FL's plugin-**wrapper child window**, which
does not route "Typing keyboard to piano", so the keystroke is dropped. (Ableton's
direct parent handles it, which is why this only bites in FL. Hardware MIDI arrives via
the driver, not this key path, so it is unaffected.) This is a
[known JUCE↔FL issue](https://forum.juce.com/t/return-keyboard-focus-in-fl-studio/50711);
the JUCE-side focus tricks (`giveAwayKeyboardFocus`, `setWantsKeyboardFocus(false)`,
returning `false` from key handlers) are documented **not** to fix it in FL.

The fix forwards unhandled key/char messages to the **root** window
(`GetAncestor(hwnd, GA_ROOT)`) — the host's top-level window, which routes them
correctly — instead of the direct parent.

## Build flag (already set in `CMakeLists.txt`, tracked in this repo)

```cmake
JUCE_FORWARD_KEYS_TO_ROOT_WINDOW=1   # activates this patch
```

The change in `forwardMessageToParent` is wrapped in
`#if JUCE_FORWARD_KEYS_TO_ROOT_WINDOW`. That macro is **undefined for any plugin that
does not set it**, so sibling plugins sharing this JUCE copy compile the original
direct-parent behaviour and are unaffected. Windows-only (the file is
`*_windows.cpp`); other platforms don't compile it at all.

## The edit (manual fallback)

In `forwardMessageToParent()`, before the existing `GetParent`/`PostMessage`, add a
guarded block that posts the message to `GetAncestor(hwnd, GA_ROOT)` (when that root
differs from `hwnd`) and returns. See `apply-juce-keyboard-focus-patch.ps1` for the
exact before/after text.

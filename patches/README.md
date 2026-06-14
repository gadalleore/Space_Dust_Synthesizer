# Local JUCE patch — MPE without the phantom-parameter flood

Space Dust needs a small modification to its JUCE copy. It lives in the JUCE tree
(outside this repo), so it is **lost on a JUCE re-clone or update**. This folder
captures it so you can re-apply it.

## TL;DR — re-applying after a fresh JUCE clone/update

```powershell
./patches/apply-juce-mpe-patch.ps1
# or: ./patches/apply-juce-mpe-patch.ps1 -JucePath C:\path\to\JUCE
```

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

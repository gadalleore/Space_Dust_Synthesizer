#!/usr/bin/env python3
"""
apply-juce-mpe-patch.py

Portable (macOS / Linux / anywhere python3 lives) re-apply of the Space Dust local
JUCE MPE patch. Mirrors patches/apply-juce-mpe-patch.ps1 for hosts without PowerShell
(this Mac has no pwsh), and is what package-macos.sh calls so a local release can never
silently build WITHOUT the patch.

WHAT IT DOES
  Space Dust builds with JUCE_VST3_EMULATE_MIDI_CC_WITH_PARAMETERS=1 so the VST3 receives
  controller data (pitch-bend, channel pressure, CC74) that MPE rides on. JUCE's default
  for that ALSO exposes all 16*130 = 2080 phantom "MIDI CC" parameters, bloating the host
  automation list. This patch (guarded by JUCE_VST3_LIMIT_MIDI_CC_PARAMS, which the build
  defines) trims that to an allowlist of ~6 controllers while keeping MPE working. It lives
  in the JUCE tree, NOT this repo, so a JUCE update/re-clone LOSES it - run this to restore.

  Idempotent: already-patched hunks are skipped. If an anchor can't be found (JUCE changed),
  the script FAILS loudly rather than leaving a half-patched / unpatched build.

  Target: modules/juce_audio_plugin_client/juce_audio_plugin_client_VST3.cpp
  Validated against JUCE 8.0.12.

USAGE
  python3 patches/apply-juce-mpe-patch.py [--juce /path/to/JUCE]
  (falls back to $JUCE_DIR, then juce_path.local)
"""

import argparse
import os
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REL_TARGET = "modules/juce_audio_plugin_client/juce_audio_plugin_client_VST3.cpp"


def resolve_juce(cli_path):
    if cli_path:
        return cli_path
    if os.environ.get("JUCE_DIR"):
        return os.environ["JUCE_DIR"]
    local = os.path.join(REPO_ROOT, "juce_path.local")
    if os.path.isfile(local):
        with open(local, "r", encoding="utf-8") as fh:
            return fh.read().strip()
    return None


# -- Hunk 1: opt-in macro default ------------------------------------------------
H1_MARKER = "JUCE_VST3_LIMIT_MIDI_CC_PARAMS"
H1_OLD = """#ifndef JUCE_VST3_EMULATE_MIDI_CC_WITH_PARAMETERS
 #if JucePlugin_WantsMidiInput
  #define JUCE_VST3_EMULATE_MIDI_CC_WITH_PARAMETERS 1
 #endif
#endif
"""
H1_NEW = """#ifndef JUCE_VST3_EMULATE_MIDI_CC_WITH_PARAMETERS
 #if JucePlugin_WantsMidiInput
  #define JUCE_VST3_EMULATE_MIDI_CC_WITH_PARAMETERS 1
 #endif
#endif

// LOCAL PATCH (Space Dust): when a project defines JUCE_VST3_LIMIT_MIDI_CC_PARAMS=1,
// only a small allowlist of controllers (MPE pitch-bend/pressure/CC74 + mod/expr/
// sustain) is exposed as VST3 "MIDI CC" parameters / MIDI-mapped, instead of all
// 16*130 = 2080. Keeps MPE working WITHOUT the huge phantom-parameter surface.
// Defaults OFF so other plugins sharing this JUCE copy are unaffected.
#ifndef JUCE_VST3_LIMIT_MIDI_CC_PARAMS
 #define JUCE_VST3_LIMIT_MIDI_CC_PARAMS 0
#endif
"""

# -- Hunk 2: allowlist helper + getMidiControllerAssignment guard ----------------
H2_MARKER = "isAllowlistedMidiController"
H2_OLD = """    //==============================================================================
    tresult PLUGIN_API getMidiControllerAssignment ([[maybe_unused]] Steinberg::int32 busIndex,
                                                    [[maybe_unused]] Steinberg::int16 channel,
                                                    [[maybe_unused]] Vst::CtrlNumber midiControllerNumber,
                                                    [[maybe_unused]] Vst::ParamID& resultID) override
    {
       #if JUCE_VST3_EMULATE_MIDI_CC_WITH_PARAMETERS
        resultID = midiControllerToParameter[channel][midiControllerNumber];
        return kResultTrue; // Returning false makes some hosts stop asking for further MIDI Controller Assignments
       #else
        return kResultFalse;
       #endif
    }"""
H2_NEW = """    //==============================================================================
   #if JUCE_VST3_LIMIT_MIDI_CC_PARAMS
    // LOCAL PATCH (Space Dust): the controllers that stay exposed/MIDI-mapped when
    // JUCE_VST3_LIMIT_MIDI_CC_PARAMS is on. Covers the MPE expression dimensions plus
    // the common pedals/wheels, so a normal keyboard and an MPE controller both work.
    static bool isAllowlistedMidiController (int ctrlNumber) noexcept
    {
        switch (ctrlNumber)
        {
            case 1:                // CC1  mod wheel
            case 11:               // CC11 expression
            case 64:               // CC64 sustain pedal
            case 74:               // CC74 timbre (MPE slide / brightness)
            case Vst::kAfterTouch: // channel pressure (MPE pressure)
            case Vst::kPitchBend:  // pitch bend (MPE pitch)
                return true;
            default:
                return false;
        }
    }
   #endif

    tresult PLUGIN_API getMidiControllerAssignment ([[maybe_unused]] Steinberg::int32 busIndex,
                                                    [[maybe_unused]] Steinberg::int16 channel,
                                                    [[maybe_unused]] Vst::CtrlNumber midiControllerNumber,
                                                    [[maybe_unused]] Vst::ParamID& resultID) override
    {
       #if JUCE_VST3_EMULATE_MIDI_CC_WITH_PARAMETERS
       #if JUCE_VST3_LIMIT_MIDI_CC_PARAMS
        // Only map the allowlisted controllers; others report "no mapping" so the host
        // doesn't try to route them (they were never exposed as parameters either).
        if (! isAllowlistedMidiController ((int) midiControllerNumber))
            return kResultFalse;
       #endif
        resultID = midiControllerToParameter[channel][midiControllerNumber];
        return kResultTrue; // Returning false makes some hosts stop asking for further MIDI Controller Assignments
       #else
        return kResultFalse;
       #endif
    }"""

# -- Hunk 3: initialiseMidiControllerMappings - only expose allowlisted ----------
H3_MARKER = "Keep p (the paramID index space)"
H3_OLD = """                midiControllerToParameter[c][i] = static_cast<Vst::ParamID> (p) + parameterToMidiControllerOffset;
                parameterToMidiController[p].channel = c;
                parameterToMidiController[p].ctrlNumber = i;

                parameters.addParameter (new Vst::Parameter (toString ("MIDI CC " + String (c) + "|" + String (i)),"""
H3_NEW = """                midiControllerToParameter[c][i] = static_cast<Vst::ParamID> (p) + parameterToMidiControllerOffset;
                parameterToMidiController[p].channel = c;
                parameterToMidiController[p].ctrlNumber = i;

               #if JUCE_VST3_LIMIT_MIDI_CC_PARAMS
                // Keep p (the paramID index space) advancing for every controller so
                // getMidiControllerForParameter still decodes channel/ctrl correctly,
                // but only EXPOSE the allowlisted controllers as host parameters.
                if (! isAllowlistedMidiController (i))
                    continue;
               #endif

                parameters.addParameter (new Vst::Parameter (toString ("MIDI CC " + String (c) + "|" + String (i)),"""

HUNKS = [
    ("macro default", H1_MARKER, H1_OLD, H1_NEW),
    ("getMidiControllerAssignment guard", H2_MARKER, H2_OLD, H2_NEW),
    ("initialiseMidiControllerMappings guard", H3_MARKER, H3_OLD, H3_NEW),
]


def main():
    ap = argparse.ArgumentParser(description="Apply the Space Dust JUCE MPE patch (idempotent).")
    ap.add_argument("--juce", dest="juce", default=None, help="Path to the JUCE tree.")
    args = ap.parse_args()

    juce = resolve_juce(args.juce)
    if not juce:
        print("ERROR: JUCE path not given. Pass --juce, set JUCE_DIR, or create juce_path.local.", file=sys.stderr)
        return 1

    target = os.path.join(juce, REL_TARGET)
    if not os.path.isfile(target):
        print(f"ERROR: not found: {target}", file=sys.stderr)
        return 1

    # Normalise to LF so anchors match regardless of the checkout's line endings.
    with open(target, "r", encoding="utf-8", newline="") as fh:
        text = fh.read()
    text = text.replace("\r\n", "\n")

    applied = skipped = failed = 0
    for name, marker, old, new in HUNKS:
        if marker in text:
            print(f"  [skip] {name} (already applied)")
            skipped += 1
            continue
        if old not in text:
            print(f"  [FAIL] {name} - anchor text not found (JUCE version changed? see patches/README.md)", file=sys.stderr)
            failed += 1
            continue
        text = text.replace(old, new, 1)
        print(f"  [ok]   {name}")
        applied += 1

    if failed:
        print(f"RESULT: {failed} hunk(s) FAILED - NOT writing. See patches/README.md.", file=sys.stderr)
        return 1

    if applied:
        with open(target, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(text)
        print(f"RESULT: applied {applied}, skipped {skipped}. Patched: {target}")
    else:
        print(f"RESULT: nothing to do (all {skipped} hunk(s) already applied).")
    return 0


if __name__ == "__main__":
    sys.exit(main())

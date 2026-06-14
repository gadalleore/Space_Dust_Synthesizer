<#
    apply-juce-mpe-patch.ps1

    Re-applies the Space Dust local JUCE patch that limits the VST3 "MIDI CC"
    parameter surface to only the controllers MPE / pedals need (instead of all
    16*130 = 2080), while keeping MIDI-CC emulation ENABLED so MPE works.

    WHY THIS EXISTS
      Space Dust builds with JUCE_VST3_EMULATE_MIDI_CC_WITH_PARAMETERS=1 (see
      CMakeLists.txt) so the VST3 actually receives controller data - pitch-bend,
      channel pressure, CC74 - which MPE rides on. JUCE's default for that also
      exposes 2080 phantom "MIDI CC" parameters that bloat a host's automation
      list. This patch (guarded by JUCE_VST3_LIMIT_MIDI_CC_PARAMS, which the build
      defines) trims that to ~96 by only exposing/mapping an allowlist of
      controllers. The patch lives in the JUCE tree, NOT in this repo, so it is
      LOST on a JUCE re-clone or update - run this script to put it back.

    The edits are string-based and idempotent: already-patched files are skipped.
    Target: modules/juce_audio_plugin_client/juce_audio_plugin_client_VST3.cpp
    Validated against JUCE 8.0.12.

    Usage:
      ./patches/apply-juce-mpe-patch.ps1                 # uses $env:JUCE_DIR or juce_path.local
      ./patches/apply-juce-mpe-patch.ps1 -JucePath C:\path\to\JUCE
#>
param(
    [string]$JucePath
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot

# Resolve JUCE location: -JucePath > JUCE_DIR env > juce_path.local
if (-not $JucePath) { $JucePath = $env:JUCE_DIR }
if (-not $JucePath -and (Test-Path (Join-Path $repoRoot "juce_path.local"))) {
    $JucePath = (Get-Content (Join-Path $repoRoot "juce_path.local") -Raw).Trim()
}
if (-not $JucePath) {
    Write-Host "ERROR: JUCE path not given. Pass -JucePath, set JUCE_DIR, or create juce_path.local." -ForegroundColor Red
    exit 1
}

$file = Join-Path $JucePath "modules\juce_audio_plugin_client\juce_audio_plugin_client_VST3.cpp"
if (-not (Test-Path $file)) {
    Write-Host "ERROR: not found: $file" -ForegroundColor Red
    exit 1
}

$text = Get-Content -LiteralPath $file -Raw
$orig = $text
$applied = 0; $skipped = 0; $failed = 0

function Apply-Hunk {
    param([string]$Name, [string]$Marker, [string]$Old, [string]$New)
    if ($script:text.Contains($Marker)) {
        Write-Host "  [skip] $Name (already applied)" -ForegroundColor DarkGray
        $script:skipped++
        return
    }
    if (-not $script:text.Contains($Old)) {
        Write-Host "  [FAIL] $Name - anchor text not found (JUCE version changed? re-apply manually, see patches\README.md)" -ForegroundColor Red
        $script:failed++
        return
    }
    $script:text = $script:text.Replace($Old, $New)
    Write-Host "  [ok]   $Name" -ForegroundColor Green
    $script:applied++
}

# ---- Hunk 1: opt-in macro default ----
$h1Old = @'
#ifndef JUCE_VST3_EMULATE_MIDI_CC_WITH_PARAMETERS
 #if JucePlugin_WantsMidiInput
  #define JUCE_VST3_EMULATE_MIDI_CC_WITH_PARAMETERS 1
 #endif
#endif
'@
$h1New = @'
#ifndef JUCE_VST3_EMULATE_MIDI_CC_WITH_PARAMETERS
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
'@
Apply-Hunk -Name "macro default" -Marker "JUCE_VST3_LIMIT_MIDI_CC_PARAMS" -Old $h1Old -New $h1New

# ---- Hunk 2: allowlist helper + getMidiControllerAssignment guard ----
$h2Old = @'
    //==============================================================================
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
    }
'@
$h2New = @'
    //==============================================================================
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
    }
'@
Apply-Hunk -Name "getMidiControllerAssignment guard" -Marker "isAllowlistedMidiController" -Old $h2Old -New $h2New

# ---- Hunk 3: initialiseMidiControllerMappings - only expose allowlisted ----
$h3Old = @'
                midiControllerToParameter[c][i] = static_cast<Vst::ParamID> (p) + parameterToMidiControllerOffset;
                parameterToMidiController[p].channel = c;
                parameterToMidiController[p].ctrlNumber = i;

                parameters.addParameter (new Vst::Parameter (toString ("MIDI CC " + String (c) + "|" + String (i)),
'@
$h3New = @'
                midiControllerToParameter[c][i] = static_cast<Vst::ParamID> (p) + parameterToMidiControllerOffset;
                parameterToMidiController[p].channel = c;
                parameterToMidiController[p].ctrlNumber = i;

               #if JUCE_VST3_LIMIT_MIDI_CC_PARAMS
                // Keep p (the paramID index space) advancing for every controller so
                // getMidiControllerForParameter still decodes channel/ctrl correctly,
                // but only EXPOSE the allowlisted controllers as host parameters.
                if (! isAllowlistedMidiController (i))
                    continue;
               #endif

                parameters.addParameter (new Vst::Parameter (toString ("MIDI CC " + String (c) + "|" + String (i)),
'@
Apply-Hunk -Name "initialiseMidiControllerMappings guard" -Marker "Keep p (the paramID index space)" -Old $h3Old -New $h3New

Write-Host ""
if ($failed -gt 0) {
    Write-Host "RESULT: $failed hunk(s) FAILED. See patches\README.md to apply by hand." -ForegroundColor Red
    exit 1
}
if ($applied -gt 0) {
    Set-Content -LiteralPath $file -Value $text -NoNewline -Encoding UTF8
    Write-Host "RESULT: applied $applied, skipped $skipped. Patched: $file" -ForegroundColor Green
} else {
    Write-Host "RESULT: nothing to do (all $skipped hunk(s) already applied)." -ForegroundColor Green
}
exit 0

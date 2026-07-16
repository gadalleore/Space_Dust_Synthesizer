<#
    apply-juce-keyboard-focus-patch.ps1

    Re-applies the Space Dust local JUCE patch that fixes FL Studio's "Typing
    keyboard to piano" dying whenever the plugin holds keyboard focus.

    WHY THIS EXISTS
      When a key the plugin editor does not handle is pressed while the editor
      has keyboard focus, JUCE forwards that Windows key message to the editor's
      DIRECT parent (GetParent). In FL Studio the direct parent is FL's plugin-
      wrapper child window, which does NOT run "Typing keyboard to piano", so the
      keystroke is dropped and the host's computer-keyboard MIDI stops. (Ableton's
      direct parent DOES handle it, which is why it only bites in FL. A hardware
      MIDI controller never uses this path, so it is unaffected.)

      The fix forwards unhandled key/char messages to the ROOT window
      (GetAncestor GA_ROOT) - the host's top-level window - which routes them
      correctly. It is guarded by JUCE_FORWARD_KEYS_TO_ROOT_WINDOW, which Space
      Dust's CMakeLists.txt defines; the macro is undefined (=> no-op) for other
      plugins sharing this JUCE copy, so they are unaffected. Windows-only.

      Known JUCE<->FL issue:
        https://forum.juce.com/t/return-keyboard-focus-in-fl-studio/50711

      The patch lives in the JUCE tree, NOT in this repo, so it is LOST on a JUCE
      re-clone or update - run this script to put it back.

    The edit is string-based and idempotent: an already-patched file is skipped.
    Target: modules/juce_gui_basics/native/juce_Windowing_windows.cpp
    Validated against JUCE 8.0.12.

    Usage:
      ./patches/apply-juce-keyboard-focus-patch.ps1                 # uses $env:JUCE_DIR or juce_path.local
      ./patches/apply-juce-keyboard-focus-patch.ps1 -JucePath C:\path\to\JUCE
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

$file = Join-Path $JucePath "modules\juce_gui_basics\native\juce_Windowing_windows.cpp"
if (-not (Test-Path $file)) {
    Write-Host "ERROR: not found: $file" -ForegroundColor Red
    exit 1
}

$text = Get-Content -LiteralPath $file -Raw

$marker = "JUCE_FORWARD_KEYS_TO_ROOT_WINDOW"
if ($text.Contains($marker)) {
    Write-Host "  [skip] forwardMessageToParent (already applied)" -ForegroundColor DarkGray
    Write-Host "RESULT: nothing to do (already applied)." -ForegroundColor Green
    exit 0
}

$old = @'
    void forwardMessageToParent (UINT message, WPARAM wParam, LPARAM lParam) const
    {
        if (HWND parentH = GetParent (hwnd))
            PostMessage (parentH, message, wParam, lParam);
    }
'@

$new = @'
    void forwardMessageToParent (UINT message, WPARAM wParam, LPARAM lParam) const
    {
       #if JUCE_FORWARD_KEYS_TO_ROOT_WINDOW
        // LOCAL PATCH (Space Dust): forward unhandled key/char messages to the ROOT
        // window instead of the direct parent. In FL Studio the plugin editor's direct
        // parent is the wrapper child window, which does NOT run "Typing keyboard to
        // piano", so unhandled keystrokes are dropped and the host's computer-keyboard
        // MIDI stops whenever the plugin holds keyboard focus. GA_ROOT is the host's
        // top-level window, which routes them correctly. Guarded by a macro Space
        // Dust's CMake defines; undefined (=> 0) for other plugins sharing this JUCE.
        if (HWND root = GetAncestor (hwnd, GA_ROOT))
        {
            if (root != hwnd)
            {
                PostMessage (root, message, wParam, lParam);
                return;
            }
        }
       #endif

        if (HWND parentH = GetParent (hwnd))
            PostMessage (parentH, message, wParam, lParam);
    }
'@

if (-not $text.Contains($old)) {
    Write-Host "  [FAIL] forwardMessageToParent - anchor text not found (JUCE version changed? see patches\README.md)" -ForegroundColor Red
    exit 1
}

$text = $text.Replace($old, $new)
Set-Content -LiteralPath $file -Value $text -NoNewline -Encoding UTF8
Write-Host "  [ok]   forwardMessageToParent" -ForegroundColor Green
Write-Host "RESULT: applied. Patched: $file" -ForegroundColor Green
exit 0

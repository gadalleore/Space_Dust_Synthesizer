#==============================================================================
# Space Dust - Automated Ableton Live Launcher
# Launches Ableton Live after successful build + copy, if not already running
#==============================================================================

#==============================================================================
# CONFIGURATION - Edit these paths for your setup
#==============================================================================

# Preferred Ableton Live executable path (update if you use a custom install location)
# If empty, the script will search common install paths for Live 10/11/12.
# Example: $AbletonExePath = "C:\ProgramData\Ableton\Live 11 Suite\Program\Ableton Live 11 Suite.exe"
$AbletonExePath = ""

# Optional: Path to test project file (leave empty "" to launch normally)
# Example: $TestProjectPath = "C:\Ableton Projects\SpaceDust_Test.als"
$TestProjectPath = ""

# Custom VST3 folder (where you want the plugin copied for testing)
$CustomVST3Folder = "$env:USERPROFILE\Documents\Ableton\User Library\VST3"

# Delay after copy (seconds) - allows filesystem to settle
$PostCopyDelaySeconds = 2

#==============================================================================
# Main Logic
#==============================================================================

Write-Host "`n[Space Dust] Checking Ableton Live status..." -ForegroundColor Cyan

# Check if Ableton Live is already running (process name starts with "Live")
$abletonRunning = Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.ProcessName -like "Live*" }

if ($abletonRunning) {
    Write-Host "[Space Dust] Ableton Live is already running. Bringing to front..." -ForegroundColor Green
    # Bring Ableton window to foreground
    try {
        Add-Type -TypeDefinition @"
using System; using System.Runtime.InteropServices;
public class Win32 { [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd); }
"@ -ErrorAction SilentlyContinue
        $proc = $abletonRunning | Select-Object -First 1
        if ($proc.MainWindowHandle -ne [IntPtr]::Zero) {
            [Win32]::SetForegroundWindow($proc.MainWindowHandle) | Out-Null
        }
    } catch { }
    Write-Host "[Space Dust] Rescan plugins in Ableton to load the updated Space Dust VST3" -ForegroundColor Yellow
    exit 0
}

Write-Host "[Space Dust] Ableton Live is not running. Launching..." -ForegroundColor Yellow

# Resolve Ableton executable: use configured path, or search common install locations
if ($AbletonExePath -ne "" -and (Test-Path $AbletonExePath)) {
    Write-Host "[Space Dust] Using: $AbletonExePath" -ForegroundColor Gray
}
else {
    $AbletonExePath = $null
    $roots = @(
        "C:\ProgramData\Ableton",
        "C:\Program Files\Ableton",
        (Join-Path ${env:ProgramFiles} "Ableton"),
        (Join-Path ${env:ProgramFiles(x86)} "Ableton")
    )
    # 1) Try known patterns: Live 12/11/10 with Suite, Standard, Intro; or "Live N" only
    $patterns = @()
    foreach ($v in 12,11,10) {
        $patterns += "Live $v Suite\Program\Ableton Live $v Suite.exe"
        $patterns += "Live $v Standard\Program\Ableton Live $v Standard.exe"
        $patterns += "Live $v Intro\Program\Ableton Live $v Intro.exe"
        $patterns += "Live $v\Program\Ableton Live $v.exe"
    }
    foreach ($root in $roots) {
        if (-not (Test-Path $root)) { continue }
        foreach ($p in $patterns) {
            $c = Join-Path $root $p
            if (Test-Path $c) { $AbletonExePath = $c; break }
        }
        if ($AbletonExePath) { break }
    }
    # 2) Fallback: any "Ableton Live *.exe" inside ...\Program\ under an Ableton root
    if (-not $AbletonExePath) {
        foreach ($root in $roots) {
            if (-not (Test-Path $root)) { continue }
            $found = Get-ChildItem -Path $root -Recurse -Filter "Ableton Live*.exe" -ErrorAction SilentlyContinue |
                Where-Object { $_.FullName -match "\\Program\\" } | Select-Object -First 1
            if ($found) { $AbletonExePath = $found.FullName; break }
        }
    }
    if ($AbletonExePath) { Write-Host "[Space Dust] Found: $AbletonExePath" -ForegroundColor Gray }
}

if (-not $AbletonExePath -or -not (Test-Path $AbletonExePath)) {
    Write-Host "[Space Dust] Ableton Live executable NOT found." -ForegroundColor Red
    Write-Host "[Space Dust] Open launch-ableton.ps1 and set this line near the top:" -ForegroundColor Yellow
    Write-Host '  $AbletonExePath = "C:\ProgramData\Ableton\Live 11 Suite\Program\Ableton Live 11 Suite.exe"' -ForegroundColor Gray
    Write-Host "[Space Dust] Use your real path. Tip: right-click Ableton shortcut > Open file location." -ForegroundColor Gray
    exit 1
}

# Prepare launch arguments
$launchArgs = @()

# If test project is specified and exists, open it
if ($TestProjectPath -ne "" -and (Test-Path $TestProjectPath)) {
    Write-Host "[Space Dust] Opening test project: $TestProjectPath" -ForegroundColor Cyan
    $launchArgs = @($TestProjectPath)
}
elseif ($TestProjectPath -ne "" -and -not (Test-Path $TestProjectPath)) {
    Write-Host "[Space Dust] WARNING: Test project not found: $TestProjectPath" -ForegroundColor Yellow
    Write-Host "[Space Dust] Launching Ableton normally (no project)" -ForegroundColor Gray
}

# Launch Ableton Live
try {
    if ($launchArgs.Count -gt 0) {
        Start-Process -FilePath $AbletonExePath -ArgumentList $launchArgs -ErrorAction Stop
    }
    else {
        Start-Process -FilePath $AbletonExePath -ErrorAction Stop
    }
    Write-Host "[Space Dust] Ableton Live launched successfully!" -ForegroundColor Green
}
catch {
    Write-Host "[Space Dust] ERROR: Failed to launch Ableton Live" -ForegroundColor Red
    Write-Host "              $($_.Exception.Message)" -ForegroundColor Gray
    exit 1
}

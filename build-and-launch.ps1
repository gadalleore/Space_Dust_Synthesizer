#==============================================================================
# Space Dust - Build and Launch Script
# Incremental build, copies VST3 to Program Files, launches/focuses Ableton.
#==============================================================================

param(
    [switch]$NoLaunch  # Pass -NoLaunch to skip opening Ableton
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $projectRoot

# ── Step 1: CMake configure (only if build dir doesn't exist) ──────────────
if (-not (Test-Path "build\CMakeCache.txt")) {
    Write-Host "[Space Dust] No build directory found, configuring CMake..." -ForegroundColor Yellow

    $juceDir = $env:JUCE_DIR
    if (-not $juceDir -and (Test-Path "juce_path.local")) {
        $juceDir = (Get-Content "juce_path.local" -Raw).Trim()
    }

    New-Item -ItemType Directory -Path build -Force | Out-Null
    Set-Location build

    $cmakeArgs = @("..", "-G", "Visual Studio 17 2022", "-A", "x64")
    if ($juceDir) { $cmakeArgs += "-DJUCE_DIR=$juceDir" }

    & cmake @cmakeArgs
    if ($LASTEXITCODE -ne 0) {
        # Retry with VS 2026
        $cmakeArgs[2] = "Visual Studio 18 2026"
        & cmake @cmakeArgs
        if ($LASTEXITCODE -ne 0) {
            Write-Host "[Space Dust] CMake configure failed." -ForegroundColor Red
            Set-Location $projectRoot; exit 1
        }
    }
    Set-Location $projectRoot
}

# ── Step 2: Incremental build ──────────────────────────────────────────────
Write-Host "[Space Dust] Building..." -ForegroundColor Cyan
cmake --build build --config Release --target SpaceDust_VST3
if ($LASTEXITCODE -ne 0) {
    Write-Host "[Space Dust] Build failed." -ForegroundColor Red
    exit 1
}
Write-Host "[Space Dust] Build succeeded." -ForegroundColor Green

# ── Step 3: Copy VST3 to all known VST3 locations ─────────────────────────
$source = "build\SpaceDust_artefacts\Release\VST3\Space Dust.vst3"
if (-not (Test-Path $source)) {
    Write-Host "[Space Dust] VST3 artifact not found at: $source" -ForegroundColor Red
    exit 1
}

$destinations = @(
    "C:\Program Files\Common Files\VST3\Space Dust.vst3",
    "$env:USERPROFILE\Documents\Ableton\User Library\VST3\Space Dust.vst3",
    "$env:USERPROFILE\Documents\VST3\Space Dust.vst3",
    "$env:USERPROFILE\VST3\Space Dust.vst3"
)

foreach ($dest in $destinations) {
    $destDir = Split-Path -Parent $dest
    if (-not (Test-Path $destDir)) { continue }  # skip if parent folder doesn't exist
    Write-Host "[Space Dust] Copying VST3 to $dest..." -ForegroundColor Cyan
    if (Test-Path $dest) { Remove-Item -Recurse -Force $dest }
    Copy-Item -Path $source -Destination $dest -Recurse -Force
    Write-Host "[Space Dust] Copy complete -> $dest" -ForegroundColor Green
}

# ── Step 4: Launch or focus Ableton ───────────────────────────────────────
if ($NoLaunch) {
    Write-Host "[Space Dust] Done. (-NoLaunch: skipping Ableton)" -ForegroundColor Gray
    exit 0
}

$running = Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.ProcessName -like "Live*" }
if ($running) {
    Write-Host "[Space Dust] Ableton is running - bringing to front..." -ForegroundColor Green
    try {
        Add-Type -TypeDefinition @"
using System; using System.Runtime.InteropServices;
public class Win32 { [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd); }
"@ -ErrorAction SilentlyContinue
        $proc = $running | Select-Object -First 1
        if ($proc.MainWindowHandle -ne [IntPtr]::Zero) {
            [Win32]::SetForegroundWindow($proc.MainWindowHandle) | Out-Null
        }
    } catch { }
    Write-Host "[Space Dust] Rescan plugins in Ableton (Prefs > Plug-Ins > Rescan) to pick up the new build." -ForegroundColor Yellow
    exit 0
}

# Find Ableton executable
$abletonExe = $null
$roots = @("C:\ProgramData\Ableton", "C:\Program Files\Ableton", "$env:ProgramFiles\Ableton")
foreach ($root in $roots) {
    if (-not (Test-Path $root)) { continue }
    $found = Get-ChildItem -Path $root -Recurse -Filter "Ableton Live*.exe" -ErrorAction SilentlyContinue |
             Where-Object { $_.FullName -match "\\Program\\" } | Select-Object -First 1
    if ($found) { $abletonExe = $found.FullName; break }
}

if (-not $abletonExe) {
    Write-Host "[Space Dust] Ableton Live not found. Set `$AbletonExePath in launch-ableton.ps1 or launch manually." -ForegroundColor Yellow
    exit 0
}

Write-Host "[Space Dust] Launching $abletonExe..." -ForegroundColor Cyan
Start-Process -FilePath $abletonExe
Write-Host "[Space Dust] Ableton launched." -ForegroundColor Green

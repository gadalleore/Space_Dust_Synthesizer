#==============================================================================
# Space Dust - Build and Launch Script (Cleaned)
# Incremental build + copy VST3 to all common locations + launch/focus Ableton
#==============================================================================

param(
    [switch]$NoLaunch
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $projectRoot

# --- Step 1: Configure (only if needed) ---
if (-not (Test-Path "build\CMakeCache.txt")) {
    Write-Host "[Space Dust] Configuring CMake..." -ForegroundColor Yellow
    $juceDir = $env:JUCE_DIR
    if (-not $juceDir -and (Test-Path "juce_path.local")) {
        $juceDir = (Get-Content "juce_path.local" -Raw).Trim()
    }
    New-Item -ItemType Directory -Path build -Force | Out-Null
    Set-Location build

    # Try VS 2022 first, then fall back to VS 2026
    $cmakeArgs = @("..", "-G", "Visual Studio 17 2022", "-A", "x64")
    if ($juceDir) { $cmakeArgs += "-DJUCE_DIR=$juceDir" }

    & cmake @cmakeArgs
    if ($LASTEXITCODE -ne 0) {
        Write-Host "[Space Dust] VS 2022 generator failed, trying VS 2026..." -ForegroundColor Yellow
        $cmakeArgs[2] = "Visual Studio 18 2026"
        & cmake @cmakeArgs
    }

    if ($LASTEXITCODE -ne 0) {
        Write-Host "[Space Dust] CMake configure failed with both generators." -ForegroundColor Red
        Set-Location $projectRoot
        exit 1
    }
    Set-Location $projectRoot
}

# --- Step 2: Build ---
Write-Host "[Space Dust] Building Release VST3..." -ForegroundColor Cyan
cmake --build build --config Release --target SpaceDust_VST3
if ($LASTEXITCODE -ne 0) {
    Write-Host "[Space Dust] Build failed." -ForegroundColor Red
    exit 1
}
Write-Host "[Space Dust] Build succeeded." -ForegroundColor Green

# --- Step 3: Copy to ALL locations (prevents stale versions) ---
$source = "build\SpaceDust_artefacts\Release\VST3\Space Dust.vst3"
if (-not (Test-Path $source)) {
    Write-Host "[Space Dust] VST3 not found at $source" -ForegroundColor Red
    exit 1
}

$destinations = @(
    "C:\Program Files\Common Files\VST3\Space Dust.vst3",
    "$env:USERPROFILE\Documents\Ableton\User Library\VST3\Space Dust.vst3",
    "$env:USERPROFILE\Documents\VST3\Space Dust.vst3",
    "$env:USERPROFILE\VST3\Space Dust.vst3",
    "$env:APPDATA\VST3\Space Dust.vst3"
)

Write-Host "[Space Dust] Copying fresh VST3 bundle to all locations..." -ForegroundColor Cyan
foreach ($dest in $destinations) {
    $destDir = Split-Path -Parent $dest
    if (-not (Test-Path $destDir)) {
        New-Item -ItemType Directory -Path $destDir -Force | Out-Null
    }
    if (Test-Path $dest) { Remove-Item -Recurse -Force $dest -ErrorAction SilentlyContinue }
    Copy-Item -Path $source -Destination $dest -Recurse -Force
    Write-Host "  Updated: $dest" -ForegroundColor Green
}
Write-Host "[Space Dust] All VST3 locations are now in sync." -ForegroundColor Green

# --- Step 4: Launch / focus Ableton ---
if ($NoLaunch) {
    Write-Host "[Space Dust] Done (-NoLaunch)." -ForegroundColor Gray
    exit 0
}

$running = Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.ProcessName -like "Live*" }
if ($running) {
    Write-Host "[Space Dust] Ableton is running - bringing to front..." -ForegroundColor Green
    try {
        Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public class Win32 {
    [DllImport("user32.dll")]
    public static extern bool SetForegroundWindow(IntPtr hWnd);
}
"@ -ErrorAction SilentlyContinue
        $proc = $running | Select-Object -First 1
        if ($proc.MainWindowHandle -ne [IntPtr]::Zero) {
            [Win32]::SetForegroundWindow($proc.MainWindowHandle) | Out-Null
        }
    } catch {}
    Write-Host "[Space Dust] Rescan plugins in Ableton to load the new build." -ForegroundColor Yellow
    exit 0
}

# Try to launch Ableton
$abletonExe = $null
$roots = @("C:\ProgramData\Ableton", "C:\Program Files\Ableton", "$env:ProgramFiles\Ableton")
foreach ($root in $roots) {
    if (-not (Test-Path $root)) { continue }
    $found = Get-ChildItem -Path $root -Recurse -Filter "Ableton Live*.exe" -ErrorAction SilentlyContinue |
             Where-Object { $_.FullName -match "\\Program\\" } | Select-Object -First 1
    if ($found) { $abletonExe = $found.FullName; break }
}

if ($abletonExe) {
    Write-Host "[Space Dust] Launching $abletonExe..." -ForegroundColor Cyan
    Start-Process -FilePath $abletonExe
} else {
    Write-Host "[Space Dust] Ableton not found. Please launch manually and rescan plugins." -ForegroundColor Yellow
}

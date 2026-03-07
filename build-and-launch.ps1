#==============================================================================
# Space Dust - Complete Build and Launch Script
# Builds the plugin, copies it, then launches (or activates) Ableton Live.
# Ableton opens after every successful build unless you pass -NoLaunch.
#==============================================================================

param(
    [switch]$NoLaunch  # Pass -NoLaunch to build and copy only, without opening Ableton
)

$ErrorActionPreference = "Stop"

Write-Host "`n[Space Dust] Starting build process..." -ForegroundColor Cyan

# Get project root
$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $projectRoot

# Step 1: Clean build directory
Write-Host "[Space Dust] Cleaning build directory..." -ForegroundColor Yellow
Remove-Item -Recurse -Force build -ErrorAction SilentlyContinue
mkdir build -ErrorAction SilentlyContinue | Out-Null

# Step 2: Generate CMake project
Write-Host "[Space Dust] Generating CMake project..." -ForegroundColor Yellow
$juceDir = $env:JUCE_DIR
if (-not $juceDir -and (Test-Path (Join-Path $projectRoot "juce_path.local"))) {
    $juceDir = (Get-Content (Join-Path $projectRoot "juce_path.local") -Raw).Trim()
}
$cmakeArgs = @("..", "-G", "Visual Studio 17 2022", "-A", "x64")
if ($juceDir) { $cmakeArgs += "-DJUCE_DIR=$juceDir" }
Set-Location build
& cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) {
    Write-Host "[Space Dust] ✗ CMake generation failed!" -ForegroundColor Red
    Set-Location ..
    exit 1
}

# Step 3: Build Release
Write-Host "[Space Dust] Building Release configuration..." -ForegroundColor Yellow
cmake --build . --config Release
if ($LASTEXITCODE -ne 0) {
    Write-Host "[Space Dust] ✗ Build failed!" -ForegroundColor Red
    Set-Location ..
    exit 1
}

Set-Location ..

# Step 4: Copy VST3 to folders Ableton can load from
Write-Host "[Space Dust] Copying VST3..." -ForegroundColor Yellow
$sourceVST3 = "build\SpaceDust_artefacts\Release\VST3\Space Dust.vst3"
if (-not (Test-Path $sourceVST3)) {
    Write-Host "[Space Dust] WARNING: VST3 not found at $sourceVST3" -ForegroundColor Yellow
    Write-Host "[Space Dust] Build completed, but plugin not found. Skipping copy and Ableton launch." -ForegroundColor Yellow
    exit 0
}

# 4a) System VST3 (Ableton often scans this by default)
$systemVST3 = "C:\Program Files\Common Files\VST3"
$destSystem = Join-Path $systemVST3 "Space Dust.vst3"
try {
    if (Test-Path $systemVST3) {
        Copy-Item -Path $sourceVST3 -Destination $destSystem -Recurse -Force -ErrorAction Stop
        Write-Host "[Space Dust] Copied to: $destSystem" -ForegroundColor Green
    }
} catch {
    Write-Host "[Space Dust] Could not copy to $systemVST3 (may need admin). Using User Library." -ForegroundColor Yellow
}

# 4b) Ableton User Library VST3 (in case you use a custom VST folder)
$customVST3 = "$env:USERPROFILE\Documents\Ableton\User Library\VST3"
if (-not (Test-Path $customVST3)) { New-Item -ItemType Directory -Path $customVST3 -Force | Out-Null }
Copy-Item -Path $sourceVST3 -Destination $customVST3 -Recurse -Force -ErrorAction SilentlyContinue
Write-Host "[Space Dust] Copied to: $customVST3" -ForegroundColor Green

# Step 5: Wait for filesystem to settle
Write-Host "[Space Dust] Waiting 2 seconds for filesystem to settle..." -ForegroundColor Gray
Start-Sleep -Seconds 2

# Step 6: Launch Ableton Live (if not disabled)
$launchOk = $true
if (-not $NoLaunch) {
    Write-Host "[Space Dust] Launching Ableton Live..." -ForegroundColor Yellow
    if (Test-Path ".\launch-ableton.ps1") {
        & .\launch-ableton.ps1
        if ($LASTEXITCODE -ne 0) {
            $launchOk = $false
            Write-Host "[Space Dust] Ableton did not start (launch-ableton.ps1 returned $LASTEXITCODE)." -ForegroundColor Red
            Write-Host "[Space Dust] Edit launch-ableton.ps1 and set `$AbletonExePath to your Ableton .exe, then run again." -ForegroundColor Yellow
        }
    }
    else {
        Write-Host "[Space Dust] launch-ableton.ps1 not found - skipping Ableton launch" -ForegroundColor Yellow
        $launchOk = $false
    }
}
else {
    Write-Host "[Space Dust] Skipping Ableton launch (NoLaunch flag set)" -ForegroundColor Gray
}

Write-Host "`n[Space Dust] Build complete! Space Dust.vst3 is ready." -ForegroundColor Green
Write-Host "[Space Dust] To see changes in Ableton: Rescan (Prefs > Plug-Ins > Rescan) or remove and re-add the instrument." -ForegroundColor Yellow
if (-not $NoLaunch -and $launchOk) {
    Write-Host "[Space Dust] Ableton launch initiated." -ForegroundColor Green
}

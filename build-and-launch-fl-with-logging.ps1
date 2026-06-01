#==============================================================================
# Space Dust - Build & Launch (LOGGING variant, FL STUDIO)
# Builds the RelWithDebInfo + MemorySafetyLogger bundle in build-safety/, copies
# it to every common VST3 path, then launches / focuses FL Studio.
# FL clone of build-and-launch-with-logging.ps1 (which targets Ableton).
#
# Logs land in: %APPDATA%\63C\Space Dust\Logs\Safety\
#   form: SpaceDust_Safety_<yyyy-MM-dd_HH-mm-ss>_PID<n>.log
# Tail the newest live:
#   Get-ChildItem "$env:APPDATA\63C\Space Dust\Logs\Safety" *.log |
#     Sort-Object LastWriteTime -Desc | Select-Object -First 1 | Get-Content -Wait
#==============================================================================
param([switch]$NoLaunch, [switch]$NoBuild)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $projectRoot

$buildDir = "build-safety"
$config   = "RelWithDebInfo"
$source   = "$buildDir\SpaceDust_artefacts\$config\VST3\Space Dust.vst3"

# --- Step 1: Build the RelWithDebInfo + logging bundle ---
if (-not $NoBuild) {
    if (-not (Test-Path (Join-Path $buildDir "CMakeCache.txt"))) {
        Write-Host "[Space Dust] $buildDir not configured - running enable-safety-logging.ps1..." -ForegroundColor Yellow
        & "$projectRoot\enable-safety-logging.ps1" -BuildDir $buildDir -Config $config
        if ($LASTEXITCODE -ne 0) { Write-Host "[Space Dust] Configure/build failed." -ForegroundColor Red; exit 1 }
    } else {
        Write-Host "[Space Dust] Building $buildDir ($config)..." -ForegroundColor Cyan
        cmake --build $buildDir --config $config --target SpaceDust_VST3 --parallel
        if ($LASTEXITCODE -ne 0) { Write-Host "[Space Dust] Build failed." -ForegroundColor Red; exit 1 }
    }
}
if (-not (Test-Path $source)) {
    Write-Host "[Space Dust] Bundle missing after build: $source" -ForegroundColor Red
    exit 1
}
Write-Host "[Space Dust] Source: $source" -ForegroundColor Green

# --- Step 2: Verify the logging needle in the source DLL ---
$srcDll = Join-Path $source "Contents\x86_64-win\Space Dust.vst3"
$bytes  = [System.IO.File]::ReadAllBytes($srcDll)
$nb     = [System.Text.Encoding]::ASCII.GetBytes("SpaceDust_Safety_")
$found  = $false
for ($i = 0; $i -lt $bytes.Length - $nb.Length; $i++) {
    $hit = $true
    for ($j = 0; $j -lt $nb.Length; $j++) { if ($bytes[$i+$j] -ne $nb[$j]) { $hit = $false; break } }
    if ($hit) { $found = $true; break }
}
if (-not $found) {
    Write-Host "[Space Dust] Logging needle 'SpaceDust_Safety_' NOT in source DLL - aborting." -ForegroundColor Red
    exit 1
}
Write-Host "[Space Dust] Logging needle confirmed in source." -ForegroundColor Green

# --- Step 3: Destinations (split admin / user) ---
# FL Studio scans C:\Program Files\Common Files\VST3 by default - the admin copy
# below is the one FL actually loads (it is where the crashing instance loaded from).
# Deliberately NOT copying to the 32-bit "Program Files (x86)\Common Files\VST3":
# this is a 64-bit-only plugin, and FL scans both folders, so a copy there shows up
# as a confusing duplicate ("Space Dust_2") in FL's plugin database.
$adminDests = @(
    "C:\Program Files\Common Files\VST3\Space Dust.vst3"
)
$userDests  = @(
    "$env:USERPROFILE\Documents\Ableton\User Library\VST3\Space Dust.vst3",
    "$env:USERPROFILE\Documents\VST3\Space Dust.vst3",
    "$env:USERPROFILE\VST3\Space Dust.vst3",
    "$env:APPDATA\VST3\Space Dust.vst3"
)

# --- Step 4: User-scope copies (no elevation) ---
Write-Host "[Space Dust] Copying to user-scope VST3 paths..." -ForegroundColor Cyan
foreach ($dest in $userDests) {
    $destDir = Split-Path -Parent $dest
    if (-not (Test-Path $destDir)) { New-Item -ItemType Directory -Path $destDir -Force | Out-Null }
    if (Test-Path $dest) { Remove-Item -Recurse -Force $dest -ErrorAction SilentlyContinue }
    Copy-Item -Path $source -Destination $dest -Recurse -Force
    Write-Host "  Updated: $dest" -ForegroundColor Green
}

# --- Step 5: Admin-scope copies (elevated relauncher) ---
Write-Host "[Space Dust] Copying to Program Files (elevation prompt)..." -ForegroundColor Cyan
$srcAbs = (Resolve-Path $source).Path
$elevatedScript = @"
`$ErrorActionPreference = 'Stop'
`$src = '$srcAbs'
`$dests = @(
    'C:\Program Files\Common Files\VST3\Space Dust.vst3'
)
foreach (`$d in `$dests) {
    `$dir = Split-Path -Parent `$d
    if (-not (Test-Path `$dir)) { New-Item -ItemType Directory -Path `$dir -Force | Out-Null }
    if (Test-Path `$d) { Remove-Item -Recurse -Force `$d -ErrorAction SilentlyContinue }
    Copy-Item -Path `$src -Destination `$d -Recurse -Force
}
"@
$elevatedPath = Join-Path $env:TEMP "spacedust-elevated-copy.ps1"
Set-Content -Path $elevatedPath -Value $elevatedScript -Encoding UTF8
Start-Process powershell -Verb RunAs -Wait -ArgumentList "-NoProfile","-ExecutionPolicy","Bypass","-File",$elevatedPath
Remove-Item $elevatedPath -ErrorAction SilentlyContinue

foreach ($d in $adminDests) {
    if (Test-Path (Join-Path $d "Contents\x86_64-win\Space Dust.vst3")) {
        Write-Host "  Updated: $d" -ForegroundColor Green
    } else {
        Write-Host "  MISSING: $d" -ForegroundColor Red
    }
}

# --- Step 6: Launch / focus FL Studio (non-elevated) ---
if ($NoLaunch) { Write-Host "[Space Dust] Done (-NoLaunch)." -ForegroundColor Gray; exit 0 }

# FL's 64-bit process is FL64 (older builds: FL). Match either.
$running = Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.ProcessName -in @("FL64","FL") }
if ($running) {
    Write-Host "[Space Dust] FL Studio is running - bringing to front..." -ForegroundColor Green
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
    Write-Host "[Space Dust] Rescan plugins in FL (Options > Manage plugins > Find installed plugins) to load the new build." -ForegroundColor Yellow
    exit 0
}

# Find FL64.exe under the Image-Line install tree.
$flExe = $null
$flCandidates = @(
    "C:\Program Files\Image-Line\FL Studio 2025\FL64.exe",
    "C:\Program Files\Image-Line\FL Studio 2024\FL64.exe"
)
foreach ($c in $flCandidates) { if (Test-Path $c) { $flExe = $c; break } }
if (-not $flExe) {
    $root = "C:\Program Files\Image-Line"
    if (Test-Path $root) {
        $hit = Get-ChildItem -Path $root -Recurse -Filter "FL64.exe" -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($hit) { $flExe = $hit.FullName }
    }
}
if ($flExe) {
    Write-Host "[Space Dust] Launching $flExe..." -ForegroundColor Cyan
    Start-Process -FilePath $flExe
} else {
    Write-Host "[Space Dust] FL Studio not found. Please launch it manually and rescan plugins." -ForegroundColor Yellow
}

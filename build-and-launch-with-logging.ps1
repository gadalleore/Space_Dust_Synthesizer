#==============================================================================
# Space Dust - Build & Launch (LOGGING variant)
# Uses the Debug+MemorySafetyLogger bundle from build-safety/ instead of the
# Release bundle from build/. Copies to every common VST3 path and launches /
# focuses Ableton, matching build-and-launch.ps1's behavior.
#==============================================================================
param([switch]$NoLaunch)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $projectRoot

# --- Step 1: Ensure the Debug+logging bundle exists (build if missing) ---
$source = "build-safety\SpaceDust_artefacts\RelWithDebInfo\VST3\Space Dust.vst3"
if (-not (Test-Path $source)) {
    Write-Host "[Space Dust] build-safety bundle missing - running enable-safety-logging.ps1..." -ForegroundColor Yellow
    & "$projectRoot\enable-safety-logging.ps1"
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $source)) {
        Write-Host "[Space Dust] Safety-logging build failed." -ForegroundColor Red
        exit 1
    }
}
Write-Host "[Space Dust] Source: $source" -ForegroundColor Green

# --- Step 2: Verify logging needle in the source DLL ---
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
$adminDests = @(
    "C:\Program Files\Common Files\VST3\Space Dust.vst3",
    "C:\Program Files (x86)\Common Files\VST3\Space Dust.vst3"
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
    'C:\Program Files\Common Files\VST3\Space Dust.vst3',
    'C:\Program Files (x86)\Common Files\VST3\Space Dust.vst3'
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

# --- Step 6: Launch / focus Ableton (non-elevated) ---
if ($NoLaunch) { Write-Host "[Space Dust] Done (-NoLaunch)." -ForegroundColor Gray; exit 0 }

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

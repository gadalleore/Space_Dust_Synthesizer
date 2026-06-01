#==============================================================================
# Space Dust - Build and Launch Script
# Incremental build + deploy VST3 to all common locations + launch/focus Ableton
#
# Hardened against two recurring problems:
#   1. LNK1104 on "Space Dust_SharedCode.lib" - a parallel-build race plus a
#      stale or antivirus-locked .lib that the linker cannot open. Fixed by
#      building the SharedCode target first, then the VST3 target; on failure we
#      delete the stale .lib and retry once.
#   2. Program Files copy nesting / "access denied". Plain Copy-Item nests the
#      bundle inside itself when it cannot replace the old one (no admin) and a
#      running DAW (Cubase/Live/FL) locks the loaded DLL. Fixed by using
#      robocopy /MIR (mirrors files AND purges any nested folder), elevating for
#      Program Files, and detecting when a DAW still holds the DLL so we can tell
#      the user to close it instead of silently producing a broken bundle.
#==============================================================================

param(
    [switch]$NoLaunch
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $projectRoot

$sharedLib = Join-Path $projectRoot "build\SpaceDust_artefacts\Release\Space Dust_SharedCode.lib"

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

# Return the (unique) names of any processes that currently have the given DLL
# loaded as a module - i.e. that would lock the file and prevent replacement.
function Get-DllLockers {
    param([string]$DllPath)
    $holders = @()
    foreach ($p in Get-Process) {
        try {
            foreach ($m in $p.Modules) {
                if ($m.FileName -ieq $DllPath) { $holders += $p.ProcessName; break }
            }
        } catch { }   # protected or exited process - ignore
    }
    return ($holders | Sort-Object -Unique)
}

$dllRel = "Contents\x86_64-win\Space Dust.vst3"

# SHA256 of a file, retrying through transient locks. Right after the linker
# writes the DLL, antivirus (Windows Defender) opens it to scan, briefly locking
# it - so a single Get-FileHash can fail even though nothing is wrong. Retry.
function Get-FileHashSafe {
    param([string]$Path, [int]$Retries = 15, [int]$DelayMs = 600)
    for ($i = 0; $i -lt $Retries; $i++) {
        try { return (Get-FileHash -LiteralPath $Path -Algorithm SHA256 -ErrorAction Stop).Hash }
        catch { Start-Sleep -Milliseconds $DelayMs }
    }
    return $null
}

# Mirror a VST3 bundle from $Source to $Dest using robocopy /MIR.
# /MIR replaces changed files AND purges extras, so it also cleans up any
# nested "Space Dust.vst3\Space Dust.vst3" left by an earlier botched copy.
#
# Success is judged by whether the deployed plugin binary matches the build
# (content hash $SrcHash) - NOT by robocopy's exit code. A running DAW's plugin
# scan can briefly lock files and push robocopy's exit code to 8+ even after the
# DLL copied fine, so the exit code alone gives false failures. The hash check
# is the ground truth and also catches genuine "old binary still there" cases.
function Copy-BundleMirror {
    param(
        [string]$Source,
        [string]$Dest,
        [string]$SrcHash,
        [switch]$Elevated
    )
    $destParent = Split-Path -Parent $Dest
    if ($Elevated) {
        $inner = @"
if (-not (Test-Path '$destParent')) { New-Item -ItemType Directory -Force -Path '$destParent' | Out-Null }
robocopy '$Source' '$Dest' /MIR /NJH /NJS /NP /NDL /NFL /R:2 /W:2 | Out-Null
"@
        $enc = [Convert]::ToBase64String([System.Text.Encoding]::Unicode.GetBytes($inner))
        Start-Process -FilePath "powershell.exe" `
            -ArgumentList "-NoProfile","-EncodedCommand",$enc -Verb RunAs -Wait | Out-Null
    } else {
        if (-not (Test-Path $destParent)) { New-Item -ItemType Directory -Force -Path $destParent | Out-Null }
        robocopy $Source $Dest /MIR /NJH /NJS /NP /NDL /NFL /R:2 /W:2 | Out-Null
    }
    $dstDll = Join-Path $Dest $dllRel
    if (-not (Test-Path $dstDll)) { return $false }
    $dstHash = Get-FileHashSafe -Path $dstDll
    return ($dstHash -and ($dstHash -eq $SrcHash))
}

# Build SharedCode first, then the VST3 target. Serializing these two cmake
# invocations avoids the parallel-build race that produces LNK1104.
function Invoke-SpaceDustBuild {
    cmake --build build --config Release --target SpaceDust
    if ($LASTEXITCODE -ne 0) { return $false }
    cmake --build build --config Release --target SpaceDust_VST3
    return ($LASTEXITCODE -eq 0)
}

# ---------------------------------------------------------------------------
# Step 1: Configure (only if needed)
# ---------------------------------------------------------------------------
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

# ---------------------------------------------------------------------------
# Step 2: Build (SharedCode -> VST3, with a stale-lib retry)
# ---------------------------------------------------------------------------
Write-Host "[Space Dust] Building Release VST3 (SharedCode -> VST3)..." -ForegroundColor Cyan
$buildOk = Invoke-SpaceDustBuild
if (-not $buildOk) {
    # Most common cause: a stale or antivirus-locked SharedCode.lib that the
    # linker cannot open (LNK1104). Delete it and retry once.
    Write-Host "[Space Dust] Build failed - clearing stale SharedCode.lib and retrying..." -ForegroundColor Yellow
    if (Test-Path $sharedLib) {
        try {
            Remove-Item -LiteralPath $sharedLib -Force -ErrorAction Stop
        } catch {
            Write-Host "[Space Dust] SharedCode.lib is locked (antivirus scan?). Waiting briefly..." -ForegroundColor Yellow
            Start-Sleep -Seconds 3
            Remove-Item -LiteralPath $sharedLib -Force -ErrorAction SilentlyContinue
        }
    }
    $buildOk = Invoke-SpaceDustBuild
}
if (-not $buildOk) {
    Write-Host "[Space Dust] Build failed." -ForegroundColor Red
    exit 1
}
Write-Host "[Space Dust] Build succeeded." -ForegroundColor Green

# ---------------------------------------------------------------------------
# Step 3: Deploy (robocopy /MIR everywhere; elevate + lock-check Program Files)
# ---------------------------------------------------------------------------
$source = Join-Path $projectRoot "build\SpaceDust_artefacts\Release\VST3\Space Dust.vst3"
if (-not (Test-Path $source)) {
    Write-Host "[Space Dust] VST3 not found at $source" -ForegroundColor Red
    exit 1
}

$programFilesDest = "C:\Program Files\Common Files\VST3\Space Dust.vst3"
$userDests = @(
    "$env:USERPROFILE\Documents\Ableton\User Library\VST3\Space Dust.vst3",
    "$env:USERPROFILE\Documents\VST3\Space Dust.vst3",
    "$env:USERPROFILE\VST3\Space Dust.vst3",
    "$env:APPDATA\VST3\Space Dust.vst3"
)

Write-Host "[Space Dust] Deploying fresh VST3 bundle..." -ForegroundColor Cyan

# Hash the freshly built DLL once (with retry for the post-link AV scan lock),
# then verify every deployed copy against it.
$srcHash = Get-FileHashSafe -Path (Join-Path $source $dllRel)
if (-not $srcHash) {
    Write-Host "[Space Dust] Could not read the freshly built DLL (locked by antivirus?). Aborting deploy." -ForegroundColor Red
    exit 1
}

$deployFailures = 0

# User-writable locations first (no elevation needed).
foreach ($dest in $userDests) {
    if (Copy-BundleMirror -Source $source -Dest $dest -SrcHash $srcHash) {
        Write-Host "  Synced: $dest" -ForegroundColor Green
    } else {
        Write-Host "  FAILED: $dest" -ForegroundColor Red
        $deployFailures++
    }
}

# Program Files needs admin, and a running DAW locks the loaded DLL. Detect the
# lock up front so we never nest the bundle or leave a half-written copy.
$pfDll = Join-Path $programFilesDest "Contents\x86_64-win\Space Dust.vst3"
$lockers = Get-DllLockers -DllPath $pfDll
if ($lockers) {
    Write-Host ""
    Write-Host "[Space Dust] Cannot update Program Files - that DLL is loaded (locked) by:" -ForegroundColor Red
    foreach ($l in $lockers) { Write-Host "    $l" -ForegroundColor Red }
    Write-Host "[Space Dust] Close that DAW completely, then re-run this script." -ForegroundColor Yellow
    Write-Host "[Space Dust] (The user-library copies above were already updated.)" -ForegroundColor Yellow
    exit 1
}

Write-Host "[Space Dust] Updating Program Files (elevated - approve the UAC prompt)..." -ForegroundColor Cyan
if (Copy-BundleMirror -Source $source -Dest $programFilesDest -SrcHash $srcHash -Elevated) {
    Write-Host "  Synced: $programFilesDest" -ForegroundColor Green
} else {
    Write-Host "  FAILED: $programFilesDest (UAC declined, or the binary did not update)" -ForegroundColor Red
    Write-Host "[Space Dust] Re-run and approve the UAC prompt, or close any DAW holding the plugin." -ForegroundColor Yellow
    exit 1
}

if ($deployFailures -eq 0) {
    Write-Host "[Space Dust] All VST3 locations are now in sync." -ForegroundColor Green
} else {
    Write-Host "[Space Dust] Program Files updated, but $deployFailures user-library location(s) did NOT verify." -ForegroundColor Yellow
    Write-Host "[Space Dust] (Usually a DAW had the file locked mid-scan - close it and re-run if that host needs the update.)" -ForegroundColor Yellow
}

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

# Clean success exit (otherwise the script inherits robocopy's success code,
# e.g. 1 = "files copied", which looks like a failure to callers).
exit 0

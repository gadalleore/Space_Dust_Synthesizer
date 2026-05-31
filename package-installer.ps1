#==============================================================================
# Space Dust - Build + package Windows installer
#
# 1. Builds a CLEAN Release VST3 with ALL logging compiled out (unless
#    -SkipBuild). The CMake configure step explicitly forces
#       -DENABLE_MEMORY_SAFETY_LOGGING=OFF
#       -DENABLE_VLD=OFF
#       -DENABLE_ASAN=OFF
#       -DCMAKE_BUILD_TYPE=Release
#    so a previously-configured build/ with safety logging ON can never leak
#    into the shipped binary.
# 2. Stages the .vst3 bundle into installer\Files\VST3\.
# 3. SAFETY-NET: scans the staged VST3 for "MemorySafetyLogger" symbols.
#    If found, the installer is REFUSED - the binary is not safe to ship.
# 4. Syncs factory presets from %USERPROFILE%\Documents\Space Dust\Presets
#    into installer\Files\Presets\ so they ship with the installer.
# 5. Runs ISCC.exe to compile installer\Output\SpaceDust-Synthesizer-1.0-Setup.exe.
#
# Flags:
#   -SkipBuild         Reuse the existing VST3 artifact (faster iteration).
#                      The safety-net scan still runs and will refuse to package
#                      if the artifact contains logger symbols.
#   -SkipPresets       Don't touch installer\Files\Presets\ (keep current set).
#   -PresetsSource P   Override the source preset folder (defaults to the user's
#                      Documents\Space Dust\Presets, matching the installer's
#                      default preset location).
#==============================================================================

param(
    [switch]$SkipBuild,
    [switch]$SkipPresets,
    [string]$PresetsSource
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $projectRoot

# Resolve JUCE_DIR: env var > juce_path.local
$juceDir = $env:JUCE_DIR
if (-not $juceDir -and (Test-Path "juce_path.local")) {
    $juceDir = (Get-Content "juce_path.local" -Raw).Trim()
}

# ── Locate ISCC ───────────────────────────────────────────────────────────
$iscc = $null
foreach ($p in @(
    "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
    "$env:ProgramFiles\Inno Setup 6\ISCC.exe"
)) {
    if (Test-Path $p) { $iscc = $p; break }
}
if (-not $iscc) {
    Write-Host "[Package] ISCC.exe not found." -ForegroundColor Red
    Write-Host "          Install Inno Setup 6 from https://jrsoftware.org/isdl.php" -ForegroundColor Red
    exit 1
}
Write-Host "[Package] ISCC: $iscc" -ForegroundColor Gray

# ── Step 1: Build Release VST3 with ALL logging compiled out ──────────────
# This is the single source of truth for installer builds: we ALWAYS re-run
# CMake configure with logging explicitly OFF so the cache can't drift to a
# logging-enabled state from a prior dev build. The user never has to
# remember to disable logging - packaging here forces it.
if (-not $SkipBuild) {
    $buildDir = "build"
    Write-Host "[Package] Configuring CMake (Release, ALL logging OFF)..." -ForegroundColor Cyan

    $cmakeArgs = @(
        "-B", $buildDir,
        "-DCMAKE_BUILD_TYPE=Release",
        "-DENABLE_MEMORY_SAFETY_LOGGING=OFF",
        "-DENABLE_VLD=OFF",
        "-DENABLE_ASAN=OFF"
    )
    if ($juceDir) { $cmakeArgs += "-DJUCE_DIR=$juceDir" }

    & cmake @cmakeArgs
    if ($LASTEXITCODE -ne 0) {
        Write-Host "[Package] CMake configure failed (exit $LASTEXITCODE)." -ForegroundColor Red
        exit 1
    }

    Write-Host "[Package] Building Release VST3..." -ForegroundColor Cyan
    & cmake --build $buildDir --config Release --target SpaceDust_VST3 --parallel
    if ($LASTEXITCODE -ne 0) {
        Write-Host "[Package] VST3 build failed (exit $LASTEXITCODE)." -ForegroundColor Red
        exit 1
    }
} else {
    Write-Host "[Package] -SkipBuild: reusing existing artifact (safety scan still runs)." -ForegroundColor Gray
}

# ── Step 2: Stage the .vst3 bundle ────────────────────────────────────────
$vstSrc = "build\SpaceDust_artefacts\Release\VST3\Space Dust.vst3"
$vstDst = "installer\Files\VST3\Space Dust.vst3"
if (-not (Test-Path $vstSrc)) {
    Write-Host "[Package] VST3 artifact missing: $vstSrc" -ForegroundColor Red
    Write-Host "          Run without -SkipBuild." -ForegroundColor Red
    exit 1
}
New-Item -ItemType Directory -Force -Path (Split-Path $vstDst) | Out-Null
if (Test-Path $vstDst) { Remove-Item -Recurse -Force $vstDst }
Copy-Item -Recurse -Force $vstSrc $vstDst
Write-Host "[Package] Staged VST3 -> $vstDst" -ForegroundColor Green

# ── Step 2.5: Safety-net scan - refuse to ship a binary with logger code ──
# The binary must not contain the "MemorySafetyLogger" symbol string. If it
# does, ENABLE_MEMORY_SAFETY_LOGGING leaked into a Release build and we abort
# rather than ship logs onto user machines.
Write-Host "[Package] Scanning staged VST3 for logger symbols..." -ForegroundColor Cyan
$stagedDll = Get-ChildItem -Path $vstDst -Recurse -Filter "Space Dust.vst3" -File `
              -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $stagedDll) {
    Write-Host "[Package] Could not find Space Dust.vst3 DLL inside staged bundle." -ForegroundColor Red
    exit 1
}
$bytes  = [System.IO.File]::ReadAllBytes($stagedDll.FullName)
$needle = [System.Text.Encoding]::ASCII.GetBytes("MemorySafetyLogger")
$found  = $false
for ($i = 0; $i -le ($bytes.Length - $needle.Length); $i++) {
    $hit = $true
    for ($j = 0; $j -lt $needle.Length; $j++) {
        if ($bytes[$i+$j] -ne $needle[$j]) { $hit = $false; break }
    }
    if ($hit) { $found = $true; break }
}
if ($found) {
    Write-Host "" -ForegroundColor Red
    Write-Host "[Package] ABORT: 'MemorySafetyLogger' symbol present in staged VST3." -ForegroundColor Red
    Write-Host "          This means ENABLE_MEMORY_SAFETY_LOGGING leaked into the Release build." -ForegroundColor Red
    Write-Host "          Refusing to package - fix the build configuration and retry:" -ForegroundColor Red
    Write-Host "              .\disable-all-logging-for-release.ps1" -ForegroundColor Red
    Write-Host "          DLL: $($stagedDll.FullName)" -ForegroundColor Red
    exit 2
}
Write-Host "[Package] Clean: no logger symbols in shipped binary." -ForegroundColor Green

# ── Step 3: Sync factory presets ──────────────────────────────────────────
if (-not $SkipPresets) {
    if (-not $PresetsSource) {
        $PresetsSource = "$env:USERPROFILE\Documents\Space Dust\Presets"
    }
    $presetDst = "installer\Files\Presets"
    New-Item -ItemType Directory -Force -Path $presetDst | Out-Null

    # Preset file extension must match PresetManager::presetExtension. Keep in sync
    # with Source/PresetManager.h if it ever changes.
    $presetExt = ".sdpreset"

    if (Test-Path $PresetsSource) {
        # Wipe previously staged preset files so renames/deletions in the source
        # are reflected exactly. README.md (this folder's doc) is preserved.
        Get-ChildItem $presetDst -Filter "*$presetExt" -File -ErrorAction SilentlyContinue |
            Remove-Item -Force

        $copied = 0
        Get-ChildItem $PresetsSource -Filter "*$presetExt" -File -ErrorAction SilentlyContinue |
            ForEach-Object {
                Copy-Item $_.FullName -Destination $presetDst -Force
                $copied++
            }
        if ($copied -gt 0) {
            Write-Host "[Package] Synced $copied factory preset(s) from:" -ForegroundColor Green
            Write-Host "          $PresetsSource"
        } else {
            Write-Host "[Package] No $presetExt presets found in $PresetsSource (skipping)." -ForegroundColor Yellow
            Write-Host "          Save some presets from Space Dust first, then re-run." -ForegroundColor Yellow
        }
    } else {
        Write-Host "[Package] Preset source folder not found: $PresetsSource" -ForegroundColor Yellow
        Write-Host "          (Installer will ship with whatever $presetExt files are already in $presetDst.)" -ForegroundColor Yellow
    }
} else {
    Write-Host "[Package] -SkipPresets: leaving installer\Files\Presets\ untouched." -ForegroundColor Gray
}

# ── Step 4: Compile installer ─────────────────────────────────────────────
Write-Host "[Package] Compiling installer..." -ForegroundColor Cyan
& $iscc "installer\SpaceDust-Setup.iss"
if ($LASTEXITCODE -ne 0) {
    Write-Host "[Package] ISCC failed (exit $LASTEXITCODE)." -ForegroundColor Red
    exit 1
}

# ── Step 5: Report ────────────────────────────────────────────────────────
$exe = "installer\Output\SpaceDust-Synthesizer-1.0-Setup.exe"
if (Test-Path $exe) {
    $info = Get-Item $exe
    Write-Host ""
    Write-Host "[Package] Installer ready:" -ForegroundColor Green
    Write-Host ("  Path : {0}" -f $info.FullName)
    Write-Host ("  Size : {0} MB" -f [math]::Round($info.Length / 1MB, 2))
    Write-Host ("  Built: {0}" -f $info.LastWriteTime)
    $stagedPresets = (Get-ChildItem "installer\Files\Presets" -Filter "*.sdpreset" -File -ErrorAction SilentlyContinue | Measure-Object).Count
    Write-Host ("  Factory presets shipped: {0}" -f $stagedPresets)
} else {
    Write-Host "[Package] Installer .exe missing despite successful compile." -ForegroundColor Red
    exit 1
}

#==============================================================================
# Space Dust - Build + package Windows installer
#
# 1. Builds the Release VST3 (unless -SkipBuild).
# 2. Stages the .vst3 bundle into installer\Files\VST3\.
# 3. Syncs your factory presets from %USERPROFILE%\Documents\Space Dust\Presets
#    into installer\Files\Presets\ so they ship with the installer.
# 4. Runs ISCC.exe to compile installer\Output\SpaceDust-Synthesizer-1.0-Setup.exe.
#
# Flags:
#   -SkipBuild         Reuse the existing VST3 artifact (faster iteration).
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

# ── Step 1: Build Release VST3 ────────────────────────────────────────────
if (-not $SkipBuild) {
    Write-Host "[Package] Building Release VST3..." -ForegroundColor Cyan
    & .\build-and-launch.ps1 -NoLaunch
    if ($LASTEXITCODE -ne 0) {
        Write-Host "[Package] VST3 build failed (exit $LASTEXITCODE)." -ForegroundColor Red
        exit 1
    }
} else {
    Write-Host "[Package] -SkipBuild: reusing existing artifact." -ForegroundColor Gray
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
    $stagedPresets = (Get-ChildItem "installer\Files\Presets" -Filter "*.xml" -File -ErrorAction SilentlyContinue | Measure-Object).Count
    Write-Host ("  Factory presets shipped: {0}" -f $stagedPresets)
} else {
    Write-Host "[Package] Installer .exe missing despite successful compile." -ForegroundColor Red
    exit 1
}

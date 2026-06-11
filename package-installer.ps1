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
    [string]$PresetsSource,
    # -- Code signing (Azure Artifact Signing / Trusted Signing) -----------
    [switch]$Sign,
    [string]$SignToolPath,
    [string]$DlibPath,
    [string]$SigningMetadata = "installer\trusted-signing.json"
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

# -- Resolve code-signing tooling (only when -Sign) ------------------------
# When -Sign is passed we sign the staged VST3 binary AND the final installer
# .exe with signtool + the Azure CodeSigning dlib. Auth comes from your Azure
# login ('az login') or AZURE_TENANT_ID/AZURE_CLIENT_ID/AZURE_CLIENT_SECRET
# env vars. Resolve everything up front so we fail fast BEFORE a long build.
# Every signature is timestamped (/tr) so it stays valid after the short-lived
# Trusted Signing cert rotates (~3 days).
$signtool = $null
$dlib     = $null
$meta     = $null
if ($Sign) {
    # signtool.exe: explicit path > newest installed Windows SDK.
    if ($SignToolPath -and (Test-Path $SignToolPath)) {
        $signtool = $SignToolPath
    } else {
        $signtool = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin\*\x64\signtool.exe" `
                        -ErrorAction SilentlyContinue |
                    Sort-Object FullName -Descending |
                    Select-Object -First 1 -ExpandProperty FullName
    }
    if (-not $signtool) {
        Write-Host "[Sign] signtool.exe not found. Install the Windows SDK, or pass -SignToolPath." -ForegroundColor Red
        exit 1
    }

    # Azure CodeSigning dlib: explicit path > env > default install locations.
    if ($DlibPath -and (Test-Path $DlibPath)) {
        $dlib = $DlibPath
    } else {
        foreach ($d in @(
            $env:DLIB_PATH,
            # winget Microsoft.Azure.TrustedSigningClientTools installs HERE (per-user):
            "$env:LOCALAPPDATA\Microsoft\MicrosoftTrustedSigningClientTools\Azure.CodeSigning.Dlib.dll",
            "$env:ProgramFiles\Microsoft\Azure.CodeSigning.Dlib\Azure.CodeSigning.Dlib.dll",
            "${env:ProgramFiles(x86)}\Microsoft\Azure.CodeSigning.Dlib\Azure.CodeSigning.Dlib.dll"
        )) {
            if ($d -and (Test-Path $d)) { $dlib = $d; break }
        }
        if (-not $dlib) {
            foreach ($root in @("$env:LOCALAPPDATA\Microsoft", "$env:ProgramFiles", "${env:ProgramFiles(x86)}")) {
                if (Test-Path $root) {
                    $hit = Get-ChildItem $root -Recurse -Filter "Azure.CodeSigning.Dlib.dll" `
                               -ErrorAction SilentlyContinue |
                           Select-Object -First 1 -ExpandProperty FullName
                    if ($hit) { $dlib = $hit; break }
                }
            }
        }
    }
    if (-not $dlib) {
        Write-Host "[Sign] Azure.CodeSigning.Dlib.dll not found." -ForegroundColor Red
        Write-Host "       Install it:  winget install -e --id Microsoft.Azure.TrustedSigningClientTools" -ForegroundColor Red
        Write-Host "       or pass -DlibPath C:\path\to\Azure.CodeSigning.Dlib.dll" -ForegroundColor Red
        exit 1
    }

    # Metadata (account endpoint / name / certificate profile). No secrets.
    $meta = $SigningMetadata
    if (-not (Test-Path $meta)) {
        Write-Host "[Sign] Signing metadata not found: $meta" -ForegroundColor Red
        Write-Host "       Copy installer\trusted-signing.json.example to installer\trusted-signing.json" -ForegroundColor Red
        Write-Host "       and fill in your Endpoint / CodeSigningAccountName / CertificateProfileName." -ForegroundColor Red
        exit 1
    }

    Write-Host "[Sign] signtool : $signtool" -ForegroundColor Gray
    Write-Host "[Sign] dlib     : $dlib"     -ForegroundColor Gray
    Write-Host "[Sign] metadata : $meta"     -ForegroundColor Gray
}

# Sign a single file with signtool + the Azure dlib. Reads $signtool/$dlib/$meta
# from script scope (resolved above). Aborts the whole package on any failure -
# we never ship a half-signed installer.
function Invoke-CodeSign {
    param([Parameter(Mandatory)][string]$Path)
    Write-Host "[Sign] Signing $Path" -ForegroundColor Cyan
    & $signtool sign /v /fd SHA256 `
        /tr "http://timestamp.acs.microsoft.com" /td SHA256 `
        /dlib $dlib /dmdf $meta `
        $Path
    if ($LASTEXITCODE -ne 0) {
        Write-Host "[Sign] signtool failed for $Path (exit $LASTEXITCODE)." -ForegroundColor Red
        exit 1
    }
    Write-Host "[Sign] Signed OK: $Path" -ForegroundColor Green
}

# Returns $true if the file contains the ASCII string "MemorySafetyLogger" -
# i.e. a logging-enabled build leaked into Release. Used to refuse shipping it.
function Test-LoggerSymbol {
    param([Parameter(Mandatory)][string]$Path)
    $bytes  = [System.IO.File]::ReadAllBytes($Path)
    $needle = [System.Text.Encoding]::ASCII.GetBytes("MemorySafetyLogger")
    for ($i = 0; $i -le ($bytes.Length - $needle.Length); $i++) {
        $hit = $true
        for ($j = 0; $j -lt $needle.Length; $j++) {
            if ($bytes[$i+$j] -ne $needle[$j]) { $hit = $false; break }
        }
        if ($hit) { return $true }
    }
    return $false
}

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

    Write-Host "[Package] Building Release VST3 + Standalone..." -ForegroundColor Cyan
    & cmake --build $buildDir --config Release --target SpaceDust_VST3 SpaceDust_Standalone --parallel
    if ($LASTEXITCODE -ne 0) {
        Write-Host "[Package] Build failed (exit $LASTEXITCODE)." -ForegroundColor Red
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

# ── Step 2.6: Sign the staged VST3 binary (before it's packaged) ──────────
# Signs the inner PE that the safety scan located, so the plugin DLL that DAWs
# load is itself trusted - not just the installer wrapping it.
if ($Sign) {
    Invoke-CodeSign -Path $stagedDll.FullName
}

# ── Step 2.7: Stage, scan, and sign the standalone .exe ───────────────────
# The standalone app links the same SharedCode as the VST3, so it gets the same
# logger safety-net scan and the same Azure signature before packaging.
$saSrc = "build\SpaceDust_artefacts\Release\Standalone\Space Dust.exe"
$saDst = "installer\Files\Standalone\Space Dust.exe"
if (-not (Test-Path $saSrc)) {
    Write-Host "[Package] Standalone artifact missing: $saSrc" -ForegroundColor Red
    Write-Host "          Run without -SkipBuild." -ForegroundColor Red
    exit 1
}
New-Item -ItemType Directory -Force -Path (Split-Path $saDst) | Out-Null
Copy-Item -Force $saSrc $saDst
Write-Host "[Package] Staged Standalone -> $saDst" -ForegroundColor Green

Write-Host "[Package] Scanning staged Standalone for logger symbols..." -ForegroundColor Cyan
if (Test-LoggerSymbol $saDst) {
    Write-Host "[Package] ABORT: 'MemorySafetyLogger' symbol present in staged Standalone." -ForegroundColor Red
    Write-Host "          Logging leaked into the Release build. Refusing to package." -ForegroundColor Red
    exit 2
}
Write-Host "[Package] Clean: no logger symbols in standalone binary." -ForegroundColor Green

if ($Sign) {
    Invoke-CodeSign -Path $saDst
}

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
# When signing, hand ISCC a "spacedust" sign tool (matching the SignTool
# directive guarded by #ifdef SIGN in the .iss). Inno then signs the Setup .exe
# AND the embedded uninstaller on the fly, so unins000.exe is trusted instead of
# showing "unknown publisher". $q expands to a literal quote inside ISCC, which
# sidesteps PowerShell/ISCC quoting of the space-bearing tool/dlib/meta paths;
# $f is auto-quoted by Inno. The same /tr timestamp keeps it valid past cert
# rotation, mirroring Invoke-CodeSign.
$isccArgs = @()
if ($Sign) {
    $metaAbs  = (Resolve-Path $meta).Path
    $signCmd  = '/Sspacedust=$q' + $signtool + '$q sign /v /fd SHA256 ' +
                '/tr http://timestamp.acs.microsoft.com /td SHA256 ' +
                '/dlib $q' + $dlib + '$q /dmdf $q' + $metaAbs + '$q $f'
    $isccArgs += "/DSIGN"
    $isccArgs += $signCmd
}
$isccArgs += "installer\SpaceDust-Setup.iss"

Write-Host "[Package] Compiling installer..." -ForegroundColor Cyan
& $iscc @isccArgs
if ($LASTEXITCODE -ne 0) {
    Write-Host "[Package] ISCC failed (exit $LASTEXITCODE)." -ForegroundColor Red
    exit 1
}

# ── Step 5: Report ────────────────────────────────────────────────────────
$exe = "installer\Output\SpaceDust-Synthesizer-1.0-Setup.exe"
if (Test-Path $exe) {
    # Sign the installer the user downloads from Gumroad - this is the binary
    # SmartScreen judges, so it matters most. (VST3 inside was already signed.)
    if ($Sign) {
        Invoke-CodeSign -Path $exe
    }
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

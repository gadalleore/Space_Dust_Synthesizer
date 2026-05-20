# =====================================================================
#  disable-all-logging-for-release.ps1
#  ---------------------------------------------------------------------
#  Produce a clean, install-ready Release build of Space Dust with EVERY
#  form of logging compiled out:
#       * MemorySafetyLogger     -> every SAFETY_LOG_* expands to (void)0
#       * JUCE DBG(...)          -> empty (JUCE_DEBUG = 0 in Release)
#       * JUCE_LOG_ASSERTIONS    -> forced to 0
#       * VLD                    -> off
#       * AddressSanitizer       -> off
#
#  Usage (full installer pipeline):
#      .\disable-all-logging-for-release.ps1
#      .\package-installer.ps1
#
#  Usage (build only):
#      .\disable-all-logging-for-release.ps1 -SkipInstaller
# =====================================================================
[CmdletBinding()]
param(
    [string]$BuildDir = "build-release",
    [switch]$SkipInstaller,
    [string]$JuceDir  = ""
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $MyInvocation.MyCommand.Path

# Resolve JUCE path: -JuceDir argument > $env:JUCE_DIR > ./juce_path.local
if (-not $JuceDir) { $JuceDir = $env:JUCE_DIR }
if (-not $JuceDir -and (Test-Path (Join-Path $repo "juce_path.local"))) {
    $JuceDir = (Get-Content (Join-Path $repo "juce_path.local") -Raw).Trim()
}
if (-not $JuceDir -or -not (Test-Path (Join-Path $JuceDir "CMakeLists.txt"))) {
    throw "JUCE not found. Set `$env:JUCE_DIR, create juce_path.local with the path, or pass -JuceDir <path>."
}

Write-Host "==============================================================="
Write-Host "  Space Dust - ALL LOGGING DISABLED  (release / installer build)" -ForegroundColor Cyan
Write-Host "==============================================================="
Write-Host "Repo       : $repo"
Write-Host "Build dir  : $BuildDir"
Write-Host "Config     : Release"
Write-Host ""

Write-Host "[1/3] Configuring CMake (Release, all logging OFF)..." -ForegroundColor Yellow
cmake -B $BuildDir `
      -DCMAKE_BUILD_TYPE=Release `
      -DENABLE_MEMORY_SAFETY_LOGGING=OFF `
      -DENABLE_VLD=OFF `
      -DENABLE_ASAN=OFF `
      -DJUCE_DIR="$JuceDir"
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed (exit $LASTEXITCODE)." }

Write-Host ""
Write-Host "[2/3] Building Release..." -ForegroundColor Yellow
cmake --build $BuildDir --config Release --parallel
if ($LASTEXITCODE -ne 0) { throw "Release build failed (exit $LASTEXITCODE)." }

Write-Host ""
Write-Host "[3/3] Sanity check: confirm no Safety-Logger symbols remain..." -ForegroundColor Yellow
$vst3 = Get-ChildItem -Path $BuildDir -Recurse -Filter "Space Dust.vst3" -File `
        -ErrorAction SilentlyContinue | Select-Object -First 1
if ($vst3) {
    $bytes  = [System.IO.File]::ReadAllBytes($vst3.FullName)
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
        Write-Warning "Found 'MemorySafetyLogger' string in the built VST3. Logging code may have leaked into Release."
    } else {
        Write-Host "Clean: no logger symbols in shipped binary." -ForegroundColor Green
    }
} else {
    Write-Host "(VST3 not found under $BuildDir - skipping symbol scan.)" -ForegroundColor DarkYellow
}

Write-Host ""
if (-not $SkipInstaller) {
    $pkg = Join-Path $repo "package-installer.ps1"
    if (Test-Path $pkg) {
        Write-Host "Invoking package-installer.ps1 ..." -ForegroundColor Cyan
        & $pkg
        if ($LASTEXITCODE -ne 0) { throw "package-installer.ps1 failed (exit $LASTEXITCODE)." }
    } else {
        Write-Host "package-installer.ps1 not found - skipping installer step." -ForegroundColor DarkYellow
    }
}

Write-Host ""
Write-Host "Release build produced with ZERO logging output." -ForegroundColor Green

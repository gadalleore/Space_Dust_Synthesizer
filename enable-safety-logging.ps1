# =====================================================================
#  enable-safety-logging.ps1
#  ---------------------------------------------------------------------
#  One-line way to build Space Dust with the Memory Safety Logger ON.
#
#  Usage:
#      .\enable-safety-logging.ps1                 # Debug build, build-safety/
#      .\enable-safety-logging.ps1 -Config RelWithDebInfo
#      .\enable-safety-logging.ps1 -BuildDir build-mine
#
#  After build, logs land in:
#      %APPDATA%\Shades\Space Dust\Logs\Safety\
#  with the form SpaceDust_Safety_<yyyy-MM-dd_HH-mm-ss>_PID<n>.log
# =====================================================================
[CmdletBinding()]
param(
    [string]$BuildDir = "build-safety",
    [ValidateSet("Debug","RelWithDebInfo","Release","MinSizeRel")]
    [string]$Config   = "Debug",
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
Write-Host "  Space Dust - Memory Safety Logging  (ENABLED)" -ForegroundColor Cyan
Write-Host "==============================================================="
Write-Host "Repo       : $repo"
Write-Host "Build dir  : $BuildDir"
Write-Host "Config     : $Config"
Write-Host ""

Write-Host "[1/3] Configuring CMake..." -ForegroundColor Yellow
cmake -B $BuildDir `
      -DENABLE_MEMORY_SAFETY_LOGGING=ON `
      -DCMAKE_BUILD_TYPE=$Config `
      -DJUCE_DIR="$JuceDir"
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed (exit $LASTEXITCODE)." }

Write-Host ""
Write-Host "[2/3] Building Space Dust ($Config)..." -ForegroundColor Yellow
cmake --build $BuildDir --config $Config --parallel
if ($LASTEXITCODE -ne 0) { throw "Build failed (exit $LASTEXITCODE)." }

$logDir = Join-Path $env:APPDATA "Shades\Space Dust\Logs\Safety"
if (-not (Test-Path $logDir)) { New-Item -ItemType Directory -Force -Path $logDir | Out-Null }

Write-Host ""
Write-Host "[3/3] Done." -ForegroundColor Green
Write-Host ""
Write-Host "Safety logging is ACTIVE in the built plugin."
Write-Host "Logs will appear at:"
Write-Host "    $logDir" -ForegroundColor Green
Write-Host ""
Write-Host "Tail the newest log live with:"
$tailCmd = 'Get-ChildItem "' + $logDir + '" *.log | Sort-Object LastWriteTime -Desc | Select-Object -First 1 | Get-Content -Wait'
Write-Host "    $tailCmd" -ForegroundColor Gray
Write-Host ""
Write-Host "(Logs older than 60 days are auto-deleted on plugin startup.)"

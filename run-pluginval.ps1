# run-pluginval.ps1
# Builds Space Dust in Release if needed, then runs Tracktion pluginval against
# the resulting VST3 with strong validation settings.

[CmdletBinding()]
param(
    [string]$BuildDir   = (Join-Path $PSScriptRoot 'build'),
    [int]   $Strictness = 10,            # 1-10; 10 = maximum
    [int]   $Repeat     = 3,             # how many validation runs
    [int]   $Timeout    = 600,           # seconds before pluginval aborts
    [int]   $SampleRates,                # optional: pass-through; pluginval picks defaults
    [switch]$NoBuild,
    [switch]$SkipGuiTests                # skip editor/GUI tests (used in headless CI)
)

$ErrorActionPreference = 'Stop'
$root        = $PSScriptRoot
$pluginval   = Join-Path $root 'tools/pluginval/pluginval.exe'
$vst3Bundle  = Join-Path $BuildDir 'SpaceDust_artefacts/Release/VST3/Space Dust.vst3'

function Write-Section($title) {
    Write-Host ''
    Write-Host ('=' * 70) -ForegroundColor DarkCyan
    Write-Host " $title" -ForegroundColor Cyan
    Write-Host ('=' * 70) -ForegroundColor DarkCyan
}

Write-Section 'pluginval validation - Space Dust'

# 1) Ensure pluginval is downloaded
if (-not (Test-Path $pluginval)) {
    Write-Host 'pluginval not found; running download script...' -ForegroundColor Yellow
    & (Join-Path $root 'download-pluginval.ps1')
    if (-not (Test-Path $pluginval)) { throw 'pluginval still not present after download.' }
}

# 2) Build Release VST3 if missing or out of date
if (-not $NoBuild) {
    Write-Host 'Building Release VST3 (serial -j 1 to avoid SharedCode race)...' -ForegroundColor Cyan
    & cmake --build $BuildDir --config Release -j 1
    if ($LASTEXITCODE -ne 0) { throw "Build failed with exit $LASTEXITCODE" }
}

if (-not (Test-Path $vst3Bundle)) {
    throw "VST3 bundle not found at: $vst3Bundle"
}

# 3) Pluginval invocation. --strictness-level 10 enables the most aggressive
#    tests, including repeated open/close, parameter abuse, threading checks,
#    and editor lifecycle stress.
Write-Section "Running pluginval (strictness=$Strictness, repeat=$Repeat)"
$args = @(
    '--strictness-level', $Strictness,
    '--validate-in-process',
    '--repeat',            $Repeat,
    '--timeout-ms',        ($Timeout * 1000),
    '--randomise',
    '--verbose'
)
# Editor/GUI tests ("open editor whilst processing") deadlock on a headless CI
# runner and hang until the job timeout. Skip them there; run them locally.
if ($SkipGuiTests) { $args += '--skip-gui-tests' }
$args += "`"$vst3Bundle`""

$logPath = Join-Path $BuildDir 'pluginval-report.log'
& $pluginval @args 2>&1 | Tee-Object -FilePath $logPath
$code = $LASTEXITCODE

Write-Section 'Result'
if ($code -eq 0) {
    Write-Host "PASS  pluginval reported no failures (log: $logPath)" -ForegroundColor Green
} else {
    Write-Host "FAIL  pluginval exit code $code  (log: $logPath)" -ForegroundColor Red
    Write-Host 'Inspect the log for the failing test name and stack.' -ForegroundColor Yellow
}
exit $code

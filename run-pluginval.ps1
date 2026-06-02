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
    [switch]$SkipGuiTests,               # skip editor/GUI tests (used in headless CI)
    [switch]$OutOfProcess                # validate in a child process (recommended for CI:
                                         # --timeout-ms can then kill a hung test instead of
                                         # an in-process deadlock hanging the whole run)
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
    '--repeat',            $Repeat,
    '--timeout-ms',        ($Timeout * 1000),
    '--randomise',
    '--verbose'
)
# In-process is convenient for local debugging, but an in-process deadlock hangs
# the whole run with no way for --timeout-ms to interrupt it (seen on the headless
# CI runner). Default to in-process locally; CI passes -OutOfProcess.
if (-not $OutOfProcess) { $args += '--validate-in-process' }
# Editor/GUI tests ("open editor whilst processing") deadlock on a headless CI
# runner and hang until the job timeout. Skip them there; run them locally.
if ($SkipGuiTests) { $args += '--skip-gui-tests' }
$args += "`"$vst3Bundle`""

# Run via Start-Process, NOT a PowerShell pipeline. The old `& $pluginval 2>&1 |
# Tee-Object` deadlocked on the headless CI runner (PS 5.1 wrapping a native
# exe's stderr + piping a GUI-subsystem process) and hung to the step timeout.
# A bare `& $pluginval` doesn't wait for a GUI-subsystem exe (false instant pass).
# Start-Process -Wait waits reliably and redirects output to files (no pipeline),
# so there's no deadlock and we still capture the full log.
$logPath = Join-Path $BuildDir 'pluginval-report.log'
$errPath = Join-Path $BuildDir 'pluginval-stderr.log'
$argLine = ($args -join ' ')
$proc = Start-Process -FilePath $pluginval -ArgumentList $argLine -Wait -NoNewWindow -PassThru `
                      -RedirectStandardOutput $logPath -RedirectStandardError $errPath
$code = $proc.ExitCode
if (Test-Path $logPath) { Get-Content $logPath }
if ((Test-Path $errPath) -and (Get-Item $errPath).Length -gt 0) { Write-Host '--- stderr ---'; Get-Content $errPath }

Write-Section 'Result'
if ($code -eq 0) {
    Write-Host 'PASS  pluginval reported no failures' -ForegroundColor Green
} else {
    Write-Host "FAIL  pluginval exit code $code" -ForegroundColor Red
    Write-Host 'Inspect the output above for the failing/last test name.' -ForegroundColor Yellow
}
exit $code

# qa-runtime-monitor.ps1
# Launches the Space Dust Standalone build, samples process memory + handle/thread
# counts at a fixed interval, and reports growth trends. Designed to catch
# runtime memory leaks (idle audio callbacks, UI timers, etc.) without
# needing VLD or ASan attached.

[CmdletBinding()]
param(
    [string]$ExePath  = (Join-Path $PSScriptRoot 'build/SpaceDust_artefacts/Release/Standalone/Space Dust.exe'),
    [int]   $DurationSec = 60,
    [int]   $IntervalSec = 2,
    [string]$LogFile  = (Join-Path $PSScriptRoot 'build/runtime-monitor.csv'),
    # Attach mode: monitor an already-running process by name instead of launching the standalone.
    # Example: -AttachByName 'Ableton Live 10 Suite'  (no .exe suffix)
    [string]$AttachByName
)

$ErrorActionPreference = 'Stop'

Write-Host ''
Write-Host ('=' * 70) -ForegroundColor DarkCyan
Write-Host " Space Dust runtime memory monitor"  -ForegroundColor Cyan

if ($AttachByName) {
    Write-Host " Mode:     ATTACH to existing process" -ForegroundColor Gray
    Write-Host " Target:   $AttachByName"              -ForegroundColor Gray
    Write-Host " Duration: $DurationSec sec at $IntervalSec sec intervals" -ForegroundColor Gray
    Write-Host " Log:      $LogFile"                   -ForegroundColor Gray
    Write-Host ('=' * 70) -ForegroundColor DarkCyan

    $candidates = Get-Process -Name $AttachByName -ErrorAction SilentlyContinue
    if (-not $candidates) {
        throw "No running process named '$AttachByName'. Launch the host first, then re-run."
    }
    $proc = $candidates | Sort-Object StartTime -Descending | Select-Object -First 1
    Write-Host ("Attached to PID {0}" -f $proc.Id) -ForegroundColor Green
} else {
    Write-Host " Mode:     LAUNCH standalone" -ForegroundColor Gray
    Write-Host " Exe:      $ExePath"          -ForegroundColor Gray
    Write-Host " Duration: $DurationSec sec at $IntervalSec sec intervals" -ForegroundColor Gray
    Write-Host " Log:      $LogFile"          -ForegroundColor Gray
    Write-Host ('=' * 70) -ForegroundColor DarkCyan

    if (-not (Test-Path $ExePath)) {
        throw "Standalone exe not found at: $ExePath`nBuild with: cmake --build build --config Release --target SpaceDust_Standalone -j 1"
    }
    $proc = Start-Process -FilePath $ExePath -PassThru
    Start-Sleep -Seconds 2  # let the audio device + UI initialize
    $proc.Refresh()
    if ($proc.HasExited) { throw 'Standalone exited immediately after launch.' }
    Write-Host ("Launched PID {0}" -f $proc.Id) -ForegroundColor Green
}

# Header
"timestamp,elapsed_sec,working_set_mb,private_mb,handles,threads,gdi_objs" |
    Out-File $LogFile -Encoding utf8

$samples  = [System.Collections.Generic.List[object]]::new()
$startUtc = Get-Date
$endUtc   = $startUtc.AddSeconds($DurationSec)

Write-Host ''
Write-Host ("{0,-10} {1,12} {2,12} {3,8} {4,8} {5,8}" -f 'Elapsed', 'WorkingSet', 'Private', 'Handles', 'Threads', 'GDI') -ForegroundColor White
Write-Host ('-' * 70) -ForegroundColor DarkGray

while ((Get-Date) -lt $endUtc) {
    try { $proc.Refresh() } catch { break }
    if ($proc.HasExited) {
        Write-Host 'Process exited early.' -ForegroundColor Yellow
        break
    }

    $elapsed = [int]((Get-Date) - $startUtc).TotalSeconds
    $wsMb    = [Math]::Round($proc.WorkingSet64    / 1MB, 2)
    $prMb    = [Math]::Round($proc.PrivateMemorySize64 / 1MB, 2)
    $handles = $proc.HandleCount
    $threads = $proc.Threads.Count

    # GDI objects (best-effort; requires User32 P/Invoke)
    $gdi = '-'
    try {
        Add-Type -Name GdiCount -Namespace QA -ErrorAction SilentlyContinue -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern int GetGuiResources(System.IntPtr hProcess, int uiFlags);
'@
        $gdi = [QA.GdiCount]::GetGuiResources($proc.Handle, 0)  # 0 = GDI
    } catch {}

    $row = "{0},{1},{2},{3},{4},{5},{6}" -f (Get-Date -Format o), $elapsed, $wsMb, $prMb, $handles, $threads, $gdi
    Add-Content -Path $LogFile -Value $row

    $samples.Add([pscustomobject]@{ Elapsed = $elapsed; WS = $wsMb; PR = $prMb; H = $handles; T = $threads; GDI = $gdi }) | Out-Null

    Write-Host ("{0,6} s    {1,10} MB {2,10} MB {3,8} {4,8} {5,8}" -f $elapsed, $wsMb, $prMb, $handles, $threads, $gdi) -ForegroundColor Gray
    Start-Sleep -Seconds $IntervalSec
}

# Shut down cleanly only when we launched the process. In attach mode the host
# is the user's choice (Ableton, FL) and we never close it.
if (-not $AttachByName) {
    if (-not $proc.HasExited) {
        Write-Host ''
        Write-Host 'Closing Standalone...' -ForegroundColor Cyan
        $proc.CloseMainWindow() | Out-Null
        if (-not $proc.WaitForExit(3000)) {
            Write-Host 'Force-killing process (CloseMainWindow timed out)' -ForegroundColor Yellow
            Stop-Process -Id $proc.Id -Force
        }
    }
}

# Analysis
Write-Host ''
Write-Host ('=' * 70) -ForegroundColor DarkCyan
Write-Host ' Trend analysis'                       -ForegroundColor Cyan
Write-Host ('=' * 70) -ForegroundColor DarkCyan

if ($samples.Count -lt 5) {
    Write-Host 'Not enough samples to analyze.' -ForegroundColor Yellow
    exit 1
}

# Use samples from after the warmup window (skip first 8 sec to let JUCE init settle)
$warmup = $samples | Where-Object { $_.Elapsed -ge 8 }
if ($warmup.Count -lt 3) { $warmup = $samples }

$wsStart = ($warmup | Select-Object -First 1).WS
$wsEnd   = ($warmup | Select-Object -Last  1).WS
$wsMin   = ($warmup | Measure-Object -Property WS -Minimum).Minimum
$wsMax   = ($warmup | Measure-Object -Property WS -Maximum).Maximum
$wsDelta = [Math]::Round($wsEnd - $wsStart, 2)

$prStart = ($warmup | Select-Object -First 1).PR
$prEnd   = ($warmup | Select-Object -Last  1).PR
$prDelta = [Math]::Round($prEnd - $prStart, 2)

$hStart = ($warmup | Select-Object -First 1).H
$hEnd   = ($warmup | Select-Object -Last  1).H
$hDelta = $hEnd - $hStart

$dur    = ($warmup | Select-Object -Last 1).Elapsed - ($warmup | Select-Object -First 1).Elapsed
if ($dur -le 0) { $dur = 1 }
$wsRate = [Math]::Round(($wsDelta * 60.0) / $dur, 2)  # MB/min, post-warmup
$prRate = [Math]::Round(($prDelta * 60.0) / $dur, 2)

# Also compute a "tail rate" using only the last third of samples — this filters
# out step-changes from voice allocation when notes start mid-run.
$tailStartIdx = [Math]::Max(0, [Math]::Floor($warmup.Count * 2 / 3))
$tail = $warmup | Select-Object -Skip $tailStartIdx
if ($tail.Count -ge 3) {
    $tailDur   = ($tail | Select-Object -Last 1).Elapsed - ($tail | Select-Object -First 1).Elapsed
    if ($tailDur -le 0) { $tailDur = 1 }
    $wsRateTail = [Math]::Round(((($tail | Select-Object -Last 1).WS) - (($tail | Select-Object -First 1).WS)) * 60.0 / $tailDur, 2)
    $prRateTail = [Math]::Round(((($tail | Select-Object -Last 1).PR) - (($tail | Select-Object -First 1).PR)) * 60.0 / $tailDur, 2)
} else {
    $wsRateTail = $wsRate
    $prRateTail = $prRate
}

Write-Host ("Working-Set:  start {0,8:F2} MB   end {1,8:F2} MB   delta {2,8:F2} MB   range [{3:F2}, {4:F2}]" -f $wsStart, $wsEnd, $wsDelta, $wsMin, $wsMax)
Write-Host ("Private:      start {0,8:F2} MB   end {1,8:F2} MB   delta {2,8:F2} MB" -f $prStart, $prEnd, $prDelta)
Write-Host ("Handles:      start {0,8} -> end {1,8}   delta {2}" -f $hStart, $hEnd, $hDelta)
Write-Host ("Post-warmup growth rate: WS {0} MB/min,   Private {1} MB/min" -f $wsRate, $prRate) -ForegroundColor White
Write-Host ("Tail rate (last third): WS {0} MB/min,   Private {1} MB/min  <-- best leak signal" -f $wsRateTail, $prRateTail) -ForegroundColor White

# Verdict — uses tail rate so one-time voice-allocation step-changes don't trip the threshold
$verdict = 'PASS'
$reasons = @()
if ($prRateTail -gt 5.0) { $verdict = 'FAIL'; $reasons += "Private tail rate ${prRateTail} MB/min (>5 MB/min threshold)" }
if ($hDelta -gt 50 -and $hEnd -gt $hStart) { $verdict = 'FAIL'; $reasons += "Handle count grew by $hDelta (>50 threshold)" }
if ($wsRateTail -gt 10.0 -and $prRateTail -gt 2.0) { $verdict = 'FAIL'; $reasons += "Working set + private both trending up in tail window" }

Write-Host ''
if ($verdict -eq 'PASS') {
    Write-Host '  PASS - no runtime memory leak detected (idle workload)' -ForegroundColor Green
    Write-Host '  Note: this exercises the idle audio callback path. To stress-test voices,' -ForegroundColor Gray
    Write-Host '  trigger notes via the on-screen keyboard while monitoring.' -ForegroundColor Gray
    exit 0
} else {
    Write-Host "  FAIL - $verdict" -ForegroundColor Red
    $reasons | ForEach-Object { Write-Host "  - $_" -ForegroundColor Red }
    Write-Host "  Full sample log: $LogFile" -ForegroundColor Yellow
    exit 1
}

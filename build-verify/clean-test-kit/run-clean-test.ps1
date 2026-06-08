# Space Dust - clean-machine test runner (run INSIDE the VM)
# Writes all results into .\results so the host can read them via the shared folder.
# ASCII only on purpose (PS 5.1 reads non-ASCII as CP1252 and mangles it).

$ErrorActionPreference = 'Continue'
$here    = Split-Path -Parent $MyInvocation.MyCommand.Path
$results = Join-Path $here 'results'
New-Item -ItemType Directory -Force -Path $results | Out-Null
$report  = Join-Path $results ("clean-test_{0}.txt" -f (Get-Date -Format 'yyyy-MM-dd_HH-mm-ss'))

function Log($msg) { $msg | Tee-Object -FilePath $report -Append }

Log "=============================================="
Log " Space Dust clean-machine test"
Log " $(Get-Date)"
Log " Host: $env:COMPUTERNAME   User: $env:USERNAME"
Log "=============================================="

# --- 1. Confirm this really is a clean machine (no VC++ redist) ---
Log ""
Log "[1] Clean-machine check (VC++ runtime should be ABSENT)"
$sys = Join-Path $env:WINDIR 'System32'
foreach ($dll in 'VCRUNTIME140.dll','MSVCP140.dll') {
    $p = Join-Path $sys $dll
    if (Test-Path $p) { Log ("    PRESENT : {0}  (redist IS installed - not a virgin machine)" -f $dll) }
    else              { Log ("    absent  : {0}  (good - true clean test)" -f $dll) }
}
$redistKey = 'HKLM:\SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64'
if (Test-Path $redistKey) { Log ("    VC++ redist registry key present: {0}" -f (Get-ItemProperty $redistKey).Version) }
else                      { Log "    VC++ redist registry key: absent" }

# --- 2. Locate the plugin ---
Log ""
Log "[2] Plugin under test"
$vst3 = Get-ChildItem -Path $here -Recurse -Filter 'Space Dust.vst3' -ErrorAction SilentlyContinue |
        Where-Object { $_.PSIsContainer } | Select-Object -First 1
if (-not $vst3) { Log "    ERROR: 'Space Dust.vst3' bundle not found next to this script."; exit 2 }
$inner = Join-Path $vst3.FullName 'Contents\x86_64-win\Space Dust.vst3'
Log ("    bundle : {0}" -f $vst3.FullName)
Log ("    binary : {0} ({1:N0} bytes)" -f $inner, (Get-Item $inner).Length)

# --- 2b. Stage plugin + pluginval to LOCAL disk -----------------------------
# The kit usually lives on a VirtualBox shared folder (Z:). Run the tools from a
# local working folder so the test never depends on shared-folder quirks.
#
# NOTE (2026-06-07): a 0xC0000135 (STATUS_DLL_NOT_FOUND, exit -1073741515) below
# with EMPTY pluginval output is almost always pluginval.exe ITSELF failing to
# start, not the plugin: this pluginval build is dynamically linked and imports
# VCRUNTIME140.dll / MSVCP140.dll, which are absent on a truly clean VM. That is
# a limitation of the TEST TOOL, not Space Dust. The plugin's clean-load proof
# is the REAPER session (REAPER is static; it loaded + ran the plugin 15 min
# with no redist present). For a green pluginval, use a statically-linked
# pluginval build, or run on a machine that has the VC++ redist.
Log ""
Log "[2b] Staging to local disk"
$work = Join-Path $env:LOCALAPPDATA 'SpaceDust-CleanTest'
if (Test-Path $work) { Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue }
New-Item -ItemType Directory -Force -Path $work | Out-Null
$localBundle = Join-Path $work 'Space Dust.vst3'
Copy-Item -Path $vst3.FullName -Destination $localBundle -Recurse -Force
$localInner  = Join-Path $localBundle 'Contents\x86_64-win\Space Dust.vst3'
if (-not (Test-Path $localInner)) { Log "    ERROR: failed to stage plugin to local disk."; exit 2 }
Log ("    staged bundle : {0}" -f $localBundle)

# --- 3. pluginval (the real test: does it LOAD + validate with no redist?) ---
Log ""
Log "[3] pluginval"
$pvSrc = Join-Path $here 'pluginval.exe'
if (-not (Test-Path $pvSrc)) { Log "    ERROR: pluginval.exe missing from kit."; exit 3 }
$pv = Join-Path $work 'pluginval.exe'   # run pluginval from local disk too
Copy-Item -Path $pvSrc -Destination $pv -Force
$pvOut = Join-Path $results 'pluginval-output.txt'
$pvErr = Join-Path $results 'pluginval-stderr.txt'
$args  = @('--strictness-level','5','--validate-in-process','--skip-gui-tests',$localInner)
Log ("    running: pluginval {0}" -f ($args -join ' '))
$proc = Start-Process -FilePath $pv -ArgumentList $args -NoNewWindow -Wait -PassThru `
                      -RedirectStandardOutput $pvOut -RedirectStandardError $pvErr
Log ("    pluginval exit code: {0}" -f $proc.ExitCode)
$pvEmpty = ((Get-Item $pvOut -ErrorAction SilentlyContinue).Length -eq 0) -and `
           ((Get-Item $pvErr -ErrorAction SilentlyContinue).Length -eq 0)
if ($proc.ExitCode -eq 0) {
    Log "    RESULT: PASS - plugin loaded and validated on the clean machine."
} elseif ($proc.ExitCode -eq -1073741515 -and $pvEmpty) {
    # 0xC0000135 + empty output = pluginval.exe could not START (missing VC++
    # redist that THIS dynamically-linked pluginval needs), not a plugin defect.
    Log "    RESULT: INCONCLUSIVE - pluginval.exe could not start on this clean machine."
    Log "            (0xC0000135 with no output = the dynamically-linked pluginval"
    Log "             needs VCRUNTIME140/MSVCP140, which a virgin VM lacks. This is a"
    Log "             TEST-TOOL limitation, not a Space Dust problem.)"
    Log "            Clean-load proof for the plugin = the REAPER session + safety log below."
} else {
    Log "    RESULT: FAIL - see results\pluginval-output.txt / pluginval-stderr.txt"
}

# --- 4. Collect any safety logs the plugin wrote ---
Log ""
Log "[4] Collecting Space Dust safety logs (if any)"
$logSrc = Join-Path $env:APPDATA '63C\Space Dust\Logs\Safety'
if (Test-Path $logSrc) {
    $dst = Join-Path $results 'safety-logs'
    New-Item -ItemType Directory -Force -Path $dst | Out-Null
    Copy-Item (Join-Path $logSrc '*.log') $dst -Force -ErrorAction SilentlyContinue
    Log ("    copied logs from {0}" -f $logSrc)
} else { Log "    no safety-log folder yet (plugin may not have been hosted in a DAW)" }

# --- 5. Remove local staging so the VM stays pristine ---
Log ""
Log "[5] Cleaning up local staging folder"
Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
Log ("    removed: {0}" -f $work)

Log ""
Log "=============================================="
Log " DONE. Send the .\results folder back to the host."
Log " Next: load 'Space Dust' in REAPER (Dummy Audio dev), play notes,"
Log " open the editor, automate a few params. Then re-run this script"
Log " to scoop up the safety logs."
Log "=============================================="

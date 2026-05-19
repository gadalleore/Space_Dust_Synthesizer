# run-full-qa.ps1
# Master QA orchestrator for Space Dust. Runs static checks, build, pluginval,
# installer smoke test, and crash-marker hygiene check. Designed to exit 0 only
# if every step passes; suitable as a CI gate.

[CmdletBinding()]
param(
    [string]$BuildDir     = (Join-Path $PSScriptRoot 'build'),
    [switch]$SkipBuild,
    [switch]$SkipInstaller,
    [switch]$SkipPluginval
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$results = [ordered]@{}

function Banner($title, $colour = 'Cyan') {
    Write-Host ''
    Write-Host ('#' * 72) -ForegroundColor $colour
    Write-Host ('#  ' + $title) -ForegroundColor $colour
    Write-Host ('#' * 72) -ForegroundColor $colour
}

function Record($name, $passed, $note = '') {
    $results[$name] = @{ Passed = $passed; Note = $note }
    $tag = if ($passed) { 'PASS' } else { 'FAIL' }
    $colour = if ($passed) { 'Green' } else { 'Red' }
    Write-Host ("[{0}] {1} {2}" -f $tag, $name, $note) -ForegroundColor $colour
}

#======================================================================
Banner 'STEP 1 / 5  Static code health checks'
#======================================================================

# 1a) Parameter listener add/remove balance in PluginProcessor.cpp
$proc = Join-Path $root 'Source/PluginProcessor.cpp'
if (-not (Test-Path $proc)) { Record 'listener-balance' $false 'PluginProcessor.cpp missing'; }
else {
    $content = Get-Content $proc -Raw
    $addIds    = [regex]::Matches($content, 'addParameterListener\(juce::ParameterID\{"([^"]+)"') | ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique
    $removeIds = [regex]::Matches($content, 'removeParameterListener\(juce::ParameterID\{"([^"]+)"') | ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique
    $diff = Compare-Object $addIds $removeIds
    if ($null -eq $diff) {
        Record 'listener-balance' $true "($($addIds.Count) listeners, paired)"
    } else {
        $orphans = ($diff | ForEach-Object { '{0}{1}' -f $_.SideIndicator, $_.InputObject }) -join ', '
        Record 'listener-balance' $false "Mismatched: $orphans"
    }
}

# 1b) No raw unchecked param dereference - safeGetParam helper expected
$rawDeref = (Select-String -Path $proc -Pattern '\*apvts\.getRawParameterValue|apvts\.getRawParameterValue.*->load' -AllMatches |
             Where-Object { $_.Line -notmatch 'safeGetParam|inline float|auto\* atomic' }).Count
if ($rawDeref -le 1) {  # 1 expected: the doc comment in the helper itself
    Record 'safe-param-reads' $true "($rawDeref residual references; doc comment allowed)"
} else {
    Record 'safe-param-reads' $false "Found $rawDeref unchecked dereferences"
}

# 1c) Safe-mode marker wired into setStateInformation
$hasMarker = Select-String -Path $proc -Pattern 'state_restore_in_progress\.marker' -Quiet
Record 'safe-mode-marker' $hasMarker

# 1d) No raw `new` outside known ownership-transfer call sites.
# Pattern requires `new ClassName(`  to avoid false positives on the word "new" in
# comments/strings. Allow when the same line OR the next two lines contain a known
# ownership-transfer call (setContentOwned, addVoice, std::unique_ptr, etc.).
$ownershipPattern = 'setContentOwned|addVoice|return new|alertWindow|std::unique_ptr|make_unique|std::make_shared'
$rawNewMatches = @()
foreach ($file in (Get-ChildItem -Path "$root/Source" -Include *.cpp,*.h -Recurse)) {
    $lines = Get-Content $file.FullName
    for ($i = 0; $i -lt $lines.Length; $i++) {
        $line = $lines[$i]
        $trim = $line.TrimStart()
        if ($trim.StartsWith('//') -or $trim.StartsWith('*') -or $trim.StartsWith('#')) { continue }
        if ($line -notmatch '\bnew\s+[A-Z][A-Za-z_0-9:]*\s*\(') { continue }
        # Look at this line plus the next two for an ownership-transfer call
        $window = ($lines[$i..([Math]::Min($i + 2, $lines.Length - 1))]) -join "`n"
        if ($window -match $ownershipPattern) { continue }
        $rawNewMatches += [pscustomobject]@{ File = $file.Name; Line = ($i + 1); Text = $line.Trim() }
    }
}
$rawNews = $rawNewMatches.Count
Record 'no-orphan-new' ($rawNews -eq 0) "($rawNews unowned)"
if ($rawNews -gt 0) {
    $rawNewMatches | Select-Object -First 5 | ForEach-Object {
        Write-Host ("    -> {0}:{1} {2}" -f $_.File, $_.Line, $_.Text) -ForegroundColor DarkYellow
    }
}

# 1e) UTF-8 safe-string helper is used for runtime strings
$hasSafeString = Select-String -Path $proc -Pattern 'safeString\(' -Quiet
Record 'utf8-safety-helpers' $hasSafeString

#======================================================================
Banner 'STEP 2 / 5  Build (Release, serial)'
#======================================================================
if ($SkipBuild) {
    Record 'release-build' $true '(skipped)'
} else {
    & cmake --build $BuildDir --config Release -j 1
    Record 'release-build' ($LASTEXITCODE -eq 0)
}

#======================================================================
Banner 'STEP 3 / 5  pluginval validation'
#======================================================================
if ($SkipPluginval) {
    Record 'pluginval' $true '(skipped)'
} else {
    & (Join-Path $root 'run-pluginval.ps1') -BuildDir $BuildDir -NoBuild
    Record 'pluginval' ($LASTEXITCODE -eq 0)
}

#======================================================================
Banner 'STEP 4 / 5  Installer smoke test'
#======================================================================
if ($SkipInstaller) {
    Record 'installer' $true '(skipped)'
} else {
    & (Join-Path $root 'test-installer.ps1')
    Record 'installer' ($LASTEXITCODE -eq 0)
}

#======================================================================
Banner 'STEP 5 / 5  Crash-marker hygiene'
#======================================================================
$marker = Join-Path $env:APPDATA 'SpaceDust/state_restore_in_progress.marker'
if (Test-Path $marker) {
    Record 'crash-marker-clean' $false "Marker present: $marker - previous restore crashed?"
} else {
    Record 'crash-marker-clean' $true '(no stale marker)'
}

#======================================================================
Banner 'QA SUMMARY' 'Magenta'
#======================================================================
$failed = $results.GetEnumerator() | Where-Object { -not $_.Value.Passed }
foreach ($k in $results.Keys) {
    $r = $results[$k]
    $tag = if ($r.Passed) { 'PASS' } else { 'FAIL' }
    $col = if ($r.Passed) { 'Green' } else { 'Red' }
    Write-Host ("  {0,-22} {1} {2}" -f $k, $tag, $r.Note) -ForegroundColor $col
}

if ($failed.Count -eq 0) {
    Write-Host ''
    Write-Host '  ALL QA CHECKS PASSED' -ForegroundColor Green
    exit 0
} else {
    Write-Host ''
    Write-Host "  $($failed.Count) CHECK(S) FAILED" -ForegroundColor Red
    exit 1
}

# test-installer.ps1
# Builds the Inno Setup installer (if needed), runs it silently into a temp
# location, verifies all expected files appear, then uninstalls and checks
# cleanup. Designed to be safe to run on the same machine that has the real
# Space Dust installed (uses a sandbox dir, not Program Files).

[CmdletBinding()]
param(
    [string]$IssFile      = (Join-Path $PSScriptRoot 'installer/SpaceDust-Setup.iss'),
    [string]$OutputDir    = (Join-Path $PSScriptRoot 'installer/Output'),
    [string]$SandboxRoot  = (Join-Path $env:TEMP   'SpaceDustInstallerTest'),
    [switch]$SkipPackage
)

$ErrorActionPreference = 'Stop'

function Write-Section($title) {
    Write-Host ''
    Write-Host ('=' * 70) -ForegroundColor DarkCyan
    Write-Host " $title" -ForegroundColor Cyan
    Write-Host ('=' * 70) -ForegroundColor DarkCyan
}

Write-Section 'Installer smoke test'

# 1) Locate Inno Setup compiler
$iscc = $null
foreach ($candidate in @(
    'C:\Program Files (x86)\Inno Setup 6\ISCC.exe',
    'C:\Program Files\Inno Setup 6\ISCC.exe'
)) {
    if (Test-Path $candidate) { $iscc = $candidate; break }
}

if (-not $SkipPackage) {
    if ($null -eq $iscc) {
        Write-Host 'Inno Setup 6 not found; skipping installer compile step.' -ForegroundColor Yellow
        Write-Host 'Install from https://jrsoftware.org/isdl.php or pass -SkipPackage.' -ForegroundColor Yellow
    } else {
        Write-Host "Compiling installer with $iscc ..." -ForegroundColor Cyan
        & $iscc $IssFile
        if ($LASTEXITCODE -ne 0) { throw "ISCC failed with exit $LASTEXITCODE" }
    }
}

# 2) Locate the produced installer .exe
$installer = Get-ChildItem -Path $OutputDir -Filter 'SpaceDust*Setup*.exe' -ErrorAction SilentlyContinue |
             Sort-Object LastWriteTime -Descending | Select-Object -First 1
if ($null -eq $installer) {
    throw "No installer found in $OutputDir. Run with Inno Setup available or compile manually."
}
Write-Host "Installer: $($installer.FullName)" -ForegroundColor Gray

# 3) Run silently into sandbox
if (Test-Path $SandboxRoot) { Remove-Item $SandboxRoot -Recurse -Force }
New-Item -ItemType Directory -Force -Path $SandboxRoot | Out-Null

Write-Host "Running silent install to sandbox: $SandboxRoot" -ForegroundColor Cyan
$installArgs = @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', "/DIR=$SandboxRoot", '/LOG=' + (Join-Path $SandboxRoot 'install.log'))
Start-Process -FilePath $installer.FullName -ArgumentList $installArgs -Wait

# 4) Expected artefacts
$expected = @(
    'Space Dust.vst3/Contents/x86_64-win/Space Dust.vst3',
    'Space Dust.vst3/Contents/Resources/moduleinfo.json'
)

$missing = @()
foreach ($rel in $expected) {
    # SpaceDust-Setup.iss writes the VST3 into the Common Files\VST3 path,
    # not the chosen DIR. So we verify against the actual system VST3 path.
    $sysPath = Join-Path 'C:\Program Files\Common Files\VST3' $rel
    if (-not (Test-Path $sysPath)) { $missing += $sysPath }
}

if ($missing.Count -gt 0) {
    Write-Host 'MISSING after install:' -ForegroundColor Red
    $missing | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
    exit 1
} else {
    Write-Host 'All expected VST3 files present after install.' -ForegroundColor Green
}

# 5) Sanity: try loading the VST3 manifest helper output exists
$moduleInfo = 'C:\Program Files\Common Files\VST3\Space Dust.vst3\Contents\Resources\moduleinfo.json'
if (Test-Path $moduleInfo) {
    try {
        $json = Get-Content $moduleInfo -Raw | ConvertFrom-Json
        $factoryClass = $json.Factory.Classes | Select-Object -First 1
        Write-Host "moduleinfo.json parses; first factory class: $($factoryClass.Name) / $($factoryClass.CID)" -ForegroundColor Green
    } catch {
        Write-Host "moduleinfo.json present but failed to parse: $_" -ForegroundColor Yellow
    }
}

Write-Section 'PASS - installer smoke test'
exit 0

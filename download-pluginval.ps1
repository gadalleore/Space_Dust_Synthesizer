# download-pluginval.ps1
# Downloads the latest Windows release of Tracktion pluginval into ./tools/pluginval/
# Safe to re-run; skips download if already present unless -Force is passed.

[CmdletBinding()]
param(
    [switch]$Force,
    [string]$DownloadDir = (Join-Path $PSScriptRoot 'tools/pluginval')
)

$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$exe = Join-Path $DownloadDir 'pluginval.exe'
if ((Test-Path $exe) -and -not $Force) {
    Write-Host "pluginval already present at: $exe" -ForegroundColor Green
    & $exe --version
    return
}

New-Item -ItemType Directory -Force -Path $DownloadDir | Out-Null

Write-Host 'Querying GitHub for latest pluginval release...' -ForegroundColor Cyan
$release = Invoke-RestMethod 'https://api.github.com/repos/Tracktion/pluginval/releases/latest' `
    -UseBasicParsing -Headers @{ 'User-Agent' = 'SpaceDust-QA' }

$asset = $release.assets | Where-Object { $_.name -match 'pluginval_Windows.*\.zip' } | Select-Object -First 1
if ($null -eq $asset) {
    throw "No Windows asset found in pluginval release $($release.tag_name)"
}

$zipPath = Join-Path $env:TEMP $asset.name
Write-Host "Downloading $($asset.name) ($([Math]::Round($asset.size / 1MB, 1)) MB)..." -ForegroundColor Cyan
Invoke-WebRequest -Uri $asset.browser_download_url -OutFile $zipPath -UseBasicParsing

Write-Host "Extracting to $DownloadDir ..." -ForegroundColor Cyan
Expand-Archive -Path $zipPath -DestinationPath $DownloadDir -Force
Remove-Item $zipPath -Force

# Locate the pluginval executable inside the extracted tree
$found = Get-ChildItem -Path $DownloadDir -Recurse -Filter 'pluginval.exe' | Select-Object -First 1
if ($null -eq $found) {
    throw 'pluginval.exe not found after extraction.'
}
if ($found.FullName -ne $exe) {
    Copy-Item $found.FullName $exe -Force
}

Write-Host "Installed pluginval $($release.tag_name) at: $exe" -ForegroundColor Green
& $exe --version

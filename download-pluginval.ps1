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

# Authenticate the api.github.com call when a token is present (CI sets GH_TOKEN /
# GITHUB_TOKEN). Unauthenticated calls share a 60-req/hr-per-IP limit, and GitHub
# Actions runners share IPs heavily, so the latest-release query intermittently
# returns HTTP 403 (rate limited) and fails the job. A token raises the limit to
# 5000/hr. Local runs with no token behave exactly as before (anonymous).
$headers = @{ 'User-Agent' = 'SpaceDust-QA' }
$token = $env:GH_TOKEN
if ([string]::IsNullOrWhiteSpace($token)) { $token = $env:GITHUB_TOKEN }
if (-not [string]::IsNullOrWhiteSpace($token)) {
    $headers['Authorization'] = "Bearer $token"
    Write-Host 'Using GitHub token for the release-metadata API call.' -ForegroundColor DarkGray
}

# Small retry around the network calls: even an authenticated API/CDN request can
# hiccup transiently on a runner. Up to 4 attempts with linear backoff.
function Invoke-WithRetry {
    param([Parameter(Mandatory)][scriptblock]$Action, [string]$What = 'request')
    $maxAttempts = 4
    for ($attempt = 1; $attempt -le $maxAttempts; $attempt++) {
        try { return & $Action }
        catch {
            if ($attempt -eq $maxAttempts) { throw }
            $wait = 5 * $attempt
            Write-Host "  $What failed (attempt $attempt/$maxAttempts): $($_.Exception.Message)" -ForegroundColor Yellow
            Write-Host "  retrying in $wait s..." -ForegroundColor Yellow
            Start-Sleep -Seconds $wait
        }
    }
}

Write-Host 'Querying GitHub for latest pluginval release...' -ForegroundColor Cyan
$release = Invoke-WithRetry -What 'release-metadata query' -Action {
    Invoke-RestMethod 'https://api.github.com/repos/Tracktion/pluginval/releases/latest' `
        -UseBasicParsing -Headers $headers
}

$asset = $release.assets | Where-Object { $_.name -match 'pluginval_Windows.*\.zip' } | Select-Object -First 1
if ($null -eq $asset) {
    throw "No Windows asset found in pluginval release $($release.tag_name)"
}

$zipPath = Join-Path $env:TEMP $asset.name
Write-Host "Downloading $($asset.name) ($([Math]::Round($asset.size / 1MB, 1)) MB)..." -ForegroundColor Cyan
Invoke-WithRetry -What 'asset download' -Action {
    Invoke-WebRequest -Uri $asset.browser_download_url -OutFile $zipPath -UseBasicParsing
}

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

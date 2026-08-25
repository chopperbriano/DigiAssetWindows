<#
.SYNOPSIS
    Stage the release assets for a win.<N> release and generate SHA256SUMS.

.DESCRIPTION
    Collects the six published assets from a completed Release build, regenerates
    web.zip from the repo's web\ folder, and writes a SHA256SUMS file over all of
    them. Prints the `gh release create` command to run next.

    SHA256SUMS exists so a node can detect a corrupt or truncated download before
    it replaces a working binary (setup-digiasset.ps1 and node\update-binaries.ps1
    both verify against it). It is NOT protection against a hostile release: the
    sums ship in the same release as the binaries, so anyone who could swap an exe
    could swap the sums too. Only Authenticode signing would cover that.

    Generating it here rather than by hand is the point - a checksum file that is
    only published when someone remembers is worse than none, because the installer
    silently downgrades to a format check and nobody notices.

    web.zip is regenerated rather than copied from build\: build\web.zip is not
    produced by CMake and has gone stale before, which would have silently shipped
    an old web console.

.PARAMETER OutDir   Where to stage (default: .\release-staging).
.PARAMETER Config   Build configuration to collect from (default: Release).

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\tools\stage-release.ps1
#>
[CmdletBinding()]
param(
    [string]$OutDir = '',
    [string]$Config = 'Release'
)
$ErrorActionPreference = 'Stop'

$RepoRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
if (-not $OutDir) { $OutDir = Join-Path $RepoRoot 'release-staging' }

# name -> path relative to the repo root. Keep this list in step with what the
# release actually publishes: a missing asset makes the documented
# /releases/latest/download/<file> URL 404 for everyone.
$Assets = [ordered]@{
    'DigiAssetWindows.exe'     = "build\src\$Config\DigiAssetWindows.exe"
    'DigiAssetWindows-cli.exe' = "build\cli\$Config\DigiAssetWindows-cli.exe"
    'DigiAssetPoolServer.exe'  = "build\pool\$Config\DigiAssetPoolServer.exe"
    'example.cfg'              = 'example.cfg'
    'seed-digibyte.ps1'        = 'snapshots\seed-digibyte.ps1'
}

Write-Host "Staging release assets -> $OutDir" -ForegroundColor Cyan
if (Test-Path $OutDir) { Get-ChildItem $OutDir -Force | Remove-Item -Recurse -Force }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$missing = @()
foreach ($name in $Assets.Keys) {
    $src = Join-Path $RepoRoot $Assets[$name]
    if (-not (Test-Path $src)) { $missing += "$name  (expected at $($Assets[$name]))"; continue }
    Copy-Item $src (Join-Path $OutDir $name) -Force
}
if ($missing.Count -gt 0) {
    Write-Host 'MISSING assets - build Release first:' -ForegroundColor Red
    $missing | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
    throw 'refusing to stage an incomplete release'
}

# Rebuild web.zip from source every time (see note above).
$webSrc = Join-Path $RepoRoot 'web'
if (-not (Test-Path $webSrc)) { throw "web\ not found at $webSrc" }
Compress-Archive -Path $webSrc -DestinationPath (Join-Path $OutDir 'web.zip') -CompressionLevel Optimal
Write-Host '  web.zip regenerated from web\' -ForegroundColor Green

# sha256sum-compatible: "<hash>  <filename>", two spaces, sorted for a stable diff.
$lines = Get-ChildItem $OutDir -File | Sort-Object Name | ForEach-Object {
    "{0}  {1}" -f (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLower(), $_.Name
}
Set-Content -Path (Join-Path $OutDir 'SHA256SUMS') -Value $lines -Encoding ASCII

# Read the version back from the binary that will actually ship, rather than from
# CMakeLists - this catches a stale build being staged against a bumped WIN_BUILD.
$exe = Join-Path $OutDir 'DigiAssetWindows.exe'
$bytes = [System.IO.File]::ReadAllBytes($exe)
$text = [System.Text.Encoding]::ASCII.GetString($bytes)
$m = [regex]::Match($text, '\d+\.\d+\.\d+-win\.\d+')
$ver = if ($m.Success) { $m.Value } else { 'UNKNOWN' }

Write-Host ''
Write-Host 'Staged:' -ForegroundColor Cyan
Get-ChildItem $OutDir -File | Sort-Object Name | ForEach-Object { "  {0,-28} {1,10:N0}" -f $_.Name, $_.Length }
Write-Host ''
Write-Host "Version in the staged binary: $ver" -ForegroundColor Green
if ($ver -eq 'UNKNOWN') { Write-Host '  WARNING: could not read a version out of the exe.' -ForegroundColor Yellow }
Write-Host ''
Write-Host 'Next:' -ForegroundColor Cyan
Write-Host "  cd `"$OutDir`"" -ForegroundColor Gray
Write-Host "  gh release create v$ver --repo chopperbriano/DigiAssetWindows --title `"v$ver`" --notes `"...`" $((Get-ChildItem $OutDir -File | Sort-Object Name | ForEach-Object { $_.Name }) -join ' ')" -ForegroundColor Gray

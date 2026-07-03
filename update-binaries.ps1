<#
.SYNOPSIS
    Update the DigiAsset binaries to the latest GitHub release in ONE step:
    download the newest DigiAssetWindows.exe (+ cli, + optionally the pool server)
    from the repo's Releases, stop the running app, copy the new exe in, restart.

.DESCRIPTION
    Pulls from https://github.com/chopperbriano/DigiAssetWindows/releases/latest by
    default. The node's built-in maintenance task already auto-updates on a schedule;
    this is for updating on demand (and for the pool exe, which isn't auto-updated).

.PARAMETER DigiAssetDir  Where the exes live (default C:\DigiAsset).
.PARAMETER IncludePool   Also update DigiAssetPoolServer.exe (auto-on if it's already there).
.PARAMETER FromBuild     Use locally-built exes in .\build instead of GitHub.
.PARAMETER Build         With -FromBuild, run the cmake Release build first.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\update-binaries.ps1               # pull latest release
    powershell -ExecutionPolicy Bypass -File .\update-binaries.ps1 -IncludePool  # + pool server
    powershell -ExecutionPolicy Bypass -File .\update-binaries.ps1 -FromBuild -Build
#>
[CmdletBinding()]
param(
    [string]$DigiAssetDir = 'C:\DigiAsset',
    [switch]$IncludePool,
    [switch]$FromBuild,
    [switch]$Build,
    [string]$Config = 'Release'
)
$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$ScriptVersion = '2.0.0'
$Repo     = 'chopperbriano/DigiAssetWindows'
$RepoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$BuildDir = Join-Path $RepoRoot 'build'
$Tmp      = Join-Path $env:TEMP 'digiasset-update'

# Elevate: writing into C:\DigiAsset and stopping processes may need admin.
$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) {
    if ($PSCommandPath) {
        $a = "-ExecutionPolicy Bypass -File `"$PSCommandPath`" -DigiAssetDir `"$DigiAssetDir`" -Config `"$Config`""
        if ($IncludePool) { $a += ' -IncludePool' }
        if ($FromBuild)   { $a += ' -FromBuild' }
        if ($Build)       { $a += ' -Build' }
        Start-Process powershell.exe -Verb RunAs -ArgumentList $a
        return
    } else { throw 'Run this in an elevated (Administrator) PowerShell.' }
}

Write-Host "=== Update DigiAsset binaries  (v$ScriptVersion) ===" -ForegroundColor Cyan
if (-not (Test-Path $DigiAssetDir)) { New-Item -ItemType Directory -Force -Path $DigiAssetDir | Out-Null }
New-Item -ItemType Directory -Force -Path $Tmp | Out-Null

# Optional local build (only relevant with -FromBuild).
if ($FromBuild -and $Build) {
    Write-Host "Building ($Config)..." -ForegroundColor Cyan
    $cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
    if (-not $cmake) {
        $vs = "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
        if (Test-Path $vs) { $cmake = $vs }
    }
    if (-not $cmake) { throw 'cmake not found. Open a Developer PowerShell (or install CMake) and re-run.' }
    & $cmake --build $BuildDir --config $Config --target DigiAssetWindows DigiAssetWindows-cli DigiAssetPoolServer
    if ($LASTEXITCODE -ne 0) { throw "build failed (exit $LASTEXITCODE)." }
    Write-Host "Build OK." -ForegroundColor Green
}

if ($FromBuild) {
    Write-Host "Source: local build ($BuildDir, $Config)" -ForegroundColor Gray
} else {
    $tag = ''
    try { $tag = (Invoke-RestMethod "https://api.github.com/repos/$Repo/releases/latest" -Headers @{ 'User-Agent' = 'digiasset-updater' } -TimeoutSec 20).tag_name } catch {}
    Write-Host "Source: GitHub release $(if ($tag) { $tag } else { 'latest' })" -ForegroundColor Gray
}
Write-Host "Target: $DigiAssetDir`n" -ForegroundColor Gray

# node + cli always; pool if asked or already present (so node boxes don't get it).
$wantPool = $IncludePool -or (Test-Path (Join-Path $DigiAssetDir 'DigiAssetPoolServer.exe'))
$items = @(
    @{ name = 'DigiAssetWindows.exe';     buildSub = 'src';  proc = 'DigiAssetWindows';    restart = $true;  want = $true     },
    @{ name = 'DigiAssetWindows-cli.exe'; buildSub = 'cli';  proc = $null;                 restart = $false; want = $true     },
    @{ name = 'DigiAssetPoolServer.exe';  buildSub = 'pool'; proc = 'DigiAssetPoolServer'; restart = $true;  want = $wantPool }
)

# Resolve the new exe (download from the release, or the local build path).
function Get-SourceExe($item) {
    if ($FromBuild) {
        $p = Join-Path $BuildDir "$($item.buildSub)\$Config\$($item.name)"
        if (Test-Path $p) { return $p } else { return $null }
    }
    $url = "https://github.com/$Repo/releases/latest/download/$($item.name)"
    $out = Join-Path $Tmp $item.name
    for ($i = 1; $i -le 3; $i++) {
        try {
            Invoke-WebRequest -Uri $url -OutFile $out -UseBasicParsing -TimeoutSec 180
            if ((Test-Path $out) -and (Get-Item $out).Length -gt 0) { return $out }
        } catch { Start-Sleep -Seconds (2 * $i) }
    }
    return $null
}

$updated = 0
$restartList = @()
foreach ($it in $items) {
    if (-not $it.want) { continue }
    $src = Get-SourceExe $it
    if (-not $src) { Write-Host "  skip $($it.name) - could not get it (not built / not in the release?)" -ForegroundColor Yellow; continue }
    $dst = Join-Path $DigiAssetDir $it.name
    $wasRunning = $false
    if ($it.proc -and (Get-Process $it.proc -ErrorAction SilentlyContinue)) {
        $wasRunning = $true
        Get-Process $it.proc -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
        # Wait until every instance is really gone so the .exe unlocks before copy.
        for ($w = 0; $w -lt 20 -and (Get-Process $it.proc -ErrorAction SilentlyContinue); $w++) { Start-Sleep -Milliseconds 500 }
        if (Get-Process $it.proc -ErrorAction SilentlyContinue) { Write-Host "  WARNING: $($it.proc) still running after kill" -ForegroundColor Red }
        else { Write-Host "  stopped $($it.proc)" -ForegroundColor Yellow }
    }
    # Copy, retrying briefly in case the file is momentarily still locked.
    $copied = $false
    for ($c = 1; $c -le 6 -and -not $copied; $c++) {
        try { Copy-Item -LiteralPath $src -Destination $dst -Force; $copied = $true }
        catch { Start-Sleep -Seconds 1 }
    }
    if (-not $copied) { Write-Host "  FAILED to update $($it.name) - file locked (is it still running?)" -ForegroundColor Red; continue }
    $updated++
    Write-Host "  + updated $($it.name)" -ForegroundColor Green
    if ($it.restart -and $wasRunning) { $restartList += $dst }
}

if ($updated -eq 0) { Write-Host "`nNothing updated." -ForegroundColor Yellow; return }

foreach ($exe in $restartList) {
    Start-Process -FilePath $exe -WorkingDirectory $DigiAssetDir
    Write-Host "  restarted $(Split-Path -Leaf $exe)" -ForegroundColor Green
}

Write-Host "`nDone. Updated $updated binary(ies) in $DigiAssetDir." -ForegroundColor Green
if ($restartList.Count -eq 0) { Write-Host "(Apps weren't running - they'll start via your logon tasks / next login.)" -ForegroundColor Gray }

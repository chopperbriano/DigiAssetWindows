<#
.SYNOPSIS
    Deploy freshly-built binaries (DigiAssetWindows.exe, DigiAssetWindows-cli.exe,
    DigiAssetPoolServer.exe) from .\build into a live install in ONE step: stop the
    running app, copy the new exe over it, restart. Optionally build first.

    For DEVELOPERS / OPERATORS updating a box from a local build.

.PARAMETER Build         Run the cmake Release build first (all three targets).
.PARAMETER DigiAssetDir  Where the deployed exes live (default C:\DigiAsset).
.PARAMETER Config        Build config to copy from (default Release).

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\update-binaries.ps1
    powershell -ExecutionPolicy Bypass -File .\update-binaries.ps1 -Build
#>
[CmdletBinding()]
param(
    [switch]$Build,
    [string]$DigiAssetDir = 'C:\DigiAsset',
    [string]$Config       = 'Release'
)
$ErrorActionPreference = 'Stop'
$ScriptVersion = '1.0.0'

$RepoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$BuildDir = Join-Path $RepoRoot 'build'

# Elevate: writing into C:\DigiAsset and stopping processes may need admin.
$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) {
    if ($PSCommandPath) {
        $a = "-ExecutionPolicy Bypass -File `"$PSCommandPath`" -DigiAssetDir `"$DigiAssetDir`" -Config `"$Config`""
        if ($Build) { $a += ' -Build' }
        Start-Process powershell.exe -Verb RunAs -ArgumentList $a
        return
    } else { throw 'Run this in an elevated (Administrator) PowerShell.' }
}

Write-Host "=== Update DigiAsset binaries  (v$ScriptVersion) ===" -ForegroundColor Cyan
Write-Host "Repo:   $RepoRoot" -ForegroundColor Gray
Write-Host "Target: $DigiAssetDir" -ForegroundColor Gray

# 1. Optional build --------------------------------------------------------
if ($Build) {
    Write-Host "`nBuilding ($Config)..." -ForegroundColor Cyan
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

if (-not (Test-Path $DigiAssetDir)) { New-Item -ItemType Directory -Force -Path $DigiAssetDir | Out-Null }

# 2. Map built -> deployed -------------------------------------------------
$items = @(
    @{ name = 'DigiAssetWindows.exe';     src = (Join-Path $BuildDir "src\$Config\DigiAssetWindows.exe");     proc = 'DigiAssetWindows';    restart = $true  },
    @{ name = 'DigiAssetWindows-cli.exe'; src = (Join-Path $BuildDir "cli\$Config\DigiAssetWindows-cli.exe"); proc = $null;                 restart = $false },
    @{ name = 'DigiAssetPoolServer.exe';  src = (Join-Path $BuildDir "pool\$Config\DigiAssetPoolServer.exe"); proc = 'DigiAssetPoolServer'; restart = $true  }
)

# 3. For each present build output: stop, copy, note whether to restart ----
$restartList = @()
$updated = 0
Write-Host ""
foreach ($it in $items) {
    if (-not (Test-Path $it.src)) { Write-Host "  skip $($it.name) - not built at $($it.src)" -ForegroundColor DarkGray; continue }
    $dst = Join-Path $DigiAssetDir $it.name
    $wasRunning = $false
    if ($it.proc -and (Get-Process $it.proc -ErrorAction SilentlyContinue)) {
        $wasRunning = $true
        Get-Process $it.proc -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
        Start-Sleep -Seconds 2
        Write-Host "  stopped $($it.proc)" -ForegroundColor Yellow
    }
    Copy-Item -LiteralPath $it.src -Destination $dst -Force
    $updated++
    Write-Host "  + updated $($it.name)" -ForegroundColor Green
    if ($it.restart -and $wasRunning) { $restartList += $dst }
}

if ($updated -eq 0) {
    Write-Host "`nNothing to update - no built binaries found in $BuildDir. Add -Build to build first." -ForegroundColor Yellow
    return
}

# 4. Restart whatever was running before the swap --------------------------
foreach ($exe in $restartList) {
    Start-Process -FilePath $exe -WorkingDirectory $DigiAssetDir
    Write-Host "  restarted $(Split-Path -Leaf $exe)" -ForegroundColor Green
}

Write-Host "`nDone. Updated $updated binary(ies) in $DigiAssetDir." -ForegroundColor Green
if ($restartList.Count -eq 0) { Write-Host "(Apps weren't running - they'll start via your logon tasks / next login.)" -ForegroundColor Gray }

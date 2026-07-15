<#
.SYNOPSIS
    Update the DigiAsset binaries to the latest GitHub release in ONE step:
    download the newest DigiAssetWindows.exe (+ cli, + optionally the pool server)
    from the repo's Releases, stop the running app, copy the new exe in, restart.

.DESCRIPTION
    Pulls from https://github.com/chopperbriano/DigiAssetWindows/releases/latest by
    default. The node's built-in maintenance task already auto-updates on a schedule;
    this is for updating on demand (and for the pool exe, which isn't auto-updated).

.PARAMETER DigiAssetDir  Where the exes live (default C:\DigiAssetWindows).
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
    [string]$DigiAssetDir = 'C:\DigiAssetWindows',
    [switch]$IncludePool,
    [switch]$FromBuild,
    [switch]$Build,
    [switch]$Force,                 # reinstall even if already on the latest release
    [string]$Config = 'Release'
)
$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$ScriptVersion = '2.4.0'
# A real Windows .exe starts with 'MZ'. Reject a truncated download or an HTML
# error body before we overwrite a working binary with garbage.
function Test-ValidExe($path) {
    if (-not (Test-Path $path)) { return $false }
    if ((Get-Item $path).Length -lt 100000) { return $false }   # our exes are >300 KB
    try { $fs = [IO.File]::OpenRead($path); $b0 = $fs.ReadByte(); $b1 = $fs.ReadByte(); $fs.Close() } catch { return $false }
    return ($b0 -eq 0x4D -and $b1 -eq 0x5A)   # 'M','Z'
}
$Supervisors = @('DigiStampNode','DigiStampMaintenance')
$Repo     = 'chopperbriano/DigiAssetWindows'
$RepoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$BuildDir = Join-Path $RepoRoot 'build'
$Tmp      = Join-Path $env:TEMP 'digiasset-update'

# Elevate: writing into C:\DigiAssetWindows and stopping processes may need admin.
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

# Already-up-to-date fast path: skip the download + node restart if we already
# installed this exact release (recorded in .installed-tag). -Force overrides.
$tagFile = Join-Path $DigiAssetDir '.installed-tag'
if (-not $FromBuild -and -not $Force -and $tag -and (Test-Path $tagFile) -and (((Get-Content $tagFile -Raw) + '').Trim() -eq $tag)) {
    Write-Host "Already on the latest release ($tag) - nothing to do." -ForegroundColor Green
    Write-Host "(Use -Force to reinstall it anyway.)" -ForegroundColor Gray
    return
}

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
            # Verify it's a real exe (not a truncated download or an HTML/error
            # body) BEFORE we let it overwrite the working binary.
            if (Test-ValidExe $out) { return $out }
            Write-Host "  downloaded $($item.name) failed validation (retry $i)..." -ForegroundColor Yellow
        } catch { Start-Sleep -Seconds (2 * $i) }
    }
    return $null
}

# Refresh the :8090 web console (the web\ folder next to the exe). From a local
# build we copy the repo's web\; from a release we download web.zip. Must run
# BEFORE the node restarts, since the server picks its web root at startup.
function Update-WebConsole {
    $dest = Join-Path $DigiAssetDir 'web'
    try {
        if ($FromBuild) {
            $srcWeb = Join-Path $RepoRoot 'web'
            if (-not (Test-Path (Join-Path $srcWeb 'index.html'))) { return }
            if (Test-Path $dest) { Remove-Item $dest -Recurse -Force -ErrorAction SilentlyContinue }
            Copy-Item -LiteralPath $srcWeb -Destination $DigiAssetDir -Recurse -Force
            Write-Host "  + web console (from build)" -ForegroundColor Green
        } else {
            $zip = Join-Path $Tmp 'web.zip'
            try { Invoke-WebRequest "https://github.com/$Repo/releases/latest/download/web.zip" -OutFile $zip -UseBasicParsing -TimeoutSec 120 }
            catch { Write-Host "  (web.zip not in this release - keeping existing web console)" -ForegroundColor DarkGray; return }
            if (-not (Test-Path $zip) -or (Get-Item $zip).Length -lt 1KB) { return }
            if (Test-Path $dest) { Remove-Item $dest -Recurse -Force -ErrorAction SilentlyContinue }
            Expand-Archive -Path $zip -DestinationPath $DigiAssetDir -Force   # zip has a top-level web\ folder
            Write-Host "  + web console (from release)" -ForegroundColor Green
        }
    } catch { Write-Host "  (web console refresh skipped: $($_.Exception.Message))" -ForegroundColor Yellow }
}

# Pause the auto-restart supervisors so they can't relaunch the OLD exe during
# the swap (which would re-lock the file or leave two instances). Re-enabled in
# the finally block no matter what happens.
$disabledSupervisors = @()
foreach ($tn in $Supervisors) {
    try { if (Get-ScheduledTask -TaskName $tn -EA SilentlyContinue) { Disable-ScheduledTask -TaskName $tn -EA Stop | Out-Null; $disabledSupervisors += $tn } } catch {}
}

$updated = 0
$restartList = @()
try {
    foreach ($it in $items) {
        if (-not $it.want) { continue }
        $src = Get-SourceExe $it
        if (-not $src) { Write-Host "  skip $($it.name) - could not get it (not built / not in the release?)" -ForegroundColor Yellow; continue }
        $dst = Join-Path $DigiAssetDir $it.name
        if ($it.proc -and (Get-Process $it.proc -ErrorAction SilentlyContinue)) {
            Get-Process $it.proc -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
            # Wait until every instance is really gone so the .exe unlocks before copy.
            for ($w = 0; $w -lt 20 -and (Get-Process $it.proc -ErrorAction SilentlyContinue); $w++) { Start-Sleep -Milliseconds 500 }
            if (Get-Process $it.proc -ErrorAction SilentlyContinue) { Write-Host "  WARNING: $($it.proc) still running after kill" -ForegroundColor Red }
            else { Write-Host "  stopped $($it.proc)" -ForegroundColor Yellow }
        }
        # Back up the working exe so a bad copy can be rolled back.
        $bak = "$dst.bak"
        if (Test-Path $dst) { try { Copy-Item -LiteralPath $dst -Destination $bak -Force } catch {} }
        # Copy, retrying briefly in case the file is momentarily still locked.
        $copied = $false
        for ($c = 1; $c -le 6 -and -not $copied; $c++) {
            try { Copy-Item -LiteralPath $src -Destination $dst -Force; $copied = $true }
            catch { Start-Sleep -Seconds 1 }
        }
        if (-not $copied) { Write-Host "  FAILED to update $($it.name) - file locked (is it still running?)" -ForegroundColor Red; continue }
        # Validate what we just wrote; roll back from .bak if it's not a real exe.
        if (-not (Test-ValidExe $dst)) {
            Write-Host "  update of $($it.name) produced an INVALID file - rolling back to the previous version." -ForegroundColor Red
            if (Test-Path $bak) { try { Copy-Item -LiteralPath $bak -Destination $dst -Force } catch {} }
            continue
        }
        $updated++
        Write-Host "  + updated $($it.name)" -ForegroundColor Green
        if ($it.restart) { $restartList += @{ exe = $dst; proc = $it.proc } }
    }

    # Refresh the web console before restarting the node so it finds web\ at boot.
    if ($updated -gt 0) { Update-WebConsole }

    if ($updated -gt 0) {
        foreach ($r in $restartList) {
            # Only start it if it isn't somehow already running (supervisors are paused).
            if (-not (Get-Process $r.proc -ErrorAction SilentlyContinue)) {
                Start-Process -FilePath $r.exe -WorkingDirectory $DigiAssetDir -WindowStyle Normal
                Write-Host "  restarted $(Split-Path -Leaf $r.exe)" -ForegroundColor Green
            }
        }
        if (-not $FromBuild -and $tag) { try { Set-Content -Path $tagFile -Value $tag -Encoding ASCII } catch {} }
    }
}
finally {
    # Always restore the supervisors so the node keeps auto-starting normally.
    foreach ($tn in $disabledSupervisors) { try { Enable-ScheduledTask -TaskName $tn -EA Stop | Out-Null } catch {} }
}

if ($updated -eq 0) { Write-Host "`nNothing updated." -ForegroundColor Yellow; return }
Write-Host "`nDone. Updated $updated binary(ies) in $DigiAssetDir." -ForegroundColor Green
if ($restartList.Count -eq 0) { Write-Host "(Apps weren't running - they'll start via your logon tasks / next login.)" -ForegroundColor Gray }

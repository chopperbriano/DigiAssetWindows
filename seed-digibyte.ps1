<#
.SYNOPSIS
    Fast-seed a DigiByte Core wallet from a pre-synced snapshot (the SAME source
    the DigiAsset installer uses). Downloads the DigiByte blockchain archive,
    verifies its SHA256, and extracts it into the wallet's data directory - so the
    wallet only syncs the recent delta instead of days/weeks from scratch.

    Works for ANY DigiByte Core wallet, not just DigiAsset.

.PARAMETER SnapshotUrl  URL of snapshot.json on your Cloudflare R2. Required.
.PARAMETER DataDir      DigiByte data directory to seed.
                        Default: %APPDATA%\DigiByte (DigiByte Core's own default).
                        Use C:\DigiByte\data for the DigiAsset layout.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\seed-digibyte.ps1 -SnapshotUrl https://pub-xxxx.r2.dev/snapshot.json
    powershell -ExecutionPolicy Bypass -File .\seed-digibyte.ps1 -SnapshotUrl https://pub-xxxx.r2.dev/snapshot.json -DataDir C:\DigiByte\data
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$SnapshotUrl,
    [string]$DataDir = (Join-Path $env:APPDATA 'DigiByte')
)
$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$ScriptVersion = '1.0.0'
function Say($m,$c='Gray'){ Write-Host $m -ForegroundColor $c }

# Elevate (writing into the datadir / stopping DigiByte may need admin).
$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) {
    if ($PSCommandPath) { Start-Process powershell.exe -Verb RunAs -ArgumentList "-ExecutionPolicy Bypass -File `"$PSCommandPath`" -SnapshotUrl `"$SnapshotUrl`" -DataDir `"$DataDir`""; return }
    else { throw 'Run this in an elevated (Administrator) PowerShell.' }
}

Say "=== Seed DigiByte wallet from snapshot  (v$ScriptVersion) ===" 'Cyan'
if (-not (Get-Command tar.exe -ErrorAction SilentlyContinue)) { throw 'tar.exe not found (needs Windows 10 1803+ / Windows 11).' }

Say "Fetching manifest: $SnapshotUrl" 'Gray'
$m = Invoke-RestMethod -Uri $SnapshotUrl -TimeoutSec 30
if (-not $m.digibyte -or -not $m.baseUrl) { throw 'Manifest is missing digibyte / baseUrl.' }
$base = ("$($m.baseUrl)").TrimEnd('/')
$url  = "$base/$($m.digibyte.file)"
Say ("Snapshot: {0}   height {1:N0}   DigiByte {2}" -f $m.digibyte.file,[int]$m.digibyte.height,$m.digibyte.version) 'White'
Say "Target:   $DataDir" 'White'

if (Test-Path (Join-Path $DataDir 'blocks')) {
    Say "`nWARNING: $DataDir already has a blockchain (blocks/). The snapshot will overwrite matching files." 'Yellow'
    if ((Read-Host "Continue? (y/N)") -notmatch '^[Yy]') { Say 'Cancelled.'; return }
}

if (Get-Process digibyte-qt,digibyted -ErrorAction SilentlyContinue) {
    Say "Stopping DigiByte so its files can be replaced..." 'Cyan'
    Get-Process digibyte-qt,digibyted -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    for ($i=0; $i -lt 20 -and (Get-Process digibyte-qt,digibyted -ErrorAction SilentlyContinue); $i++) { Start-Sleep -Milliseconds 500 }
    Start-Sleep -Seconds 2
}

New-Item -ItemType Directory -Force -Path $DataDir | Out-Null
$tmp = Join-Path $env:TEMP $m.digibyte.file
if (Test-Path $tmp) { Remove-Item $tmp -Force }

Say "Downloading (large - resumable, please wait)..." 'Cyan'
$got = $false
try { Import-Module BitsTransfer -ErrorAction SilentlyContinue; Start-BitsTransfer -Source $url -Destination $tmp -ErrorAction Stop; $got = $true } catch {}
if (-not $got) { try { Invoke-WebRequest -Uri $url -OutFile $tmp -UseBasicParsing -TimeoutSec 0; $got = (Test-Path $tmp) } catch {} }
if (-not $got) { throw "Download failed: $url" }

Say "Verifying SHA256..." 'Cyan'
$h = (Get-FileHash $tmp -Algorithm SHA256).Hash.ToLower()
if ($h -ne ("$($m.digibyte.sha256)").ToLower()) { Remove-Item $tmp -Force; throw 'Checksum MISMATCH - refusing to use this file.' }
Say "  checksum OK." 'Green'

Say "Extracting into $DataDir ..." 'Cyan'
& tar.exe -xzf "$tmp" -C "$DataDir"
if ($LASTEXITCODE -ne 0) { throw "tar extract failed (exit $LASTEXITCODE)." }
Remove-Item $tmp -Force -ErrorAction SilentlyContinue

Say "`n===== Done =====" 'Green'
Say "DigiByte data seeded to $DataDir (height ~$([int]$m.digibyte.height))." 'White'
Say "Start DigiByte Core; it will verify the snapshot and sync only the recent blocks." 'White'
Say "NOTE: your digibyte.conf must match the snapshot's indexes (txindex=1, blockfilterindex=1)," 'Gray'
Say "otherwise DigiByte will rebuild them. This trusts the snapshot's checksum from your source." 'Gray'

<#
.SYNOPSIS
    Fast-seed a DigiByte Core wallet from a pre-synced snapshot (the SAME source
    the DigiAsset installer uses). Downloads the DigiByte blockchain archive,
    verifies its SHA256, and extracts it into the wallet's data directory - so the
    wallet only syncs the recent delta instead of days/weeks from scratch.

    Works for ANY DigiByte Core wallet, not just DigiAsset.

    Typical use: install DigiByte Core, START it once and CLOSE it (so the data
    directory exists), then run this to seed the blockchain from R2.

.PARAMETER SnapshotUrl  URL of snapshot.json. Defaults to the official R2 feed, so
                        normally you pass nothing.
.PARAMETER DataDir      DigiByte data directory to seed.
                        Default: %APPDATA%\DigiByte (DigiByte Core's own default).
                        Use C:\DigiByte\data for the DigiAsset layout.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\seed-digibyte.ps1
    powershell -ExecutionPolicy Bypass -File .\seed-digibyte.ps1 -DataDir C:\DigiByte\data
#>
[CmdletBinding()]
param(
    [string]$SnapshotUrl = 'https://pub-bd3f441e6b464d499ba583016accfa01.r2.dev/snapshot.json',
    [string]$DataDir = (Join-Path $env:APPDATA 'DigiByte')
)
$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$ScriptVersion = '1.2.0'
function Say($m,$c='Gray'){ Write-Host $m -ForegroundColor $c }

# Resumable BITS download with live %/speed/ETA; falls back to a plain download.
function Get-DL($u,$d){
    Import-Module BitsTransfer -ErrorAction SilentlyContinue
    if (Get-Command Start-BitsTransfer -ErrorAction SilentlyContinue) {
        try {
            $j = Get-BitsTransfer -Name 'DigiByteSeed' -ErrorAction SilentlyContinue | Where-Object { $_.FileList.RemoteName -eq $u } | Select-Object -First 1
            if (-not $j) { Get-BitsTransfer -Name 'DigiByteSeed' -ErrorAction SilentlyContinue | Remove-BitsTransfer -ErrorAction SilentlyContinue; $j = Start-BitsTransfer -Source $u -Destination $d -DisplayName 'DigiByteSeed' -Asynchronous -Priority Foreground -ErrorAction Stop }
            $lb=0; $lt=Get-Date
            while ($j.JobState -in 'Connecting','Transferring','Queued','TransientError') {
                if ($j.JobState -eq 'TransientError') { try { $j | Resume-BitsTransfer -Asynchronous -ErrorAction SilentlyContinue } catch {} }
                $bt=$j.BytesTransferred; $tot=$j.BytesTotal; $now=Get-Date; $s=($now-$lt).TotalSeconds
                $spd = if ($s -ge 1) { ($bt-$lb)/$s } else { $null }; if ($spd) { $lb=$bt; $lt=$now }
                if ($tot -gt 0) { $pct=[int](($bt/$tot)*100); $eta = if ($spd -and $spd -gt 0) { [TimeSpan]::FromSeconds([int](($tot-$bt)/$spd)).ToString() } else { '--:--:--' }
                    Write-Progress -Activity "Downloading DigiByte snapshot" -PercentComplete $pct -Status ("{0:N1}/{1:N1} GB  {2}%  {3:N1} MB/s  ETA {4}" -f ($bt/1GB),($tot/1GB),$pct,(($(if($spd){$spd}else{0}))/1MB),$eta) }
                Start-Sleep -Seconds 2
            }
            Write-Progress -Activity "Downloading DigiByte snapshot" -Completed
            if ($j.JobState -eq 'Transferred') { Complete-BitsTransfer -BitsJob $j; return $true }
            $j | Remove-BitsTransfer -ErrorAction SilentlyContinue; return $false
        } catch {}
    }
    try { Invoke-WebRequest -Uri $u -OutFile $d -UseBasicParsing -TimeoutSec 0; return (Test-Path $d) } catch { return $false }
}
# Extract with a heartbeat (tar is silent for minutes on a huge archive).
function Expand-DL($a,$dst){
    $drv=(Split-Path $dst -Qualifier).TrimEnd(':'); $di=try{New-Object System.IO.DriveInfo $drv}catch{$null}; $fb=if($di){$di.AvailableFreeSpace}else{0}
    Say "Extracting into $dst - heavy disk activity for several minutes; this is NORMAL, not frozen." 'Yellow'
    $p=Start-Process tar.exe -ArgumentList @('-xzf',"$a",'-C',"$dst") -PassThru -WindowStyle Hidden; $t0=Get-Date
    while (-not $p.HasExited) { Start-Sleep -Seconds 5; $w=0; if($di){try{$w=[math]::Max(0,$fb-$di.AvailableFreeSpace)}catch{}}
        Write-Progress -Activity "Extracting" -Status ("~{0:N1} GB written  elapsed {1}  (working...)" -f ($w/1GB),(((Get-Date)-$t0).ToString('hh\:mm\:ss'))) }
    Write-Progress -Activity "Extracting" -Completed; return ($p.ExitCode -eq 0)
}

# Elevate (writing into the datadir / stopping DigiByte may need admin).
$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) {
    if ($PSCommandPath) { Start-Process powershell.exe -Verb RunAs -ArgumentList "-ExecutionPolicy Bypass -File `"$PSCommandPath`" -SnapshotUrl `"$SnapshotUrl`" -DataDir `"$DataDir`""; return }
    else { throw 'Run this in an elevated (Administrator) PowerShell.' }
}

Say "=== Seed DigiByte wallet from snapshot  (v$ScriptVersion) ===" 'Cyan'
if (-not (Get-Command tar.exe -ErrorAction SilentlyContinue)) { throw 'tar.exe not found (needs Windows 10 1803+ / Windows 11).' }

Say "Fetching manifest: $SnapshotUrl" 'Gray'
# Parse defensively: R2 serves .json as octet-stream, so Invoke-RestMethod would
# hand back raw text instead of an object. Fetch text, strip any BOM, parse.
$m = $null
try {
    $resp = Invoke-WebRequest -Uri $SnapshotUrl -UseBasicParsing -TimeoutSec 30
    $txt = $resp.Content
    if ($txt -is [byte[]]) { $txt = [System.Text.Encoding]::UTF8.GetString($txt) }
    $m = ($txt.TrimStart([char]0xFEFF)) | ConvertFrom-Json
} catch { throw "Could not fetch/parse manifest ($SnapshotUrl): $($_.Exception.Message)" }
if (-not $m -or -not $m.digibyte -or -not $m.baseUrl) { throw 'Manifest is missing digibyte / baseUrl.' }
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

Say "Downloading (large; resumable - safe to leave running)..." 'Cyan'
if (-not (Get-DL $url $tmp)) { throw "Download failed: $url" }

Say "Verifying SHA256 (reads the whole file, ~a minute)..." 'Cyan'
$h = (Get-FileHash $tmp -Algorithm SHA256).Hash.ToLower()
if ($h -ne ("$($m.digibyte.sha256)").ToLower()) { Remove-Item $tmp -Force; throw 'Checksum MISMATCH - refusing to use this file.' }
Say "  checksum OK." 'Green'

if (-not (Expand-DL $tmp $DataDir)) { throw 'tar extract failed.' }
Remove-Item $tmp -Force -ErrorAction SilentlyContinue

Say "`n===== Done =====" 'Green'
Say "DigiByte data seeded to $DataDir (height ~$([int]$m.digibyte.height))." 'White'
Say "Start DigiByte Core; it will verify the snapshot and sync only the recent blocks." 'White'
Say "NOTE: your digibyte.conf must match the snapshot's indexes (txindex=1, blockfilterindex=1)," 'Gray'
Say "otherwise DigiByte will rebuild them. This trusts the snapshot's checksum from your source." 'Gray'

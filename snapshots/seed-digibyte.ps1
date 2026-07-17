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
.PARAMETER DataDir      DigiByte data directory to seed. If you OMIT this, the
                        script PROMPTS you for the location (offering the detected
                        default) and sanity-checks it before extracting.
                        Stock DigiByte Core:            %APPDATA%\DigiByte
                        DigiAsset for Windows layout:   C:\DigiByte\data
.PARAMETER Force        Skip the "does this look like a DigiByte folder?" check and
                        the overwrite confirmation (for unattended/scripted use).

.EXAMPLE
    # Interactive - prompts for the location and validates it before extracting:
    powershell -ExecutionPolicy Bypass -File .\seed-digibyte.ps1

.EXAMPLE
    # Non-interactive - seed a specific data directory:
    powershell -ExecutionPolicy Bypass -File .\seed-digibyte.ps1 -DataDir "$env:APPDATA\DigiByte"
#>
[CmdletBinding()]
param(
    [string]$SnapshotUrl = 'https://pub-bd3f441e6b464d499ba583016accfa01.r2.dev/snapshot.json',
    [string]$DataDir = 'C:\DigiByte\data',
    [switch]$Force
)
$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$ScriptVersion = '1.4.0'
# Did the user set -DataDir explicitly? Captured BEFORE we elevate so the answer
# survives the UAC relaunch (we only forward -DataDir when it was set). That is
# how the elevated instance knows whether to PROMPT for a location or not.
$DataDirExplicit = $PSBoundParameters.ContainsKey('DataDir')
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

# --- DigiByte data-directory detection + validation -------------------------
# The snapshot tar contains blocks\ + chainstate\ (+ indexes\) and unpacks them
# STRAIGHT into the target folder. So the target MUST be a DigiByte Core data
# directory - point it at the wrong place and you scatter tens of GB of chain
# data into some random folder. These helpers pick a sane default and sanity-
# check whatever the user picks before we commit to downloading/extracting.

# Look for tell-tale signs that $dir is (or is meant to be) a DigiByte data dir.
# Returns .Looks ($true when at least one signal is present) plus the reasons.
function Test-DigiByteDataDir($dir) {
    $reasons = New-Object System.Collections.Generic.List[string]
    $hasChain = $false
    if (Test-Path -LiteralPath $dir) {
        if (Test-Path (Join-Path $dir 'blocks'))        { [void]$reasons.Add('has blocks\');      $hasChain = $true }
        if (Test-Path (Join-Path $dir 'chainstate'))    { [void]$reasons.Add('has chainstate\');  $hasChain = $true }
        if (Test-Path (Join-Path $dir 'digibyte.conf')) { [void]$reasons.Add('has digibyte.conf') }
        if (Test-Path (Join-Path $dir 'wallets'))       { [void]$reasons.Add('has wallets\') }
        if (Test-Path (Join-Path $dir 'wallet.dat'))    { [void]$reasons.Add('has wallet.dat') }
        foreach ($f in 'peers.dat','banlist.dat','mempool.dat','.lock','debug.log','settings.json') {
            if (Test-Path (Join-Path $dir $f)) { [void]$reasons.Add("has $f") }
        }
        # The fork layout keeps digibyte.conf in the PARENT (C:\DigiByte) with the
        # blockchain in C:\DigiByte\data, so a config one level up counts too.
        $parent = Split-Path $dir -Parent
        if ($parent -and (Test-Path (Join-Path $parent 'digibyte.conf'))) { [void]$reasons.Add('parent has digibyte.conf') }
    }
    # Weak signal: an as-yet-unused datadir (DigiByte never launched) is empty,
    # but a path literally named ...\DigiByte is still a strong hint of intent.
    if ($dir -match '(?i)(^|[\\/])digibyte(\b|[\\/]|$)') { [void]$reasons.Add("path is named 'DigiByte'") }
    [pscustomobject]@{ Dir=$dir; Exists=(Test-Path -LiteralPath $dir); Looks=($reasons.Count -gt 0); HasChain=$hasChain; Reasons=$reasons }
}

# Pick the most likely existing datadir to offer as the prompt default.
function Find-DigiByteDataDir {
    foreach ($c in @((Join-Path $env:APPDATA 'DigiByte'), 'C:\DigiByte\data', 'C:\DigiByte\Data')) {
        if ((Test-DigiByteDataDir $c).Looks) { return $c }
    }
    Join-Path $env:APPDATA 'DigiByte'   # stock DigiByte Core default
}

# Elevate (writing into the datadir / stopping DigiByte may need admin).
$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) {
    if ($PSCommandPath) {
        # Only forward -DataDir when the user actually set it, so the elevated
        # instance still knows to PROMPT for a location when they didn't.
        $fwd = "-ExecutionPolicy Bypass -File `"$PSCommandPath`" -SnapshotUrl `"$SnapshotUrl`""
        if ($DataDirExplicit) { $fwd += " -DataDir `"$DataDir`"" }
        if ($Force)           { $fwd += " -Force" }
        Start-Process powershell.exe -Verb RunAs -ArgumentList $fwd; return
    }
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
    # Strip a UTF-8 BOM whether it decoded as U+FEFF or as mojibake bytes (ï»¿).
    $txt = $txt.TrimStart([char]0xFEFF, [char]0xEF, [char]0xBB, [char]0xBF)
    $m = $txt | ConvertFrom-Json
} catch { throw "Could not fetch/parse manifest ($SnapshotUrl): $($_.Exception.Message)" }
# Defensive: re-parse digibyte if an old/corrupt manifest carried it as a string.
if ($m -and $m.digibyte -is [string]) {
    try { $m.digibyte = (($m.digibyte).TrimStart([char]0xFEFF, [char]0xEF, [char]0xBB, [char]0xBF) | ConvertFrom-Json) } catch {}
}
if (-not $m -or -not $m.digibyte -or -not $m.baseUrl) { throw 'Manifest is missing digibyte / baseUrl.' }
$base = ("$($m.baseUrl)").TrimEnd('/')
$url  = "$base/$($m.digibyte.file)"
$szGB = if ($m.digibyte.sizeBytes) { [double]$m.digibyte.sizeBytes / 1GB } else { 0 }
Say ("Snapshot: {0}   height {1:N0}   DigiByte {2}{3}" -f `
        $m.digibyte.file,[int]$m.digibyte.height,$m.digibyte.version,`
        $(if ($szGB) { "   ~{0:N1} GB download" -f $szGB } else { '' })) 'White'

# --- Choose + validate the target data directory ---------------------------
# The tar unpacks blocks\ + chainstate\ straight into $DataDir, so pointing it
# at the wrong folder scatters the chain there. Prompt (unless -DataDir was
# given), sanity-check the choice, and require an explicit override when it does
# not look like a DigiByte data directory.
if (-not $DataDirExplicit -and -not $Force) {
    $suggested = Find-DigiByteDataDir
    Say ''
    Say 'Where should the DigiByte blockchain data be placed?' 'Cyan'
    Say 'This is your DigiByte Core DATA directory - the folder that holds blocks\ and chainstate\:' 'Gray'
    Say "    Stock DigiByte Core:    $env:APPDATA\DigiByte" 'Gray'
    Say '    DigiAsset for Windows:  C:\DigiByte\data' 'Gray'
    while ($true) {
        $ans = Read-Host "`nData directory [$suggested]"
        if ([string]::IsNullOrWhiteSpace($ans)) { $ans = $suggested }
        $ans = [System.Environment]::ExpandEnvironmentVariables($ans.Trim().Trim('"'))
        $chk = Test-DigiByteDataDir $ans
        if ($chk.Looks) {
            Say ("  OK - looks like a DigiByte data directory ({0})." -f ($chk.Reasons -join '; ')) 'Green'
            $DataDir = $ans; break
        }
        Say ''
        Say "  '$ans' does NOT look like a DigiByte data directory." 'Yellow'
        Say '  Found none of: blocks\, chainstate\, digibyte.conf, a wallet, or "DigiByte" in the path.' 'Yellow'
        Say '  Extracting here would drop blocks\ + chainstate\ into that exact folder.' 'Yellow'
        $ov = Read-Host '  [O]verride and use it anyway / [R]e-enter a path / [C]ancel'
        if     ($ov -match '^[Oo]') { $DataDir = $ans; Say "  Overriding - using $ans" 'Yellow'; break }
        elseif ($ov -match '^[Cc]') { Say 'Cancelled.'; return }
        # anything else: loop and re-enter
    }
} else {
    # -DataDir (or -Force) given: still validate, but only ask once (and not at
    # all under -Force) so scripted/unattended runs do not hang on a prompt.
    $chk = Test-DigiByteDataDir $DataDir
    if (-not $chk.Looks) {
        Say ''
        Say "WARNING: '$DataDir' does not look like a DigiByte data directory" 'Yellow'
        Say '  (no blocks\, chainstate\, digibyte.conf, wallet, or "DigiByte" in the path).' 'Yellow'
        if ($Force) { Say '  -Force set: continuing anyway.' 'Yellow' }
        elseif ((Read-Host '  Extract here anyway? (y/N)') -notmatch '^[Yy]') { Say 'Cancelled.'; return }
    }
}
Say "Target:   $DataDir" 'White'

if (Test-Path (Join-Path $DataDir 'blocks')) {
    Say "`nThis folder already has a blockchain (blocks\). The snapshot will overwrite matching files." 'Yellow'
    if (-not $Force -and ((Read-Host 'Continue? (y/N)') -notmatch '^[Yy]')) { Say 'Cancelled.'; return }
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

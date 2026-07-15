<#
.SYNOPSIS
    One command to refresh the fast-sync snapshot: build the DigiByte blockchain +
    DigiAsset chain.db archives, upload them to Cloudflare R2, rebuild + upload
    snapshot.json, verify it's live, and prune old local copies. Can register
    itself as a WEEKLY scheduled task.

.DESCRIPTION
    Run on the always-on box that has BOTH a fully-synced DigiByte wallet and a
    fully-synced DigiAsset node. It stops them briefly for a clean snapshot, then
    restarts them (make-snapshot handles that). Requires rclone installed with a
    configured remote (default name 'r2') pointing at your R2 account.

    Paths (the standard layout): DigiByte in C:\DigiByte (data in C:\DigiByte\Data),
    node in C:\DigiAssetWindows.

.EXAMPLE
    # run once now
    powershell -ExecutionPolicy Bypass -File .\publish-snapshot.ps1

    # register a weekly job (Sundays 03:00) - runs while you're logged on (Autologon)
    powershell -ExecutionPolicy Bypass -File .\publish-snapshot.ps1 -Schedule

    # also delete superseded archives from R2 so the bucket doesn't grow forever
    powershell -ExecutionPolicy Bypass -File .\publish-snapshot.ps1 -PruneRemote
#>
[CmdletBinding()]
param(
    [string]$BaseUrl      = 'https://pub-bd3f441e6b464d499ba583016accfa01.r2.dev',
    [string]$RcloneRemote = 'r2',
    [string]$Bucket       = 'digibyte-snapshots',
    [ValidateSet('both','digibyte','chaindb')][string]$Component = 'both',
    [string]$DigiByteDir  = 'C:\DigiByte',
    [string]$DigiAssetDir = 'C:\DigiAssetWindows',
    [string]$DataDir      = '',
    [int]   $Height       = 0,        # stamp both archives with this height (for a data-copy box with nothing running)
    [string]$OutDir       = 'C:\DigiAssetSnapshots',
    [int]   $KeepLocal    = 1,        # keep newest N local .tar.gz of each kind
    [switch]$PruneRemote,             # delete superseded archives from R2 too
    [switch]$NoManifest,              # upload the archive(s) only; do NOT rebuild/replace snapshot.json
    [switch]$Schedule,                # register a weekly task instead of running now
    [string]$ScheduleDay  = 'Sunday',
    [string]$ScheduleTime = '03:00'
)
$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$SCRIPT_VERSION = '1.2.0'
function Say($m,$c='Gray'){ Write-Host $m -ForegroundColor $c }
function Step($n,$m){ Write-Host ''; Write-Host "[$n] $m" -ForegroundColor Cyan }
$here     = if ($PSScriptRoot) { $PSScriptRoot } else { Split-Path -Parent $MyInvocation.MyCommand.Path }

# Load defaults saved by setup-cloudflare-snapshots.ps1 (remote/bucket/publicUrl)
# so this script runs with NO arguments after a one-time setup. Anything passed on
# the command line still wins.
$cfgFile = Join-Path $here 'snapshot-config.json'
if (Test-Path $cfgFile) {
    try {
        $sc = Get-Content $cfgFile -Raw | ConvertFrom-Json
        if (-not $PSBoundParameters.ContainsKey('RcloneRemote') -and $sc.remote)    { $RcloneRemote = $sc.remote }
        if (-not $PSBoundParameters.ContainsKey('Bucket')       -and $sc.bucket)    { $Bucket       = $sc.bucket }
        if (-not $PSBoundParameters.ContainsKey('BaseUrl')      -and $sc.publicUrl) { $BaseUrl      = $sc.publicUrl }
    } catch { Say "  (couldn't read $cfgFile - using defaults)" 'Yellow' }
}
$makeSnap = Join-Path $here 'make-snapshot.ps1'
$remote   = "${RcloneRemote}:${Bucket}"

# --- Elevate (make-snapshot stops/starts services; scheduling needs admin) ---
$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) {
    if ($PSCommandPath) {
        Say 'Snapshot publish needs Administrator - approve the UAC prompt...' 'Yellow'
        $fwd = "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" -BaseUrl `"$BaseUrl`" -RcloneRemote `"$RcloneRemote`" -Bucket `"$Bucket`" -Component $Component -DigiByteDir `"$DigiByteDir`" -DigiAssetDir `"$DigiAssetDir`" -DataDir `"$DataDir`" -Height $Height -OutDir `"$OutDir`" -KeepLocal $KeepLocal"
        if ($PruneRemote) { $fwd += ' -PruneRemote' }
        if ($NoManifest)  { $fwd += ' -NoManifest' }
        if ($Schedule)    { $fwd += " -Schedule -ScheduleDay $ScheduleDay -ScheduleTime $ScheduleTime" }
        Start-Process powershell.exe -Verb RunAs -ArgumentList $fwd; return
    } else { throw 'Run this in an elevated (Administrator) PowerShell.' }
}

# --- Schedule mode: register the weekly task and exit ------------------------
if ($Schedule) {
    $selfArg = "-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File `"$PSCommandPath`" -BaseUrl `"$BaseUrl`" -RcloneRemote `"$RcloneRemote`" -Bucket `"$Bucket`" -Component $Component -DigiByteDir `"$DigiByteDir`" -DigiAssetDir `"$DigiAssetDir`" -OutDir `"$OutDir`" -KeepLocal $KeepLocal"
    if ($PruneRemote) { $selfArg += ' -PruneRemote' }
    $a = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument $selfArg
    $t = New-ScheduledTaskTrigger -Weekly -DaysOfWeek $ScheduleDay -At $ScheduleTime
    $u = [Security.Principal.WindowsIdentity]::GetCurrent().Name
    $p = New-ScheduledTaskPrincipal -UserId $u -LogonType Interactive -RunLevel Highest
    $s = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -StartWhenAvailable -ExecutionTimeLimit (New-TimeSpan -Hours 6)
    Register-ScheduledTask -TaskName 'DigiAssetSnapshotPublish' -Action $a -Trigger $t -Principal $p -Settings $s -Force | Out-Null
    Say "Weekly snapshot task 'DigiAssetSnapshotPublish' registered: $ScheduleDay at $ScheduleTime." 'Green'
    Say "Runs as $u while logged on (use Autologon on an always-on box)." 'White'
    Say "It snapshots both DBs, uploads to $remote, refreshes snapshot.json, and verifies." 'White'
    return
}

# --- Preconditions -----------------------------------------------------------
if (-not (Get-Command rclone -ErrorAction SilentlyContinue)) { throw "rclone not found. Run setup-cloudflare-snapshots.ps1 once to install + configure it: .\setup-cloudflare-snapshots.ps1" }
# Verify the remote itself exists, not just the binary - otherwise the failure
# only surfaces AFTER archives are built (services stopped/restarted, GBs written).
$remotes = @(& rclone listremotes 2>$null)
if ($remotes -notcontains "${RcloneRemote}:") { throw "rclone remote '${RcloneRemote}:' is not configured. Run the one-time setup first: .\setup-cloudflare-snapshots.ps1 (it installs rclone + configures your R2 remote from your Cloudflare keys)." }
if (-not (Test-Path $makeSnap)) { throw "make-snapshot.ps1 not found next to this script ($makeSnap)." }
$flags = @('--s3-no-check-bucket','--s3-chunk-size','128M','--s3-upload-concurrency','8','--s3-disable-checksum','--progress')

Say '==============================================================' 'Cyan'
Say " Publish fast-sync snapshot ($Component) -> $remote   (v$SCRIPT_VERSION)" 'Cyan'
Say '==============================================================' 'Cyan'

# --- 1. Build the archives + part files (NO manifest here) -------------------
# Step 1 builds ARCHIVES ONLY in NON-INTERACTIVE mode. This is the fix for the
# weekly-task hang: the old code built the manifest here too, which prompts for
# the R2 URL via Read-Host and blocked the hidden scheduled run until timeout.
# The manifest is built once, correctly, in Step 3 (which passes -BaseUrl).
$step1Component = if ($Component -eq 'both') { 'archives' } else { $Component }
Step 1 'Building snapshot archives (stops + restarts DigiByte / the node)'
# Build the argument list and only add -DataDir when it's set. Passing an EMPTY
# -DataDir through `powershell.exe -File` drops the empty token, so make-snapshot
# saw "-DataDir" with no value and failed ("Missing an argument for parameter").
$snapArgs = @('-NoProfile','-ExecutionPolicy','Bypass','-File',$makeSnap,
              '-Component',$step1Component,'-NonInteractive',
              '-DigiByteDir',$DigiByteDir,'-DigiAssetDir',$DigiAssetDir,'-OutDir',$OutDir)
if ($DataDir) { $snapArgs += @('-DataDir',$DataDir) }
if ($Height -gt 0) { $snapArgs += @('-Height', "$Height") }
& powershell.exe @snapArgs
if ($LASTEXITCODE -ne 0) { throw "make-snapshot failed (exit $LASTEXITCODE)." }

# --- 2. Upload archives + part files -----------------------------------------
Step 2 "Uploading archives to $remote"
& rclone copy $OutDir "$remote/" --include "*.tar.gz" --include "*-part.json" @flags
if ($LASTEXITCODE -ne 0) { throw "rclone upload of archives failed (exit $LASTEXITCODE)." }

# --- 2b. Verify the uploaded archive bytes match locally ---------------------
# --s3-disable-checksum means rclone doesn't hash-verify multipart uploads, so a
# truncated/partial upload could otherwise be published and break fast-sync for
# EVERY new node. Compare each uploaded archive's size on R2 to the local part
# file's recorded size; abort before publishing the manifest on any mismatch.
Step '2b' 'Verifying uploaded archive integrity on R2'
foreach ($partName in 'digibyte-part.json','chaindb-part.json') {
    $pf = Join-Path $OutDir $partName
    if (-not (Test-Path $pf)) { continue }   # that component wasn't built this run
    $part = Get-Content $pf -Raw | ConvertFrom-Json
    $file = $part.file; $expect = [int64]$part.sizeBytes
    $info = @(& rclone lsjson "$remote/" --include $file --no-modtime 2>$null | ConvertFrom-Json)
    $got  = if ($info.Count -ge 1) { [int64]$info[0].Size } else { -1 }
    if ($got -ne $expect) {
        throw "Upload integrity check FAILED for $file : R2 has $got bytes, expected $expect. NOT publishing snapshot.json (new nodes would get a corrupt/partial file). Re-run to retry the upload."
    }
    Say "  OK  $file  ($('{0:N0}' -f $got) bytes match)" 'Green'
}

# --- 3. Rebuild + upload snapshot.json ---------------------------------------
# -NoManifest: the archive(s) are now on R2, but we deliberately DON'T rebuild
# snapshot.json. Use this to stage a new chain.db without making it live, then
# publish a height-consistent pair later with -Component both.
$cur = $null
if ($NoManifest) {
    Write-Host ''
    Say '[3] Skipping snapshot.json (-NoManifest).' 'Yellow'
    Say '    The archive(s) are uploaded, but the LIVE manifest still points at the previous parts.' 'Yellow'
    Say '    Publish a consistent pair later with:  .\publish-snapshot.ps1 -Component both' 'Yellow'
} else {
    Step 3 'Rebuilding + uploading snapshot.json'
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $makeSnap -Component manifest -NonInteractive -BaseUrl $BaseUrl -OutDir $OutDir
    $manifest = Join-Path $OutDir 'snapshot.json'
    if (-not (Test-Path $manifest)) { throw 'snapshot.json was not produced.' }
    & rclone copyto $manifest "$remote/snapshot.json" --s3-no-check-bucket
    if ($LASTEXITCODE -ne 0) { throw "rclone upload of snapshot.json failed (exit $LASTEXITCODE)." }

    # --- 4. Verify it's live -------------------------------------------------
    Step 4 'Verifying the live manifest'
    try {
        $resp = Invoke-WebRequest ("$($BaseUrl.TrimEnd('/'))/snapshot.json") -UseBasicParsing -TimeoutSec 30
        $txt = $resp.Content; if ($txt -is [byte[]]) { $txt = [Text.Encoding]::UTF8.GetString($txt) }
        $txt = $txt.TrimStart([char]0xFEFF, [char]0xEF, [char]0xBB, [char]0xBF)   # strip BOM (U+FEFF or mojibake)
        $cur = $txt | ConvertFrom-Json
        Say ("  live: DigiByte height {0:N0} ({1}), chain.db height {2:N0}" -f [int]$cur.digibyte.height, $cur.digibyte.file, [int]$cur.chaindb.height) 'Green'
    } catch { Say "  WARNING: could not verify snapshot.json: $($_.Exception.Message)" 'Yellow' }
}

# --- 5. Prune old local archives ---------------------------------------------
Step 5 "Pruning old local archives (keeping newest $KeepLocal of each kind)"
foreach ($pat in 'digibyte*.tar.gz','digiasset-chaindb*.tar.gz') {
    Get-ChildItem (Join-Path $OutDir $pat) -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending |
        Select-Object -Skip $KeepLocal | ForEach-Object { Remove-Item $_.FullName -Force -ErrorAction SilentlyContinue; Say "  - removed $($_.Name)" 'Gray' }
}

# --- 6. Prune superseded archives from R2 (opt-in) ---------------------------
if ($PruneRemote -and $cur) {
    Step 6 "Pruning superseded archives from $remote"
    $keep = @($cur.digibyte.file, $cur.chaindb.file, 'snapshot.json', 'digibyte-part.json', 'chaindb-part.json')
    $listing = (& rclone lsf "$remote/" 2>$null)
    foreach ($f in $listing) {
        $name = $f.Trim().TrimEnd('/')
        if ($name -like '*.tar.gz' -and ($keep -notcontains $name)) {
            & rclone deletefile "$remote/$name" --s3-no-check-bucket 2>$null
            Say "  - deleted $name from R2" 'Gray'
        }
    }
}

Write-Host ''
Say '===================== SNAPSHOT PUBLISHED =====================' 'Green'
Say " Nodes will now fast-sync from the refreshed $remote/snapshot.json" 'White'

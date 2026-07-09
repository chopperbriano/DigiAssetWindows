<#
.SYNOPSIS
  Benchmark DigiAsset node sync speed over a fixed asset-era block window, with
  pipelinesync on or off. Correctness gate: assetCount at the end height MUST
  match between the two runs.

.DESCRIPTION
  RUN AS ADMINISTRATOR (needed to stop a node instance the SYSTEM supervisor may
  have launched - a user-context Stop-Process can't kill it, which is how a
  zombie node ends up being measured).

  Efficient workflow (sync the slow header era only ONCE):
    1) .\bench-sync.ps1 -Prepare        # fresh-sync up to the asset era, back up chain.db (~hours, one time)
    2) .\bench-sync.ps1 -Pipeline 0      # restore backup, measure the window (minutes)
    3) .\bench-sync.ps1 -Pipeline 1      # restore backup, measure the window (minutes)
  Then compare blocks/sec, and confirm the two assetCount values match.

  Without a backup, a measured run falls back to wiping chain.db and syncing from
  scratch (it warns, and re-syncs the whole header era each time - slow).

  Every run: disables the DigiStampNode + DigiStampMaintenance supervisors so they
  can't relaunch a second node mid-run, locks polling to the PID it started (so it
  can never read a stray instance's height), and re-enables the supervisors when
  finished.

  PREREQUISITES (left running, not re-synced): DigiByte Core fully synced +
  running, IPFS running, and config.cfg has verifydatabasewrite=0.
#>
param(
  [ValidateSet(0,1)][int]$Pipeline = 0,
  [switch]$Prepare,
  [string]$Dir = 'C:\DigiAssetWindows',
  [int]$StartHeight = 8432316,   # DigiAsset activation - where asset processing begins
  [int]$Blocks = 25000,          # size of the measured window
  [int]$PollSeconds = 10,
  [string]$BackupPath = '',
  [int]$AssetPort = 0             # DigiAsset node RPC port the CLI talks to (0 = auto: rpcassetport or 14024)
)
$ErrorActionPreference = 'Stop'
if (-not $BackupPath) { $BackupPath = Join-Path $Dir 'chain_bench_backup.db' }
$cfg = Join-Path $Dir 'config.cfg'
$exe = Join-Path $Dir 'DigiAssetWindows.exe'
$cli = Join-Path $Dir 'DigiAssetWindows-cli.exe'
$db  = Join-Path $Dir 'chain.db'
$endHeight = $StartHeight + $Blocks
$supervisors = @('DigiStampNode','DigiStampMaintenance')

foreach ($p in @($cfg,$exe,$cli)) { if (-not (Test-Path $p)) { throw "missing: $p" } }

# Resolve the DigiAsset RPC port the CLI connects to (rpcassetport, default 14024).
# The CLI reads its height from whatever OWNS this port - so the benchmark must
# make sure that owner is the node WE started, not a stray old-named instance.
if ($AssetPort -le 0) {
  $AssetPort = 14024
  $pm = [regex]::Match(((Get-Content $cfg) -join "`n"), '(?m)^\s*rpcassetport\s*=\s*(\d+)')
  if ($pm.Success) { $AssetPort = [int]$pm.Groups[1].Value }
}
Write-Host "  (DigiAsset RPC port = $AssetPort)"

# Must be admin - otherwise Stop-Process can't kill a SYSTEM-owned node instance,
# and the benchmark ends up reading a zombie node stuck at a low height.
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltinRole]::Administrator)
if (-not $isAdmin) { throw "Run this from an ADMINISTRATOR PowerShell (needed to stop SYSTEM-owned node instances)." }

function Get-Stat($name) {
  try {
    $j = & $cli getnodestats 2>$null | Out-String
    $m = [regex]::Match($j, "`"$name`"\s*:\s*(\d+)")
    if ($m.Success) { return [int]$m.Groups[1].Value }
    return -1
  } catch { return -1 }
}
# All node exe processes - matches the current name AND older names (e.g. a
# leftover DigiAssetCore.exe from before the rename), but NOT the short-lived
# *-cli helper. A stray old-named node holding the RPC port is exactly what made
# the benchmark read a zombie height.
function Get-NodeProcs {
  Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.Name -like '*DigiAsset*' -and $_.Name -notlike '*cli*' }
}
# PID(s) currently LISTENING on the DigiAsset RPC port. This is what the CLI (and
# therefore the height reading) actually talks to - by port, not by process name,
# so it catches a stray of ANY name.
function Get-PortOwnerPids {
  @(Get-NetTCPConnection -LocalPort $AssetPort -State Listen -ErrorAction SilentlyContinue | Select-Object -ExpandProperty OwningProcess -Unique)
}
function Describe-Pid($procId) {
  $p = Get-Process -Id $procId -ErrorAction SilentlyContinue
  if ($p) { return ("pid $procId ($($p.Name)) $($p.Path)") } else { return "pid $procId (gone)" }
}
function Stop-AllNodes {
  # Clean shutdown FIRST. Force-killing a node whose SQLite runs journal_mode=MEMORY
  # (the fast-sync pragma) can TEAR chain.db - there's no on-disk journal to replay.
  # In -Prepare that torn db then gets backed up and restored by the measured runs,
  # producing "Table creation failed". So ask the node to stop via its CLI, wait up
  # to 30s, and only force-kill as a last resort.
  if (Get-NodeProcs) {
    if (Test-Path $cli) { try { Push-Location $Dir; & $cli shutdown 2>$null | Out-Null; Pop-Location } catch { try { Pop-Location } catch {} } }
    for ($i=0; $i -lt 60 -and (Get-NodeProcs); $i++) { Start-Sleep -Milliseconds 500 }
    if (Get-NodeProcs) {
      Write-Host "  node did not stop cleanly within 30s - force-killing (chain.db may be left inconsistent)" -ForegroundColor Yellow
      Get-NodeProcs | Stop-Process -Force -ErrorAction SilentlyContinue
    }
  }
  # Also clear anything squatting on the RPC port (a stray of any name).
  foreach ($op in Get-PortOwnerPids) { Stop-Process -Id $op -Force -ErrorAction SilentlyContinue }
  Start-Sleep -Seconds 3
  $left = @(Get-NodeProcs) + @(Get-PortOwnerPids | ForEach-Object { Get-Process -Id $_ -ErrorAction SilentlyContinue })
  $left = $left | Where-Object { $_ } | Select-Object -Unique
  if ($left) { throw ("could not stop all node/RPC-port instances: " + (($left | ForEach-Object { Describe-Pid $_.Id }) -join '; ') + ". Are you elevated?") }
}
# Verify the node WE started is the one the CLI reads. If a different PID owns the
# RPC port, name it loudly (this is the stray that was faking the stuck height).
function Assert-OwnsPort($ourPid) {
  $owners = Get-PortOwnerPids
  if (-not $owners) { return }  # RPC not up yet
  if ($owners -notcontains $ourPid) {
    throw ("RPC port $AssetPort is owned by " + (($owners | ForEach-Object { Describe-Pid $_ }) -join '; ') +
           " - NOT the node we started (pid $ourPid). That stray is what the CLI/bench was reading. Kill it and re-run.")
  }
}
function Set-Flag($v) {
  $lines = Get-Content $cfg
  if ($lines -match '^\s*pipelinesync\s*=') {
    ($lines -replace '^\s*pipelinesync\s*=.*', "pipelinesync=$v") | Set-Content $cfg -Encoding ASCII
  } else {
    Add-Content $cfg "pipelinesync=$v"
  }
}
function Start-NodeLocked {
  $proc = Start-Process -FilePath $exe -WorkingDirectory $Dir -PassThru
  return $proc.Id
}
function Assert-Single($ourPid) {
  $extra = Get-NodeProcs | Where-Object { $_.Id -ne $ourPid }
  if ($extra) { throw ("a SECOND node instance appeared (pid " + ($extra.Id -join ',') + "). A supervisor or a stray old-named node - aborting so we don't measure the wrong one.") }
  if (-not (Get-Process -Id $ourPid -ErrorAction SilentlyContinue)) { throw "the node (pid $ourPid) exited unexpectedly - check its window/log." }
}

# Disable the auto-restart supervisors for the duration; re-enabled in finally.
foreach ($tn in $supervisors) {
  try { Disable-ScheduledTask -TaskName $tn -ErrorAction Stop | Out-Null; Write-Host "  disabled supervisor task: $tn" } catch {}
}
try {
  Stop-AllNodes

  if ($Prepare) {
    Write-Host "=== PREPARE: fresh-sync to height $StartHeight, then back up chain.db ===" -ForegroundColor Cyan
    if (Test-Path $db) { Remove-Item $db -Force }
    Set-Flag 0
    $ourPid = Start-NodeLocked
    Write-Host "  node started (pid $ourPid); syncing header era to $StartHeight ..."
    while ($true) {
      Start-Sleep -Seconds $PollSeconds
      Assert-Single $ourPid
      Assert-OwnsPort $ourPid
      $h = Get-Stat 'syncHeight'
      $owner = (Get-PortOwnerPids) -join ','
      if ($h -lt 0) { Write-Host "  (waiting for RPC... rpc-port owner=[$owner])"; continue }
      Write-Host "  height $h   [rpc-port $AssetPort owner=$owner, ours=$ourPid]"
      if ($h -ge $StartHeight) { break }
    }
    Stop-AllNodes
    Copy-Item $db $BackupPath -Force
    Write-Host "  backup written: $BackupPath  (height ~$StartHeight)" -ForegroundColor Green
    Write-Host "  Next:  .\bench-sync.ps1 -Pipeline 0   then   .\bench-sync.ps1 -Pipeline 1" -ForegroundColor Yellow
    return
  }

  Write-Host "=== bench-sync: pipelinesync=$Pipeline, window $StartHeight..$endHeight ($Blocks blocks) ===" -ForegroundColor Cyan

  if (Test-Path $BackupPath) {
    Copy-Item $BackupPath $db -Force
    Write-Host "  restored chain.db from backup (skips the header-era re-sync)"
  } else {
    if (Test-Path $db) { Remove-Item $db -Force }
    Write-Host "  no backup found - wiping and syncing from scratch. This re-syncs the whole" -ForegroundColor Yellow
    Write-Host "  header era every run; run '.\bench-sync.ps1 -Prepare' once to avoid that." -ForegroundColor Yellow
  }

  Set-Flag $Pipeline
  Write-Host "  config.cfg: pipelinesync=$Pipeline"
  $ourPid = Start-NodeLocked
  Write-Host "  node started (pid $ourPid); waiting to reach height $StartHeight ..."

  $t0 = $null
  while ($true) {
    Start-Sleep -Seconds $PollSeconds
    Assert-Single $ourPid
    Assert-OwnsPort $ourPid
    $h = Get-Stat 'syncHeight'
    $owner = (Get-PortOwnerPids) -join ','
    if ($h -lt 0) { Write-Host "  (waiting for RPC... rpc-port owner=[$owner])"; continue }
    if (-not $t0) {
      if ($h -ge $StartHeight) { $t0 = Get-Date; Write-Host "  >> window START at height $h ($(Get-Date $t0 -Format HH:mm:ss))" -ForegroundColor Green }
      else { Write-Host "  sync... height $h   [rpc-port $AssetPort owner=$owner, ours=$ourPid]" }
    } else {
      $rate = [math]::Round(($h - $StartHeight) / ((Get-Date) - $t0).TotalSeconds, 1)
      Write-Host "  height $h  (~$rate blk/s so far)   [owner=$owner]"
      if ($h -ge $endHeight) {
        $secs = ((Get-Date) - $t0).TotalSeconds
        $bps  = [math]::Round($Blocks / $secs, 1)
        $ac   = Get-Stat 'assetCount'
        Write-Host ""
        Write-Host "  RESULT (pipelinesync=$Pipeline):" -ForegroundColor Yellow
        Write-Host ("    {0} blocks in {1:N0}s  =  {2} blocks/sec" -f $Blocks, $secs, $bps) -ForegroundColor Yellow
        Write-Host ("    assetCount at height {0} = {1}   (must match the other run)" -f $h, $ac) -ForegroundColor Yellow
        break
      }
    }
  }

  Stop-AllNodes
  Write-Host "  node stopped. Done."
}
finally {
  foreach ($tn in $supervisors) {
    try { Enable-ScheduledTask -TaskName $tn -ErrorAction Stop | Out-Null } catch {}
  }
  Write-Host "  (supervisors re-enabled; the node will auto-start normally again)"
}

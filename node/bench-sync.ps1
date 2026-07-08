<#
.SYNOPSIS
  Benchmark DigiAsset node initial sync speed from scratch, with pipelinesync
  on or off, over a fixed asset-era block window. Run it twice (-Pipeline 0,
  then -Pipeline 1) and compare blocks/sec. Correctness gate: assetCount at the
  end height MUST match between the two runs.

.DESCRIPTION
  Each run: stops the node, DELETES chain.db (fresh sync), sets pipelinesync in
  config.cfg, starts the node, and times how long it takes to advance from
  -StartHeight to -StartHeight+-Blocks (the asset-era region where the pipeline
  engages; the pre-asset header era before StartHeight is not counted). Prints
  blocks/sec and the assetCount fingerprint, then stops the node.

  PREREQUISITES (leave these running — they are NOT re-synced):
    * DigiByte Core fully synced and running (the node reads blocks from it).
    * IPFS Desktop / kubo running (the node needs the IPFS API to start).
    * config.cfg has verifydatabasewrite=0 (fast pragmas).

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File .\bench-sync.ps1 -Pipeline 0
  powershell -ExecutionPolicy Bypass -File .\bench-sync.ps1 -Pipeline 1
#>
param(
  [ValidateSet(0,1)][int]$Pipeline = 0,
  [string]$Dir = 'C:\DigiAssetWindows',
  [int]$StartHeight = 8432316,   # DigiAsset activation — where asset processing begins
  [int]$Blocks = 25000,          # size of the measured window
  [int]$PollSeconds = 10
)
$ErrorActionPreference = 'Stop'
$cfg = Join-Path $Dir 'config.cfg'
$exe = Join-Path $Dir 'DigiAssetWindows.exe'
$cli = Join-Path $Dir 'DigiAssetWindows-cli.exe'
$db  = Join-Path $Dir 'chain.db'
$endHeight = $StartHeight + $Blocks

foreach ($p in @($cfg,$exe,$cli)) { if (-not (Test-Path $p)) { throw "missing: $p" } }

function Get-SyncHeight {
  try {
    $j = & $cli getnodestats 2>$null | Out-String
    return [int]([regex]::Match($j, '"syncHeight"\s*:\s*(\d+)').Groups[1].Value)
  } catch { return -1 }
}
function Get-AssetCount {
  try {
    $j = & $cli getnodestats 2>$null | Out-String
    return [int]([regex]::Match($j, '"assetCount"\s*:\s*(\d+)').Groups[1].Value)
  } catch { return -1 }
}

Write-Host "=== bench-sync: pipelinesync=$Pipeline, window $StartHeight..$endHeight ($Blocks blocks) ===" -ForegroundColor Cyan

# 0. Disable the auto-restart supervisors, or they relaunch the node mid-bench
#    and TWO instances collide on the exclusive-locked chain.db -> the sync
#    stalls at a low height. Re-enabled in the finally block below.
$supervisors = @('DigiStampNode','DigiStampMaintenance')
foreach ($tn in $supervisors) {
  try { Disable-ScheduledTask -TaskName $tn -ErrorAction Stop | Out-Null; Write-Host "  disabled supervisor task: $tn" } catch {}
}
try {

# 1. Stop ALL node instances + wipe chain.db for a true from-scratch sync.
Get-Process DigiAssetWindows -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 3
if ((Get-Process DigiAssetWindows -ErrorAction SilentlyContinue)) {
  throw "a DigiAssetWindows.exe is still running after stop - kill it before benchmarking (check for a supervisor relaunch)"
}
if (Test-Path $db) { Remove-Item $db -Force }
Write-Host "  chain.db wiped (fresh sync)"

# 2. Set pipelinesync in config.cfg (replace existing line or append).
$lines = Get-Content $cfg
if ($lines -match '^\s*pipelinesync\s*=') {
  ($lines -replace '^\s*pipelinesync\s*=.*', "pipelinesync=$Pipeline") | Set-Content $cfg -Encoding ASCII
} else {
  Add-Content $cfg "pipelinesync=$Pipeline"
}
Write-Host "  config.cfg: pipelinesync=$Pipeline"

# 3. Start the node.
Start-Process -FilePath $exe -WorkingDirectory $Dir
Write-Host "  node started; waiting for it to reach height $StartHeight ..."

# 4. Poll until the window start, then time to the window end.
$t0 = $null
while ($true) {
  Start-Sleep -Seconds $PollSeconds
  $h = Get-SyncHeight
  if ($h -lt 0) { Write-Host "  (waiting for RPC...)"; continue }
  if (-not $t0) {
    if ($h -ge $StartHeight) { $t0 = Get-Date; Write-Host "  >> window START at height $h ($(Get-Date $t0 -Format HH:mm:ss))" -ForegroundColor Green }
    else { Write-Host "  header sync... height $h" }
  } else {
    $rate = [math]::Round(($h - $StartHeight) / ((Get-Date) - $t0).TotalSeconds, 1)
    Write-Host "  height $h  (~$rate blk/s so far)"
    if ($h -ge $endHeight) {
      $secs = ((Get-Date) - $t0).TotalSeconds
      $bps  = [math]::Round($Blocks / $secs, 1)
      $ac   = Get-AssetCount
      Write-Host ""
      Write-Host "  RESULT (pipelinesync=$Pipeline):" -ForegroundColor Yellow
      Write-Host ("    {0} blocks in {1:N0}s  =  {2} blocks/sec" -f $Blocks, $secs, $bps) -ForegroundColor Yellow
      Write-Host ("    assetCount at height {0} = {1}   <-- must match the other run" -f $h, $ac) -ForegroundColor Yellow
      break
    }
  }
}

# 5. Stop the node.
Get-Process DigiAssetWindows -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Write-Host "  node stopped. Done."

}
finally {
  # Re-enable the supervisors so the node auto-starts again after benchmarking.
  foreach ($tn in $supervisors) {
    try { Enable-ScheduledTask -TaskName $tn -ErrorAction Stop | Out-Null; Write-Host "  re-enabled supervisor task: $tn" } catch {}
  }
  Write-Host "  (supervisors restored; the node will auto-start normally again)"
}

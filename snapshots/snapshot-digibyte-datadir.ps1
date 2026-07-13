<#
.SYNOPSIS
    Provision DigiByte nodes across your OWN fleet by copying a fully-indexed data
    directory - so a new node NEVER re-syncs or re-indexes the 23M-block chain.

    Two modes:
      -Mode Snapshot   On ONE healthy, fully-synced+indexed box: cleanly stop
                       DigiByte, archive blocks\ + chainstate\ + indexes\ into a
                       .tar.gz (+ .sha256), restart the node.
      -Mode Restore    On a target box: verify the archive's SHA256, stop DigiByte,
                       move any existing chain data aside, extract the archive into
                       the data dir. Start DigiByte; it verifies and syncs only the
                       small delta since the snapshot. No reindex, no full re-sync.

.DESCRIPTION
    This is the LOCAL / LAN counterpart to the R2 snapshot tools:
      * make-snapshot.ps1 + publish-snapshot.ps1  -> distribute over the internet (R2)
      * seed-digibyte.ps1                          -> restore FROM the R2 feed
      * THIS script                                -> build once, copy directly to
                                                      other boxes (file or \\share),
                                                      no 33GB round-trip through R2.

    WALLET SAFETY: only blocks\, chainstate\, and indexes\ are ever archived. The
    wallets\ folder, digibyte.conf, .cookie, peers.dat and mempool.dat are NEVER
    included. Each node MUST keep its own wallet (its own keys + payout address);
    copying a wallet between nodes would share private keys and collide payouts.

    INDEXES CARRY OVER: whatever indexes the source has built (txindex, and
    optionally coinstatsindex / blockfilterindex / digidollarstatsindex) travel in
    indexes\, so restored nodes get them for free. The target's digibyte.conf must
    still ENABLE the matching toggles (txindex=1 etc.) or Core ignores/rebuilds
    them - run verify-pool-stack.ps1 on the target to confirm.

.PARAMETER Mode         Snapshot | Restore  (required)
.PARAMETER DigiByteDir  DigiByte install root (default C:\DigiByte).
.PARAMETER DataDir      The data dir holding blocks\ + chainstate\. Blank = auto
                        (C:\DigiByte\data, else %APPDATA%\DigiByte).
.PARAMETER Archive      Snapshot: output .tar.gz path (default
                        <DigiByteDir>\digibyte-datadir-<height>.tar.gz; may be a
                        \\server\share path). Restore: the archive to apply (required).
.PARAMETER Sha256       Restore: expected SHA256. Blank = read <Archive>.sha256.
.PARAMETER Force        Skip confirmation prompts (for unattended fleet runs).

.EXAMPLE
    # On the healthy box - write the archive to a share the fleet can reach:
    .\snapshot-digibyte-datadir.ps1 -Mode Snapshot -Archive \\nas\dgb\digibyte-datadir.tar.gz
.EXAMPLE
    # On each new box:
    .\snapshot-digibyte-datadir.ps1 -Mode Restore -Archive \\nas\dgb\digibyte-datadir.tar.gz
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][ValidateSet('Snapshot', 'Restore')][string]$Mode,
    [string]$DigiByteDir = 'C:\DigiByte',
    [string]$DataDir     = '',
    [string]$Archive     = '',
    [string]$Sha256      = '',
    [switch]$Force
)
$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$ScriptVersion = '1.0.0'

function Say($m, $c = 'Gray') { Write-Host $m -ForegroundColor $c }
function Confirm-OrAbort($prompt) {
    if ($Force) { return $true }
    return ((Read-Host $prompt) -match '^[Yy]')
}

# ---- elevate (stopping DigiByte + writing the datadir needs admin) ----------
$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) {
    if ($PSCommandPath) {
        $fArg = if ($Force) { ' -Force' } else { '' }
        Start-Process powershell.exe -Verb RunAs -ArgumentList ("-ExecutionPolicy Bypass -File `"$PSCommandPath`" -Mode $Mode -DigiByteDir `"$DigiByteDir`" -DataDir `"$DataDir`" -Archive `"$Archive`" -Sha256 `"$Sha256`"" + $fArg)
        return
    } else { throw 'Run this in an elevated (Administrator) PowerShell.' }
}

Say "=== DigiByte data-dir $Mode  (v$ScriptVersion) ===" 'Cyan'
if (-not (Get-Command tar.exe -ErrorAction SilentlyContinue)) { throw 'tar.exe not found (needs Windows 10 1803+ / Windows 11).' }

# ---- resolve data dir (folder containing blocks\ + chainstate\) -------------
if (-not $DataDir) {
    if     (Test-Path (Join-Path $DigiByteDir 'data\blocks'))     { $DataDir = Join-Path $DigiByteDir 'data' }
    elseif (Test-Path (Join-Path $env:APPDATA 'DigiByte\blocks')) { $DataDir = Join-Path $env:APPDATA 'DigiByte' }
    else   { $DataDir = Join-Path $DigiByteDir 'data' }
}
$DgbConf = if (Test-Path (Join-Path $DigiByteDir 'digibyte.conf')) { Join-Path $DigiByteDir 'digibyte.conf' } else { Join-Path $DataDir 'digibyte.conf' }
Say "Data dir : $DataDir" 'White'

# ---- small conf reader + RPC + clean-stop helpers --------------------------
function Read-Cfg($path) {
    $h = @{}
    if (Test-Path $path) {
        foreach ($l in Get-Content $path) {
            $t = $l.Trim()
            if ($t -and -not $t.StartsWith('#')) { $i = $t.IndexOf('='); if ($i -gt 0) { $h[$t.Substring(0, $i).Trim()] = $t.Substring($i + 1).Trim() } }
        }
    }
    return $h
}
function Get-DgbExe($name) {
    $direct = Join-Path $DigiByteDir ("daemon\" + $name)
    if (Test-Path $direct) { return $direct }
    $flat = Join-Path $DigiByteDir $name
    if (Test-Path $flat) { return $flat }
    $found = Get-ChildItem $DigiByteDir -Recurse -Filter $name -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($found) { return $found.FullName }
    return $null
}
function Invoke-DgbRpc($method) {
    $cfg = Read-Cfg $DgbConf
    $user = $cfg['rpcuser']; $pass = $cfg['rpcpassword']
    $port = $cfg['rpcport']; if (-not $port) { $port = '14022' }
    if (-not $user -or -not $pass) {
        $ck = Join-Path $DataDir '.cookie'
        if (Test-Path $ck) { $pair = (Get-Content $ck -Raw).Trim(); $auth = 'Basic ' + [Convert]::ToBase64String([Text.Encoding]::ASCII.GetBytes($pair)) }
        else { throw 'No RPC credentials (rpcuser/rpcpassword) and no .cookie - cannot talk to DigiByte.' }
    } else {
        $auth = 'Basic ' + [Convert]::ToBase64String([Text.Encoding]::ASCII.GetBytes($user + ':' + $pass))
    }
    $body = @{ jsonrpc = '1.0'; id = 'snap'; method = $method; params = @() } | ConvertTo-Json -Compress
    return (Invoke-RestMethod -Uri ("http://127.0.0.1:" + $port + "/") -Method Post -Headers @{ Authorization = $auth } -Body $body -ContentType 'text/plain' -TimeoutSec 30).result
}
# Stop DigiByte CLEANLY. Never force-kill: a hard kill mid-flush can leave
# chainstate torn and force a reindex - exactly what we are trying to avoid.
function Stop-DigiByteClean {
    $proc = Get-Process digibyte-qt, digibyted -ErrorAction SilentlyContinue
    if (-not $proc) { Say "DigiByte is not running - its data is already flushed." 'Green'; return }
    Say "Stopping DigiByte cleanly (RPC stop)..." 'Cyan'
    try { Invoke-DgbRpc 'stop' | Out-Null } catch { Say "  (RPC stop failed: $($_.Exception.Message))" 'Yellow' }
    for ($i = 0; $i -lt 180 -and (Get-Process digibyte-qt, digibyted -ErrorAction SilentlyContinue); $i++) { Start-Sleep -Milliseconds 500 }
    if (Get-Process digibyte-qt, digibyted -ErrorAction SilentlyContinue) {
        throw "DigiByte did not stop cleanly within 90s. CLOSE it yourself (File > Exit / tray > Exit), wait ~10s, then re-run. Do NOT force-kill - that risks a reindex."
    }
    Say "  stopped cleanly." 'Green'
    Start-Sleep -Seconds 2
}
# Run tar with a heartbeat (it is silent for minutes on a huge archive).
function Invoke-TarHeartbeat($tarArgs, $activity) {
    Say "$activity - heavy disk activity for several minutes; this is NORMAL, not frozen." 'Yellow'
    $p = Start-Process tar.exe -ArgumentList $tarArgs -PassThru -WindowStyle Hidden
    $t0 = Get-Date
    while (-not $p.HasExited) { Start-Sleep -Seconds 5; Write-Progress -Activity $activity -Status ("elapsed {0} (working...)" -f (((Get-Date) - $t0).ToString('hh\:mm\:ss'))) }
    Write-Progress -Activity $activity -Completed
    return ($p.ExitCode -eq 0)
}

$dgbDirsPresent = @('blocks', 'chainstate', 'indexes') | Where-Object { Test-Path (Join-Path $DataDir $_) }

# ===========================================================================
if ($Mode -eq 'Snapshot') {
    if ($dgbDirsPresent -notcontains 'blocks' -or $dgbDirsPresent -notcontains 'chainstate') {
        throw "No DigiByte blockchain at $DataDir (need blocks\ + chainstate\). Pass -DataDir <folder>."
    }
    if ($dgbDirsPresent -notcontains 'indexes') {
        Say "NOTE: no indexes\ folder - restored nodes will build indexes themselves." 'Yellow'
    } else {
        Say ("Including indexes\ (" + ((Get-ChildItem (Join-Path $DataDir 'indexes') -Directory -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Name) -join ', ') + ")") 'Gray'
    }

    # capture what is running so we can restart the same thing
    $wasQt     = [bool](Get-Process digibyte-qt -ErrorAction SilentlyContinue)
    $wasDaemon = [bool](Get-Process digibyted   -ErrorAction SilentlyContinue)

    # confirm synced (a mid-sync snapshot still works, just seeds less)
    $height = 0
    try { $info = Invoke-DgbRpc 'getblockchaininfo'; $height = [int]$info.blocks; $vp = [double]$info.verificationprogress
          if ($vp -lt 0.999) { Say ("  WARNING: DigiByte is only {0:P1} synced." -f $vp) 'Yellow'; if (-not (Confirm-OrAbort "  Snapshot anyway? (y/N)")) { return } }
    } catch { Say "  (could not read height via RPC; continuing)" 'Yellow' }

    if (-not $Archive) { $Archive = Join-Path $DigiByteDir ("digibyte-datadir-" + $height + ".tar.gz") }
    $outDir = Split-Path -Parent $Archive
    if ($outDir -and -not (Test-Path $outDir)) { New-Item -ItemType Directory -Force -Path $outDir | Out-Null }
    Say "Archive  : $Archive" 'White'

    Stop-DigiByteClean

    $dirs = @('blocks', 'chainstate', 'indexes') | Where-Object { Test-Path (Join-Path $DataDir $_) }
    $tarArgs = @('-czf', "$Archive", '-C', "$DataDir") + $dirs
    $ok = Invoke-TarHeartbeat $tarArgs "Archiving DigiByte data"
    if (-not $ok) { throw "tar failed while archiving." }
    Say ("  wrote {0} ({1:N1} GB)" -f (Split-Path $Archive -Leaf), ((Get-Item $Archive).Length / 1GB)) 'Green'

    Say "Computing SHA256 (reads the whole file)..." 'Cyan'
    $sha = (Get-FileHash $Archive -Algorithm SHA256).Hash.ToLower()
    Set-Content -Path ($Archive + '.sha256') -Value $sha -Encoding ASCII
    Say ("  " + $sha) 'Gray'
    Say ("  wrote " + (Split-Path ($Archive + '.sha256') -Leaf)) 'Green'

    # restart the source node
    if ($wasDaemon) {
        $exe = Get-DgbExe 'digibyted.exe'
        if ($exe) { Say "Restarting digibyted..." 'Cyan'; Start-Process $exe -ArgumentList "-datadir=`"$DataDir`" -conf=`"$DgbConf`"" -WindowStyle Hidden }
    } elseif ($wasQt) {
        $exe = Get-DgbExe 'digibyte-qt.exe'
        if ($exe) { Say "Reopening DigiByte-Qt..." 'Cyan'; Start-Process $exe -ArgumentList "-datadir=`"$DataDir`"" }
    }
    Say "`n===== Snapshot done =====" 'Green'
    Say "Copy BOTH files to each target box (or a share), then run: -Mode Restore -Archive <path>" 'White'
    return
}

# ===========================================================================
if ($Mode -eq 'Restore') {
    if (-not $Archive) { throw "Restore needs -Archive <path to .tar.gz> (local file or \\share)." }
    if (-not (Test-Path $Archive)) { throw "Archive not found: $Archive" }

    # verify checksum before we touch anything
    if (-not $Sha256) {
        $side = $Archive + '.sha256'
        if (Test-Path $side) { $Sha256 = (Get-Content $side -Raw).Trim().ToLower() }
    }
    if ($Sha256) {
        Say "Verifying SHA256 (reads the whole file, ~a minute)..." 'Cyan'
        $h = (Get-FileHash $Archive -Algorithm SHA256).Hash.ToLower()
        if ($h -ne $Sha256.ToLower()) { throw "Checksum MISMATCH - refusing to restore this file." }
        Say "  checksum OK." 'Green'
    } else {
        Say "WARNING: no SHA256 provided and no .sha256 sidecar - cannot verify integrity." 'Yellow'
        if (-not (Confirm-OrAbort "Restore WITHOUT verification? (y/N)")) { return }
    }

    if ($dgbDirsPresent.Count -gt 0) {
        Say "`n$DataDir already has chain data ($($dgbDirsPresent -join ', '))." 'Yellow'
        Say "It will be moved aside (renamed) before extracting. Your wallets\ and digibyte.conf are NOT touched." 'Yellow'
        if (-not (Confirm-OrAbort "Continue? (y/N)")) { return }
    }

    Stop-DigiByteClean

    # move existing blocks/chainstate/indexes aside (rename = instant, no extra space)
    if ($dgbDirsPresent.Count -gt 0) {
        $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
        foreach ($d in $dgbDirsPresent) {
            $src = Join-Path $DataDir $d
            $dst = Join-Path $DataDir ($d + '.pre-restore-' + $stamp)
            Rename-Item -Path $src -NewName (Split-Path $dst -Leaf)
        }
        Say "  moved old chain data to *.pre-restore-$stamp (delete once the node is confirmed healthy)." 'Gray'
    }

    New-Item -ItemType Directory -Force -Path $DataDir | Out-Null
    $ok = Invoke-TarHeartbeat @('-xzf', "$Archive", '-C', "$DataDir") "Extracting DigiByte data"
    if (-not $ok) { throw "tar extract failed. Your old data is still at *.pre-restore-* - rename it back if needed." }

    Say "`n===== Restore done =====" 'Green'
    Say "Start DigiByte Core; it will verify the snapshot and sync only the recent blocks." 'White'
    Say "IMPORTANT: make sure this box's digibyte.conf ENABLES the indexes you restored" 'Yellow'
    Say "(txindex=1, etc.) or Core will ignore/rebuild them. Verify with:" 'Yellow'
    Say "  pool\deploy\verify-pool-stack.ps1" 'White'
    Say "This box keeps its OWN wallet - no keys were copied from the source." 'Green'
    return
}

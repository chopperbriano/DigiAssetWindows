<#
.SYNOPSIS
    Make fast-sync snapshot pieces so new nodes skip the ~week-long sync.
    Supports the DigiByte blockchain and the DigiAsset chain.db coming from
    DIFFERENT boxes: run one component on each box, upload, then assemble the
    manifest.

.PARAMETER Component
    digibyte  - archive the DigiByte blockchain (run on the synced-wallet box)
    chaindb   - archive chain.db (run on the analyzed-node box)
    both      - do both here (default; use when one box has everything)
    manifest  - assemble snapshot.json from the two part-files (local or from R2)

.PARAMETER BaseUrl   Your R2 public base URL (needed for -Component manifest, and
                     stamped into the manifest). e.g. https://pub-xxxx.r2.dev
.PARAMETER DigiByteDir / DigiAssetDir / OutDir   Paths (sane defaults).

.EXAMPLE
    # Box A (synced DigiByte):
    .\make-snapshot.ps1 -Component digibyte
    # Box B (synced DigiAsset node):
    .\make-snapshot.ps1 -Component chaindb
    # After uploading both archives + their *-part.json, on any box:
    .\make-snapshot.ps1 -Component manifest -BaseUrl https://pub-xxxx.r2.dev
#>
[CmdletBinding()]
param(
    [ValidateSet('both','digibyte','chaindb','manifest','archives')][string]$Component = 'both',
    # archives = digibyte + chaindb, NO manifest (used by publish-snapshot step 1
    #            so the unattended weekly run never hits an interactive prompt).
    # Set for unattended/scheduled runs: any "are you sure?" prompt safe-aborts
    # (throws) instead of blocking forever on Read-Host in a hidden window.
    [switch]$NonInteractive,
    [string]$DigiByteDir  = 'C:\DigiByte',
    [string]$DigiAssetDir = 'C:\DigiAssetWindows',
    # The actual DigiByte data directory (the folder containing blocks\ and
    # chainstate\). Leave blank to auto-detect C:\DigiByte\data or %APPDATA%\DigiByte.
    [string]$DataDir      = '',
    # Block height of the DigiByte snapshot. Only needed if DigiByte has no RPC
    # (a plain wallet) so we can't read it - look at DigiByte-Qt's status bar.
    [int]   $Height       = 0,
    [string]$OutDir       = 'C:\DigiAssetSnapshots',
    [string]$BaseUrl      = '',
    # How long to wait (seconds) for DigiByte / the DigiAsset node to come up and
    # answer RPC after a (re)start - DigiByte-Qt loads its block index for minutes,
    # and the node reconnects to it after that. These are UPPER bounds: every wait
    # polls and moves on as soon as the thing is ready.
    [int]   $StartWaitSec = 600,
    # How long to wait for the DigiAsset node to exit cleanly after it accepts the
    # shutdown request (it finishes its block and flushes chain.db first).
    [int]   $StopWaitSec  = 600
)
$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$ScriptVersion = '2.9.0'

$NodeExe = Join-Path $DigiAssetDir 'DigiAssetWindows.exe'
$CliExe  = Join-Path $DigiAssetDir 'DigiAssetWindows-cli.exe'
function Say($m,$c='Gray'){ Write-Host $m -ForegroundColor $c }
# A yes/no gate that is SAFE to hit unattended: in -NonInteractive mode it never
# waits on a human - it refuses (throws) so a scheduled run aborts cleanly with a
# non-zero exit instead of hanging in a hidden window for hours.
function Confirm-OrAbort($question, $reason){
    if ($NonInteractive) { throw "Aborting (non-interactive): $reason" }
    return ((Read-Host $question) -match '^[Yy]')
}

# Write UTF-8 WITHOUT a BOM. PowerShell 5.1's `Set-Content -Encoding UTF8` prepends
# a BOM (EF BB BF); when R2 serves the file back as octet-stream, the BOM arrives as
# "ï»¿" and ConvertFrom-Json rejects it ("Invalid JSON primitive: ï"). BOM-free
# output keeps snapshot.json + the part files parseable everywhere.
function Write-Utf8NoBom($path, $text) {
    [System.IO.File]::WriteAllText($path, $text, (New-Object System.Text.UTF8Encoding($false)))
}

# Run `tar -czf` as a background process with a live heartbeat (archive size +
# elapsed), so a multi-GB compress doesn't look frozen. Compressing ~30 GB with
# single-threaded gzip is inherently slow (often 20-60 min) - this just shows it's
# still working. Returns $true on success.
function Invoke-TarWithProgress($archive, $srcDir, $items, $label, $sayEverySec = 300) {
    # Windows' own tar, one argument string with every path quoted: an array -ArgumentList is
    # joined with bare spaces under PS 5.1, so a path containing a space would split.
    $argStr = (@('-czf', "`"$archive`"", '-C', "`"$srcDir`"") + ($items | ForEach-Object { "`"$_`"" })) -join ' '
    $p = Start-Process -FilePath (Join-Path $env:SystemRoot 'System32\tar.exe') -ArgumentList $argStr -PassThru -WindowStyle Hidden
    $t0 = Get-Date; $lastSay = $t0
    while (-not $p.HasExited) {
        Start-Sleep -Seconds 3
        $gb = 0.0; if (Test-Path $archive) { try { $gb = (Get-Item $archive).Length / 1GB } catch {} }
        $elStr = ((Get-Date) - $t0).ToString('hh\:mm\:ss')
        # Live banner (blue box) refreshes every loop (~3s); the scrolling text log
        # line is much less frequent (every 5 min) so long archives don't flood the
        # console - the banner is the live "still working" signal.
        Write-Progress -Activity "Archiving $label" -Status ("{0:N2} GB written   elapsed {1}   (compressing, please wait...)" -f $gb, $elStr)
        if (((Get-Date) - $lastSay).TotalSeconds -ge $sayEverySec) {
            Say ("  ...still archiving $label - {0:N2} GB written, elapsed {1}" -f $gb, $elStr) 'DarkGray'
            $lastSay = Get-Date
        }
    }
    Write-Progress -Activity "Archiving $label" -Completed
    return ($p.ExitCode -eq 0)
}

# --- Elevate --------------------------------------------------------------
$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) {
    if ($PSCommandPath) { Start-Process powershell.exe -Verb RunAs -ArgumentList "-ExecutionPolicy Bypass -File `"$PSCommandPath`" -Component $Component -DigiByteDir `"$DigiByteDir`" -DigiAssetDir `"$DigiAssetDir`" -DataDir `"$DataDir`" -Height $Height -OutDir `"$OutDir`" -BaseUrl `"$BaseUrl`" -StartWaitSec $StartWaitSec -StopWaitSec $StopWaitSec"; return }
    else { throw 'Run this in an elevated (Administrator) PowerShell.' }
}

# Resolve the DigiByte data directory (the folder containing blocks\ + chainstate\).
if (-not $DataDir) {
    if     (Test-Path (Join-Path $DigiByteDir 'data\blocks'))     { $DataDir = Join-Path $DigiByteDir 'data' }
    elseif (Test-Path (Join-Path $env:APPDATA 'DigiByte\blocks')) { $DataDir = Join-Path $env:APPDATA 'DigiByte' }
    else   { $DataDir = Join-Path $DigiByteDir 'data' }
}
$DgbData = $DataDir
# Our layout keeps digibyte.conf in C:\DigiByte (parent of Data); a stock install
# keeps it in the datadir. Prefer the parent, fall back to the datadir.
$DgbConf = if (Test-Path (Join-Path $DigiByteDir 'digibyte.conf')) { Join-Path $DigiByteDir 'digibyte.conf' } else { Join-Path $DgbData 'digibyte.conf' }
Say "=== Make DigiAsset fast-sync snapshot ($Component)  (v$ScriptVersion) ===" 'Cyan'
if ($Component -ne 'manifest' -and -not (Test-Path (Join-Path $env:SystemRoot 'System32\tar.exe'))) { throw "tar.exe not found (needs Windows 10 1803+ / Windows 11)." }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

# --- Resolve BaseUrl NOW, not after the archives are built -------------------
# The manifest step needs the public R2 URL. It used to prompt for it inside
# New-Manifest, which runs LAST - so `-Component both` compressed ~34 GB for
# 20-60 minutes and only THEN blocked on a Read-Host, with nobody watching.
# Resolve it here instead: prefer what the caller passed, fall back to the
# publicUrl saved by setup-cloudflare-snapshots.ps1, and only prompt as a last
# resort - before any archiving starts.
if ($Component -in 'both','manifest') {
    if (-not $BaseUrl) {
        $cfgFile = Join-Path $PSScriptRoot 'snapshot-config.json'
        if (Test-Path $cfgFile) {
            try {
                $sc = Get-Content $cfgFile -Raw | ConvertFrom-Json
                if ($sc.publicUrl) { $BaseUrl = $sc.publicUrl; Say "  base URL from snapshot-config.json: $BaseUrl" 'Gray' }
            } catch { Say "  (couldn't read $cfgFile)" 'Yellow' }
        }
    }
    if (-not $BaseUrl) {
        if ($NonInteractive) { throw "manifest needs -BaseUrl in non-interactive mode (the public R2 URL)." }
        Say "`nThe manifest records the public URL nodes download from. Asking now so a long" 'Yellow'
        Say "archive run doesn't finish and then stop here waiting for an answer." 'Yellow'
        $BaseUrl = (Read-Host "Enter your R2 public base URL (e.g. https://pub-xxxx.r2.dev)")
    }
    if (-not $BaseUrl) { throw "no R2 public base URL given - cannot write the manifest." }
}

function Read-Cfg($path){ $h=@{}; if(Test-Path $path){ foreach($l in Get-Content $path){ $t=$l.Trim(); if($t -and -not $t.StartsWith('#')){ $i=$t.IndexOf('='); if($i -gt 0){ $h[$t.Substring(0,$i).Trim()]=$t.Substring($i+1).Trim() } } } }; return $h }

# --- DigiByte component ---------------------------------------------------
function New-DigiByteArchive {
    if (-not (Test-Path (Join-Path $DgbData 'blocks'))) { throw "No DigiByte blockchain at $DgbData (no blocks\ folder). Pass -DataDir <folder with blocks\ + chainstate\>." }
    Say "`nDigiByte data: $DgbData" 'White'
    $h = if ($Height -gt 0) { $Height } else { 0 }
    $h = $Height   # base height (0 unless -Height was passed); RPC below overrides it when reachable
    $ver = 'unknown'
    # Capture whichever is running (GUI wallet OR headless daemon) so we restart
    # the SAME one afterwards - previously a running digibyted was left stopped.
    $dgbProc = Get-Process digibyte-qt,digibyted -ErrorAction SilentlyContinue | Select-Object -First 1
    $qtPath  = $dgbProc.Path
    $running = [bool]$dgbProc
    if ($running) {
        # Try RPC (needs server=1 + creds/cookie) for the height and a clean stop.
        $cfg = Read-Cfg $DgbConf
        $authPair = $null
        if ($cfg['rpcuser']) { $authPair = "$($cfg['rpcuser']):$($cfg['rpcpassword'])" }
        else { $ck = Join-Path $DgbData '.cookie'; if (Test-Path $ck) { $authPair = (Get-Content $ck -Raw).Trim() } }
        $port = 14022; if ($cfg['rpcport']) { try { $port=[int]$cfg['rpcport'] } catch {} }
        if ($authPair) {
            function Dgb($m){ $b64=[Convert]::ToBase64String([Text.Encoding]::ASCII.GetBytes($authPair)); Invoke-RestMethod -Uri "http://127.0.0.1:$port" -Method Post -ContentType 'text/plain' -Headers @{Authorization="Basic $b64"} -TimeoutSec 15 -Body ('{"jsonrpc":"1.0","id":"s","method":"'+$m+'","params":[]}') }
            try {
                $info=(Dgb 'getblockchaininfo').result; $h=[int]$info.blocks
                $pct=[math]::Round([double]$info.verificationprogress*100,2)
                try { $ver=((Dgb 'getnetworkinfo').result.subversion) -replace '[^0-9\.]','' } catch {}
                Say ("  height {0:N0}   synced {1}%   version {2}" -f $h,$pct,$ver) 'White'
                if ($pct -lt 99.9) { Say "  WARNING: DigiByte is NOT fully synced." 'Yellow'; if(-not (Confirm-OrAbort "  Continue anyway? (y/N)" "DigiByte not fully synced ($pct%)")){ return } }
                Say "Stopping DigiByte cleanly (via RPC)..." 'Cyan'; try { Dgb 'stop' | Out-Null } catch {}
            } catch { Say "  RPC not answering (server=1 not enabled?)." 'Yellow' }
        } else { Say "  DigiByte is running but has no RPC access (no server=1 / creds)." 'Yellow' }
        # A big node flushes its chainstate on the way out, which can take minutes (the
        # v9.26.6 notes: "stop the old node normally and wait for it to exit"). Same
        # budget as the DigiAsset node: -StopWaitSec.
        $t0 = Get-Date
        while ((Get-Process digibyte-qt,digibyted -EA SilentlyContinue) -and ((Get-Date) - $t0).TotalSeconds -lt $StopWaitSec) {
            Write-Progress -Activity 'Waiting for DigiByte to exit' -Status ("elapsed {0:mm\:ss} of up to {1}s (flushing its databases)" -f ((Get-Date) - $t0), $StopWaitSec)
            Start-Sleep -Seconds 1
        }
        Write-Progress -Activity 'Waiting for DigiByte to exit' -Completed
        if (Get-Process digibyte-qt,digibyted -EA SilentlyContinue) {
            throw "DigiByte is still running ${StopWaitSec}s after the stop request and I couldn't stop it cleanly. Please CLOSE DigiByte yourself (File > Exit, or right-click the tray icon > Exit), wait for it to fully close, then re-run this. Do NOT force-kill it - that can corrupt the data."
        }
    } else {
        Say "  DigiByte is not running - good, its data is already flushed to disk." 'Green'
    }
    if ($h -le 0) { Say "  NOTE: block height unknown - the manifest height-check will be skipped. (Pass -Height <N> from DigiByte-Qt's status bar to record it.)" 'Yellow' }
    Start-Sleep -Seconds 2
    $dgbDirs = @('blocks','chainstate','indexes') | Where-Object { Test-Path (Join-Path $DgbData $_) }
    if ($dgbDirs -notcontains 'blocks' -or $dgbDirs -notcontains 'chainstate') { throw "blocks\ or chainstate\ missing under $DgbData." }
    # FIXED filename (no height suffix) so the URL in snapshot.json is stable and R2
    # just overwrites. Remove any old digibyte archives first so we never upload
    # stale copies (rclone copy would otherwise push every *.tar.gz in the folder).
    $archive = Join-Path $OutDir 'digibyte.tar.gz'
    Get-ChildItem $OutDir -Filter 'digibyte*.tar.gz' -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
    Say "Archiving the DigiByte blockchain (big - compressing ~30 GB can take 20-60 min)..." 'Cyan'
    if (-not (Invoke-TarWithProgress $archive $DgbData $dgbDirs 'DigiByte blockchain')) { throw "tar failed." }
    Say ("  + {0} ({1:N1} GB)" -f (Split-Path $archive -Leaf),((Get-Item $archive).Length/1GB)) 'Green'
    Say "Computing SHA256 (reads the whole file, ~a minute for a large archive)..." 'Cyan'
    $sha=(Get-FileHash $archive -Algorithm SHA256).Hash.ToLower()
    $part=[ordered]@{ file=(Split-Path $archive -Leaf); sha256=$sha; height=$h; version=$ver; sizeBytes=(Get-Item $archive).Length }
    Write-Utf8NoBom (Join-Path $OutDir 'digibyte-part.json') ($part|ConvertTo-Json)
    Say "  + digibyte-part.json" 'Green'
    if ($running -and $qtPath) {
        Say "Reopening DigiByte..." 'Cyan'; Start-Process $qtPath -ArgumentList "-datadir=`"$DgbData`""
        # Wait (up to -StartWaitSec) for DigiByte to finish loading and answer RPC
        # before moving on: the chain.db step that follows needs the DigiAsset node
        # to be answering, and the node can't until DigiByte is back. RPC returns
        # error -28 ("Loading block index...") while warming up, which lands in the
        # catch below, so we only stop waiting on a real getblockchaininfo result.
        if ($authPair) {
            $b64 = [Convert]::ToBase64String([Text.Encoding]::ASCII.GetBytes($authPair))
            $body = '{"jsonrpc":"1.0","id":"s","method":"getblockchaininfo","params":[]}'
            $t0 = Get-Date; $ready = $false
            while (-not $ready -and ((Get-Date) - $t0).TotalSeconds -lt $StartWaitSec) {
                Start-Sleep -Seconds 5
                try { $r = Invoke-RestMethod -Uri "http://127.0.0.1:$port" -Method Post -ContentType 'text/plain' -Headers @{Authorization="Basic $b64"} -TimeoutSec 10 -Body $body; if ($r.result.blocks -gt 0) { $ready = $true } } catch {}
                Write-Progress -Activity 'Waiting for DigiByte to finish loading' -Status ("elapsed {0:mm\:ss} of up to {1}s" -f ((Get-Date) - $t0), $StartWaitSec)
            }
            Write-Progress -Activity 'Waiting for DigiByte to finish loading' -Completed
            if ($ready) { Say ("  DigiByte answering RPC after {0:N0}s" -f ((Get-Date) - $t0).TotalSeconds) 'Green' }
            else        { Say "  DigiByte still not answering RPC after ${StartWaitSec}s - carrying on; the chain.db step waits for the node separately." 'Yellow' }
        } else {
            # No RPC access to poll - give it a fixed head start instead.
            Say "  (no DigiByte RPC access to check readiness - waiting 60s)" 'Gray'; Start-Sleep -Seconds 60
        }
    }
}

# --- chain.db component ---------------------------------------------------
# Ask the node for syncstate. Returns @{ Out = <CLI text>; Height = <int or 0> }.
# The CLI reports every failure (RPC down, command forbidden by rpcallow, auth)
# on STDOUT with exit code 0, so the text is kept: it is the only explanation we
# get when syncstate/shutdown don't work.
# Answered = the node replied to syncstate at all (RPC is up), which is what the
# shutdown needs. syncstate has no height field - it returns {count, sync}, where
# count is DigiByte's height and sync is 0 when fully synced (negative = blocks
# behind, positive = a non-running state) - so the chain.db height comes from
# getnodestats' syncHeight, falling back to count when sync is 0.
function Invoke-NodeCli($cmd) {
    try { Push-Location $DigiAssetDir; return (& $CliExe $cmd 2>&1 | Out-String).Trim() }
    catch { return $_.Exception.Message }
    finally { try { Pop-Location } catch {} }
}
function Get-NodeSyncState {
    if (-not (Test-Path $CliExe)) { return @{ Out = "$CliExe not found"; Height = 0; Answered = $false } }
    $out = Invoke-NodeCli 'syncstate'
    $count = [regex]::Match($out, '"count"\s*:\s*(\d+)')
    $sync  = [regex]::Match($out, '"sync"\s*:\s*(-?\d+)')
    $answered = $count.Success -and $sync.Success
    $h = 0
    if ($answered) {
        $mm = [regex]::Match((Invoke-NodeCli 'getnodestats'), '"syncHeight"\s*:\s*(\d+)')
        if ($mm.Success) { $h = [int]$mm.Groups[1].Value }
        elseif ([int]$sync.Groups[1].Value -eq 0) { $h = [int]$count.Groups[1].Value }
    }
    return @{ Out = $out; Height = $h; Answered = $answered }
}

function New-ChainDbArchive {
    $chainDb = Join-Path $DigiAssetDir 'chain.db'
    if (-not (Test-Path $chainDb)) { throw "chain.db not found at $chainDb" }
    # Use $cdbHeight, NOT $height: PowerShell variable names are case-INSENSITIVE,
    # so a local $height would BE the $Height parameter - and `$height=0` here would
    # clobber the caller's -Height to 0 (that bug made chain.db always publish as
    # height 0 even when -Height was passed).
    $cdbHeight = 0
    $nodeUp = [bool](Get-Process DigiAssetWindows,DigiAssetCore -EA SilentlyContinue)
    # When the DigiByte archive ran first, DigiByte was down for 20-60 min and was
    # only just reopened: the node can't answer RPC until DigiByte-Qt has loaded and
    # the node has reconnected. Asking it to shut down in that window is what made
    # the shutdown request silently miss. So wait (up to -StartWaitSec) until the
    # node answers syncstate before going any further.
    $ss = @{ Out = ''; Height = 0; Answered = $false }
    if ($nodeUp) {
        $t0 = Get-Date
        while ($true) {
            $ss = Get-NodeSyncState
            if ($ss.Answered -or ((Get-Date) - $t0).TotalSeconds -ge $StartWaitSec) { break }
            if (-not (Get-Process DigiAssetWindows,DigiAssetCore -EA SilentlyContinue)) { break }
            Write-Progress -Activity 'Waiting for the DigiAsset node to answer RPC' -Status ("elapsed {0:mm\:ss} of up to {1}s (it reconnects to DigiByte after a restart)" -f ((Get-Date) - $t0), $StartWaitSec)
            Start-Sleep -Seconds 5
        }
        Write-Progress -Activity 'Waiting for the DigiAsset node to answer RPC' -Completed
        if ($ss.Answered -and ((Get-Date) - $t0).TotalSeconds -ge 5) { Say ("  node answering RPC after {0:N0}s" -f ((Get-Date) - $t0).TotalSeconds) 'Green' }
    }
    $syncOut = $ss.Out; $cdbHeight = $ss.Height
    if ($cdbHeight -le 0 -and $Height -gt 0) { $cdbHeight = $Height }   # allow a manual stamp
    if ($cdbHeight -le 0) {
        if ($nodeUp) {
            $why = if ($ss.Answered) { 'answered but reported no chain.db height' } else { "did not answer syncstate within ${StartWaitSec}s" }
            Say "  NOTE: the node is running but $why - labelling it 0. The CLI said:" 'Yellow'
            Say "    $(if ($syncOut) { $syncOut } else { '(no output)' })" 'Yellow'
        } else {
            Say "  NOTE: chain.db height unknown (no running node here to read syncstate) - labelling it 0." 'Yellow'
        }
        Say "  Cosmetic only; fast-sync still works. Pass -Height <N> to record the real height." 'Yellow'
    }
    Say "`nStopping the DigiAsset node (clean shutdown)..." 'Cyan'
    if (Get-Process DigiAssetWindows,DigiAssetCore -EA SilentlyContinue) {
        $shutOut = ''
        if (Test-Path $CliExe) { try { Push-Location $DigiAssetDir; $shutOut = (& $CliExe shutdown 2>&1 | Out-String).Trim(); Pop-Location } catch { try{Pop-Location}catch{}; $shutOut = $_.Exception.Message } }
        else { $shutOut = "$CliExe not found" }
        # A successful shutdown RPC prints "true". Only an explicit refusal (rpcallow) means
        # the node will not stop. Anything else - above all a CLI timeout - usually means
        # it IS stopping: before win.145 the shutdown RPC stopped the chain analyzer (which
        # finishes its block) and IPFS before replying, which outlasts the CLI's 10 s
        # timeout. The CLI then printed "libcurl error: 22" - 22 is CURLE_OPERATION_TIMEDOUT
        # in the node's own curl numbering, not an HTTP error - while the node carried on
        # shutting down. So refuse fast only on "forbidden"; otherwise sit out the full
        # -StopWaitSec below.
        if ($shutOut -match 'forbidden') {
            throw "The DigiAsset node refused the shutdown request (config.cfg rpcallow). The CLI said: $shutOut`nAdd rpcallowshutdown=1 to $DigiAssetDir\config.cfg (or update the node to win.143+, which allows it by default), restart the node, then re-run."
        }
        if ($shutOut -notmatch '^\s*true\s*$') {
            Say "  the shutdown request did not answer 'true' ($(if ($shutOut) { ($shutOut -split "`n")[-1].Trim() } else { 'no output' })) - usually it is still shutting down; waiting for it to exit." 'Yellow'
        }
        # Wait up to -StopWaitSec for a CLEAN exit: after "Safe to shut down" the node
        # still finishes its current block, stops the RPC server and flushes chain.db,
        # which on a large DB can take well over a minute. We must NOT force-kill into the
        # archive: the node runs SQLite with journal_mode=MEMORY, so a hard kill
        # mid-write can leave chain.db torn (no -wal to replay) and that torn DB would
        # be served to every new node. If it won't stop cleanly, abort.
        $t0 = Get-Date
        while ((Get-Process DigiAssetWindows,DigiAssetCore -EA SilentlyContinue) -and ((Get-Date) - $t0).TotalSeconds -lt $StopWaitSec) {
            Write-Progress -Activity 'Waiting for the DigiAsset node to exit' -Status ("elapsed {0:mm\:ss} of up to {1}s (flushing chain.db)" -f ((Get-Date) - $t0), $StopWaitSec)
            Start-Sleep -Milliseconds 500
        }
        Write-Progress -Activity 'Waiting for the DigiAsset node to exit' -Completed
        if (Get-Process DigiAssetWindows,DigiAssetCore -EA SilentlyContinue) {
            throw "The DigiAsset node was still running ${StopWaitSec}s after the shutdown request. Aborting so we don't snapshot a possibly-inconsistent chain.db. Look at the node window: 'Shutting down...' means it is still flushing (let it finish, then re-run, or raise -StopWaitSec); no such line means the request never arrived (check rpcallow in config.cfg, then re-run)."
        }
        Say ("  node exited cleanly after {0:N0}s" -f ((Get-Date) - $t0).TotalSeconds) 'Green'
    }
    Start-Sleep -Seconds 2
    $chainFiles = @('chain.db','chain.db-wal','chain.db-shm') | Where-Object { Test-Path (Join-Path $DigiAssetDir $_) }
    # FIXED filename (no height suffix) so the URL in snapshot.json is stable and R2
    # just overwrites. Remove any old chaindb archives first so we never upload stale
    # copies (rclone copy would otherwise push every *.tar.gz in the folder).
    $archive = Join-Path $OutDir 'digiasset-chaindb.tar.gz'
    Get-ChildItem $OutDir -Filter 'digiasset-chaindb*.tar.gz' -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
    Say "Archiving chain.db..." 'Cyan'
    if (-not (Invoke-TarWithProgress $archive $DigiAssetDir $chainFiles 'chain.db' 15)) { throw "tar failed." }
    Say ("  + {0} ({1:N1} GB)" -f (Split-Path $archive -Leaf),((Get-Item $archive).Length/1GB)) 'Green'
    Say "Computing SHA256 (reads the whole file)..." 'Cyan'
    $sha=(Get-FileHash $archive -Algorithm SHA256).Hash.ToLower()
    $part=[ordered]@{ file=(Split-Path $archive -Leaf); sha256=$sha; height=$cdbHeight; sizeBytes=(Get-Item $archive).Length }
    Write-Utf8NoBom (Join-Path $OutDir 'chaindb-part.json') ($part|ConvertTo-Json)
    Say "  + chaindb-part.json  (chain.db height: $cdbHeight)" 'Green'
    if (Test-Path $NodeExe) { Say "Restarting the node..." 'Cyan'; Start-Process -FilePath $NodeExe -WorkingDirectory $DigiAssetDir }
}

# --- Assemble the manifest from the two parts -----------------------------
function New-Manifest {
    if (-not $BaseUrl) {
        if ($NonInteractive) { throw "manifest needs -BaseUrl in non-interactive mode (the public R2 URL)." }
        $BaseUrl = (Read-Host "Enter your R2 public base URL (e.g. https://pub-xxxx.r2.dev)")
    }
    $base = $BaseUrl.TrimEnd('/')
    function Load-Part($name){
        # Always return a PARSED object (or $null) - never a raw string. R2 serves
        # .json as octet-stream (with a UTF-8 BOM), which made Invoke-RestMethod
        # hand back a STRING that then got embedded into snapshot.json as a
        # stringified blob (with a mangled BOM) instead of a nested object. Fetch
        # the text ourselves, strip any BOM, and ConvertFrom-Json.
        $txt = $null
        $local = Join-Path $OutDir $name
        if (Test-Path $local) {
            $txt = Get-Content $local -Raw
        } else {
            try {
                Say "  fetching $name from R2..."
                $r = Invoke-WebRequest -Uri "$base/$name" -UseBasicParsing -TimeoutSec 20
                $txt = $r.Content
                if ($txt -is [byte[]]) { $txt = [Text.Encoding]::UTF8.GetString($txt) }
            } catch { return $null }
        }
        if (-not $txt) { return $null }
        try { return (($txt.TrimStart([char]0xFEFF)) | ConvertFrom-Json) } catch { return $null }
    }
    $d = Load-Part 'digibyte-part.json'
    $c = Load-Part 'chaindb-part.json'
    if (-not $d) { throw "digibyte-part.json not found locally or at $base - run/upload the digibyte component first." }
    if (-not $c) { throw "chaindb-part.json not found locally or at $base - run/upload the chaindb component first." }
    if ([int]$d.height -gt 0 -and [int]$c.height -gt 0 -and [int]$c.height -gt [int]$d.height) {
        Say "`n  WARNING: chain.db height ($($c.height)) is AHEAD of the DigiByte snapshot ($($d.height))." 'Red'
        Say "  That is unsafe - the node would have analysis for blocks the wallet doesn't have yet." 'Red'
        Say "  Use a DigiByte snapshot at >= the chain.db height (regenerate the DigiByte part)." 'Red'
        if (-not (Confirm-OrAbort "  Write the manifest anyway? (y/N)" "chain.db height ahead of DigiByte snapshot")) { return }
    }
    $man=[ordered]@{ baseUrl=$base; created=(Get-Date).ToString('s'); digibyte=$d; chaindb=$c }
    Write-Utf8NoBom (Join-Path $OutDir 'snapshot.json') ($man|ConvertTo-Json -Depth 6)
    Say "`n  + snapshot.json  (digibyte height $($d.height), chain.db height $($c.height))" 'Green'
}

# --- Dispatch -------------------------------------------------------------
# Pause the node's login supervisor (setup-digiasset.ps1 -Mode LaunchNode) for the
# whole run. It restarts DigiByte and the node whenever they exit, which would
# relaunch the node seconds after the clean shutdown below - while chain.db is being
# archived. It honours this file (ignoring it once it is 6h old, so a crashed run
# can't pause it forever), and resumes as soon as it is removed.
# Preflight, before anything is stopped or compressed: the chain.db step needs the
# node to accept `cli shutdown`, and with no rpcallow lines (or a config that never
# got the setup defaults) the node refuses it - which used to surface only after the
# DigiByte archive had already taken 35+ minutes.
if (($Component -ne 'digibyte') -and ($Component -ne 'manifest') -and (Get-Process DigiAssetWindows,DigiAssetCore -EA SilentlyContinue)) {
    $ncfg = Read-Cfg (Join-Path $DigiAssetDir 'config.cfg')
    $isOn = { param($v) "$v" -match '^(1|true)$' }
    $allowAll = & $isOn $ncfg['rpcallow*']
    # an explicit rpcallow<name> wins over rpcallow*, as in the node (Server::isRPCAllowed)
    $missing = @('shutdown','syncstate','getnodestats') | Where-Object { if ($ncfg.ContainsKey("rpcallow$_")) { -not (& $isOn $ncfg["rpcallow$_"]) } else { -not $allowAll } }
    if ($missing -contains 'shutdown') {
        throw "The node's config.cfg does not allow the 'shutdown' RPC, so the chain.db step cannot stop it cleanly. Add these lines to $DigiAssetDir\config.cfg, restart the node, then re-run:`n" + (($missing | ForEach-Object { "  rpcallow$_=1" }) -join "`n")
    }
    if ($missing) { Say ("  NOTE: config.cfg does not allow " + ($missing -join ', ') + " - the chain.db height will not be recorded (add rpcallow<name>=1 lines).") 'Yellow' }
}

$supervisorPause = Join-Path $DigiAssetDir 'supervisor.pause'
if ($Component -ne 'manifest') {
    try { Set-Content -Path $supervisorPause -Value "$(Get-Date -Format s) make-snapshot: archiving ($Component)" -Encoding ASCII } catch {}
}
try {
    switch ($Component) {
        'digibyte' { New-DigiByteArchive }
        'chaindb'  { New-ChainDbArchive }
        'manifest' { New-Manifest }
        # chain.db FIRST. The pair is only safe with chain.db at or behind the DigiByte
        # snapshot (a node restored from it catches up through the newer blocks), and
        # the node can never be ahead of the DigiByte it reads - so stop the node and
        # archive chain.db, then archive DigiByte. The old order (DigiByte first) always
        # produced a chain.db AHEAD, because the node kept indexing while DigiByte was
        # being compressed; that went unnoticed only while chain.db's height was never
        # recorded (fixed in 2.4.1), and since then New-Manifest rightly refuses it.
        'archives' { New-ChainDbArchive; New-DigiByteArchive }   # both archives, NO manifest
        default    { New-ChainDbArchive; New-DigiByteArchive; New-Manifest }
    }
} finally {
    Remove-Item $supervisorPause -Force -ErrorAction SilentlyContinue
}

Say "`n===== Done ($Component) =====" 'Green'
Say "Output folder: $OutDir" 'White'

# Be blunt about this: make-snapshot only BUILDS files. Running it and walking
# away used to look like a completed publish, while the live snapshot.json still
# pointed at the previous snapshot and nobody noticed for days.
Say "`n*** NOTHING HAS BEEN UPLOADED. These files are still only on this PC. ***" 'Yellow'
Say "Nodes keep fast-syncing from the PREVIOUS snapshot until you upload them." 'Yellow'

Say "`nEasiest way to finish - one command that uploads, republishes, and verifies:" 'Cyan'
Say "  .\publish-snapshot.ps1 -SkipBuild" 'White'
Say "  (-SkipBuild reuses what you just built instead of recompressing it all again)" 'Gray'

Say "`nOr by hand - archives FIRST, snapshot.json LAST:" 'Cyan'
if ($Component -in 'digibyte','both')  { Say "  upload: digibyte.tar.gz  +  digibyte-part.json" 'Gray' }
if ($Component -in 'chaindb','both')   { Say "  upload: digiasset-chaindb.tar.gz  +  chaindb-part.json" 'Gray' }
if ($Component -in 'manifest','both')  { Say "  upload: snapshot.json  (LAST - publishing it first points nodes at files that aren't up yet)" 'Gray' }
Say "  rclone copy $OutDir\ r2:<your-bucket>/ --progress" 'White'

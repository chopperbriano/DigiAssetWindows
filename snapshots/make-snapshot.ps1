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
    [string]$BaseUrl      = ''
)
$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$ScriptVersion = '2.3.0'

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

# Run `tar -czf` as a background process with a live heartbeat (archive size +
# elapsed), so a multi-GB compress doesn't look frozen. Compressing ~30 GB with
# single-threaded gzip is inherently slow (often 20-60 min) - this just shows it's
# still working. Returns $true on success.
function Invoke-TarWithProgress($archive, $srcDir, $items, $label) {
    $argList = @('-czf', "$archive", '-C', "$srcDir") + $items
    $p = Start-Process -FilePath 'tar.exe' -ArgumentList $argList -PassThru -WindowStyle Hidden
    $t0 = Get-Date; $lastSay = $t0
    while (-not $p.HasExited) {
        Start-Sleep -Seconds 3
        $gb = 0.0; if (Test-Path $archive) { try { $gb = (Get-Item $archive).Length / 1GB } catch {} }
        $elStr = ((Get-Date) - $t0).ToString('hh\:mm\:ss')
        # Live banner (blue box) refreshes every loop (~3s); the scrolling text log
        # line is much less frequent (every 5 min) so long archives don't flood the
        # console - the banner is the live "still working" signal.
        Write-Progress -Activity "Archiving $label" -Status ("{0:N2} GB written   elapsed {1}   (compressing, please wait...)" -f $gb, $elStr)
        if (((Get-Date) - $lastSay).TotalSeconds -ge 300) {
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
    if ($PSCommandPath) { Start-Process powershell.exe -Verb RunAs -ArgumentList "-ExecutionPolicy Bypass -File `"$PSCommandPath`" -Component $Component -DigiByteDir `"$DigiByteDir`" -DigiAssetDir `"$DigiAssetDir`" -DataDir `"$DataDir`" -Height $Height -OutDir `"$OutDir`" -BaseUrl `"$BaseUrl`""; return }
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
if ($Component -ne 'manifest' -and -not (Get-Command tar.exe -ErrorAction SilentlyContinue)) { throw "tar.exe not found (needs Windows 10 1803+ / Windows 11)." }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

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
        for($i=0;$i -lt 60 -and (Get-Process digibyte-qt,digibyted -EA SilentlyContinue);$i++){ Start-Sleep -Seconds 1 }
        if (Get-Process digibyte-qt,digibyted -EA SilentlyContinue) {
            throw "DigiByte is still running and I couldn't stop it cleanly. Please CLOSE DigiByte yourself (File > Exit, or right-click the tray icon > Exit), wait ~10s for it to fully close, then re-run this. Do NOT force-kill it - that can corrupt the data."
        }
    } else {
        Say "  DigiByte is not running - good, its data is already flushed to disk." 'Green'
    }
    if ($h -le 0) { Say "  NOTE: block height unknown - the manifest height-check will be skipped. (Pass -Height <N> from DigiByte-Qt's status bar to record it.)" 'Yellow' }
    Start-Sleep -Seconds 2
    $dgbDirs = @('blocks','chainstate','indexes') | Where-Object { Test-Path (Join-Path $DgbData $_) }
    if ($dgbDirs -notcontains 'blocks' -or $dgbDirs -notcontains 'chainstate') { throw "blocks\ or chainstate\ missing under $DgbData." }
    $archive = Join-Path $OutDir "digibyte-$h.tar.gz"
    Say "Archiving the DigiByte blockchain (big - compressing ~30 GB can take 20-60 min)..." 'Cyan'
    if (-not (Invoke-TarWithProgress $archive $DgbData $dgbDirs 'DigiByte blockchain')) { throw "tar failed." }
    Say ("  + {0} ({1:N1} GB)" -f (Split-Path $archive -Leaf),((Get-Item $archive).Length/1GB)) 'Green'
    Say "Computing SHA256 (reads the whole file, ~a minute for a large archive)..." 'Cyan'
    $sha=(Get-FileHash $archive -Algorithm SHA256).Hash.ToLower()
    $part=[ordered]@{ file=(Split-Path $archive -Leaf); sha256=$sha; height=$h; version=$ver; sizeBytes=(Get-Item $archive).Length }
    ($part|ConvertTo-Json) | Set-Content -Path (Join-Path $OutDir 'digibyte-part.json') -Encoding UTF8
    Say "  + digibyte-part.json" 'Green'
    if ($running -and $qtPath) { Say "Reopening DigiByte..." 'Cyan'; Start-Process $qtPath -ArgumentList "-datadir=`"$DgbData`"" }
}

# --- chain.db component ---------------------------------------------------
function New-ChainDbArchive {
    $chainDb = Join-Path $DigiAssetDir 'chain.db'
    if (-not (Test-Path $chainDb)) { throw "chain.db not found at $chainDb" }
    $height=0
    if (Test-Path $CliExe) { try { Push-Location $DigiAssetDir; $s = (& $CliExe syncstate 2>$null | Out-String); Pop-Location; $mm=[regex]::Match($s,'"height"\s*:\s*(\d+)'); if($mm.Success){ $height=[int]$mm.Groups[1].Value } } catch { try{Pop-Location}catch{} } }
    if ($height -le 0 -and $Height -gt 0) { $height = $Height }   # allow a manual stamp
    if ($height -le 0) {
        Say "  NOTE: chain.db height unknown (no running node here to read syncstate) - labelling it 0." 'Yellow'
        Say "  Cosmetic only; fast-sync still works. Pass -Height <N> to record the real height." 'Yellow'
    }
    Say "`nStopping the DigiAsset node (clean shutdown)..." 'Cyan'
    if (Get-Process DigiAssetWindows,DigiAssetCore -EA SilentlyContinue) {
        if (Test-Path $CliExe) { try { Push-Location $DigiAssetDir; & $CliExe shutdown 2>$null | Out-Null; Pop-Location } catch { try{Pop-Location}catch{} } }
        # Wait up to 60s for a CLEAN exit. We must NOT force-kill into the archive:
        # the node runs SQLite with journal_mode=MEMORY, so a hard kill mid-write can
        # leave chain.db torn (no -wal to replay) and that torn DB would be served to
        # every new node. If it won't stop cleanly, abort rather than snapshot it.
        for($i=0;$i -lt 120 -and (Get-Process DigiAssetWindows,DigiAssetCore -EA SilentlyContinue);$i++){ Start-Sleep -Milliseconds 500 }
        if (Get-Process DigiAssetWindows,DigiAssetCore -EA SilentlyContinue) {
            throw "The DigiAsset node did not shut down cleanly within 60s. Aborting so we don't snapshot a possibly-inconsistent chain.db. Close the node window and re-run."
        }
    }
    Start-Sleep -Seconds 2
    $chainFiles = @('chain.db','chain.db-wal','chain.db-shm') | Where-Object { Test-Path (Join-Path $DigiAssetDir $_) }
    $archive = Join-Path $OutDir "digiasset-chaindb-$height.tar.gz"
    Say "Archiving chain.db..." 'Cyan'
    if (-not (Invoke-TarWithProgress $archive $DigiAssetDir $chainFiles 'chain.db')) { throw "tar failed." }
    Say ("  + {0} ({1:N1} GB)" -f (Split-Path $archive -Leaf),((Get-Item $archive).Length/1GB)) 'Green'
    Say "Computing SHA256 (reads the whole file)..." 'Cyan'
    $sha=(Get-FileHash $archive -Algorithm SHA256).Hash.ToLower()
    $part=[ordered]@{ file=(Split-Path $archive -Leaf); sha256=$sha; height=$height; sizeBytes=(Get-Item $archive).Length }
    ($part|ConvertTo-Json) | Set-Content -Path (Join-Path $OutDir 'chaindb-part.json') -Encoding UTF8
    Say "  + chaindb-part.json  (chain.db height: $height)" 'Green'
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
    ($man|ConvertTo-Json -Depth 6) | Set-Content -Path (Join-Path $OutDir 'snapshot.json') -Encoding UTF8
    Say "`n  + snapshot.json  (digibyte height $($d.height), chain.db height $($c.height))" 'Green'
}

# --- Dispatch -------------------------------------------------------------
switch ($Component) {
    'digibyte' { New-DigiByteArchive }
    'chaindb'  { New-ChainDbArchive }
    'manifest' { New-Manifest }
    'archives' { New-DigiByteArchive; New-ChainDbArchive }   # both archives, NO manifest
    default    { New-DigiByteArchive; New-ChainDbArchive; New-Manifest }
}

Say "`n===== Done ($Component) =====" 'Green'
Say "Output folder: $OutDir" 'White'
if ($Component -in 'digibyte','both')  { Say "  upload: digibyte-*.tar.gz  +  digibyte-part.json" 'Gray' }
if ($Component -in 'chaindb','both')   { Say "  upload: digiasset-chaindb-*.tar.gz  +  chaindb-part.json" 'Gray' }
if ($Component -in 'manifest','both')  { Say "  upload: snapshot.json  (do this LAST, after both parts are up)" 'Gray' }
Say "Upload with:  rclone copy $OutDir\ r2:<your-bucket>/ --progress" 'Cyan'

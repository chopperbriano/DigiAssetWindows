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
    [ValidateSet('both','digibyte','chaindb','manifest')][string]$Component = 'both',
    [string]$DigiByteDir  = 'C:\DigiByte',
    [string]$DigiAssetDir = 'C:\DigiAsset',
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
$ScriptVersion = '2.0.0'

$NodeExe = Join-Path $DigiAssetDir 'DigiAssetWindows.exe'
$CliExe  = Join-Path $DigiAssetDir 'DigiAssetWindows-cli.exe'
function Say($m,$c='Gray'){ Write-Host $m -ForegroundColor $c }

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
$DgbConf = Join-Path $DgbData 'digibyte.conf'
Say "=== Make DigiAsset fast-sync snapshot ($Component)  (v$ScriptVersion) ===" 'Cyan'
if ($Component -ne 'manifest' -and -not (Get-Command tar.exe -ErrorAction SilentlyContinue)) { throw "tar.exe not found (needs Windows 10 1803+ / Windows 11)." }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

function Read-Cfg($path){ $h=@{}; if(Test-Path $path){ foreach($l in Get-Content $path){ $t=$l.Trim(); if($t -and -not $t.StartsWith('#')){ $i=$t.IndexOf('='); if($i -gt 0){ $h[$t.Substring(0,$i).Trim()]=$t.Substring($i+1).Trim() } } } }; return $h }

# --- DigiByte component ---------------------------------------------------
function New-DigiByteArchive {
    if (-not (Test-Path (Join-Path $DgbData 'blocks'))) { throw "No DigiByte blockchain at $DgbData (no blocks\ folder). Pass -DataDir <folder with blocks\ + chainstate\>." }
    Say "`nDigiByte data: $DgbData" 'White'
    $h = if ($Height -gt 0) { $Height } else { 0 }
    $ver = 'unknown'
    $qtPath  = (Get-Process digibyte-qt -ErrorAction SilentlyContinue | Select-Object -First 1).Path
    $running = [bool](Get-Process digibyte-qt,digibyted -ErrorAction SilentlyContinue)
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
                if ($pct -lt 99.9) { Say "  WARNING: DigiByte is NOT fully synced." 'Yellow'; if((Read-Host "  Continue? (y/N)") -notmatch '^[Yy]'){ return } }
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
    Say "Archiving the DigiByte blockchain (big - be patient)..." 'Cyan'
    & tar.exe -czf "$archive" -C "$DgbData" $dgbDirs
    if ($LASTEXITCODE -ne 0) { throw "tar failed (exit $LASTEXITCODE)." }
    Say ("  + {0} ({1:N1} GB)" -f (Split-Path $archive -Leaf),((Get-Item $archive).Length/1GB)) 'Green'
    Say "Computing SHA256..." 'Cyan'
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
    Say "`nStopping the DigiAsset node (clean shutdown)..." 'Cyan'
    if (Get-Process DigiAssetWindows -EA SilentlyContinue) {
        if (Test-Path $CliExe) { try { Push-Location $DigiAssetDir; & $CliExe shutdown 2>$null | Out-Null; Pop-Location } catch { try{Pop-Location}catch{} } }
        for($i=0;$i -lt 40 -and (Get-Process DigiAssetWindows -EA SilentlyContinue);$i++){ Start-Sleep -Milliseconds 500 }
        Get-Process DigiAssetWindows -EA SilentlyContinue | Stop-Process -Force -EA SilentlyContinue
    }
    Start-Sleep -Seconds 2
    $chainFiles = @('chain.db','chain.db-wal','chain.db-shm') | Where-Object { Test-Path (Join-Path $DigiAssetDir $_) }
    $archive = Join-Path $OutDir "digiasset-chaindb-$height.tar.gz"
    Say "Archiving chain.db..." 'Cyan'
    & tar.exe -czf "$archive" -C "$DigiAssetDir" $chainFiles
    if ($LASTEXITCODE -ne 0) { throw "tar failed (exit $LASTEXITCODE)." }
    Say ("  + {0} ({1:N1} GB)" -f (Split-Path $archive -Leaf),((Get-Item $archive).Length/1GB)) 'Green'
    Say "Computing SHA256..." 'Cyan'
    $sha=(Get-FileHash $archive -Algorithm SHA256).Hash.ToLower()
    $part=[ordered]@{ file=(Split-Path $archive -Leaf); sha256=$sha; height=$height; sizeBytes=(Get-Item $archive).Length }
    ($part|ConvertTo-Json) | Set-Content -Path (Join-Path $OutDir 'chaindb-part.json') -Encoding UTF8
    Say "  + chaindb-part.json  (chain.db height: $height)" 'Green'
    if (Test-Path $NodeExe) { Say "Restarting the node..." 'Cyan'; Start-Process -FilePath $NodeExe -WorkingDirectory $DigiAssetDir }
}

# --- Assemble the manifest from the two parts -----------------------------
function New-Manifest {
    if (-not $BaseUrl) { $BaseUrl = (Read-Host "Enter your R2 public base URL (e.g. https://pub-xxxx.r2.dev)") }
    $base = $BaseUrl.TrimEnd('/')
    function Load-Part($name){
        $local = Join-Path $OutDir $name
        if (Test-Path $local) { return (Get-Content $local -Raw | ConvertFrom-Json) }
        try { Say "  fetching $name from R2..."; return (Invoke-RestMethod -Uri "$base/$name" -TimeoutSec 20) } catch { return $null }
    }
    $d = Load-Part 'digibyte-part.json'
    $c = Load-Part 'chaindb-part.json'
    if (-not $d) { throw "digibyte-part.json not found locally or at $base - run/upload the digibyte component first." }
    if (-not $c) { throw "chaindb-part.json not found locally or at $base - run/upload the chaindb component first." }
    if ([int]$d.height -gt 0 -and [int]$c.height -gt 0 -and [int]$c.height -gt [int]$d.height) {
        Say "`n  WARNING: chain.db height ($($c.height)) is AHEAD of the DigiByte snapshot ($($d.height))." 'Red'
        Say "  That is unsafe - the node would have analysis for blocks the wallet doesn't have yet." 'Red'
        Say "  Use a DigiByte snapshot at >= the chain.db height (regenerate the DigiByte part)." 'Red'
        if ((Read-Host "  Write the manifest anyway? (y/N)") -notmatch '^[Yy]') { return }
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
    default    { New-DigiByteArchive; New-ChainDbArchive; New-Manifest }
}

Say "`n===== Done ($Component) =====" 'Green'
Say "Output folder: $OutDir" 'White'
if ($Component -in 'digibyte','both')  { Say "  upload: digibyte-*.tar.gz  +  digibyte-part.json" 'Gray' }
if ($Component -in 'chaindb','both')   { Say "  upload: digiasset-chaindb-*.tar.gz  +  chaindb-part.json" 'Gray' }
if ($Component -in 'manifest','both')  { Say "  upload: snapshot.json  (do this LAST, after both parts are up)" 'Gray' }
Say "Upload with:  rclone copy $OutDir\ r2:<your-bucket>/ --progress" 'Cyan'

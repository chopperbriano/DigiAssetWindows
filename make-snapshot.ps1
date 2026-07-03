<#
.SYNOPSIS
    Make a fast-sync SNAPSHOT from a fully-synced box: packages the DigiByte
    blockchain + the DigiAsset chain.db into two verified archives that new nodes
    can download to skip the ~week-long sync from scratch.

    Run this ON A SYNCED NODE. It cleanly stops the apps, builds the archives,
    checksums them, writes snapshot.json, and restarts the apps. Then you upload
    the 3 output files to Cloudflare R2 (instructions printed at the end).

.PARAMETER DigiByteDir   Default C:\DigiByte
.PARAMETER DigiAssetDir  Default C:\DigiAsset
.PARAMETER OutDir        Where to write the archives (default C:\DigiAssetSnapshots)
.PARAMETER BaseUrl       Your R2 public URL (e.g. https://pub-xxxx.r2.dev). Optional -
                         you can fill it into snapshot.json after uploading.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\make-snapshot.ps1
    powershell -ExecutionPolicy Bypass -File .\make-snapshot.ps1 -BaseUrl https://pub-abc123.r2.dev
#>
[CmdletBinding()]
param(
    [string]$DigiByteDir  = 'C:\DigiByte',
    [string]$DigiAssetDir = 'C:\DigiAsset',
    [string]$OutDir       = 'C:\DigiAssetSnapshots',
    [string]$BaseUrl      = ''
)
$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$ScriptVersion = '1.0.0'

$DgbData    = Join-Path $DigiByteDir 'data'
$DgbConf    = Join-Path $DgbData 'digibyte.conf'
$NodeExe    = Join-Path $DigiAssetDir 'DigiAssetWindows.exe'
$CliExe     = Join-Path $DigiAssetDir 'DigiAssetWindows-cli.exe'
$ChainDb    = Join-Path $DigiAssetDir 'chain.db'

function Say($m,$c='Gray'){ Write-Host $m -ForegroundColor $c }

# --- Elevate --------------------------------------------------------------
$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) {
    if ($PSCommandPath) {
        $a = "-ExecutionPolicy Bypass -File `"$PSCommandPath`" -DigiByteDir `"$DigiByteDir`" -DigiAssetDir `"$DigiAssetDir`" -OutDir `"$OutDir`" -BaseUrl `"$BaseUrl`""
        Start-Process powershell.exe -Verb RunAs -ArgumentList $a; return
    } else { throw 'Run this in an elevated (Administrator) PowerShell.' }
}

Say "=== Make DigiAsset fast-sync snapshot  (v$ScriptVersion) ===" 'Cyan'

# --- Prerequisites --------------------------------------------------------
if (-not (Get-Command tar.exe -ErrorAction SilentlyContinue)) { throw "tar.exe not found (needs Windows 10 1803+ / Windows 11)." }
if (-not (Test-Path $DgbData))  { throw "DigiByte data not found at $DgbData" }
if (-not (Test-Path $ChainDb))  { throw "chain.db not found at $ChainDb - is the DigiAsset node installed + synced here?" }
$cfg = @{}
if (Test-Path $DgbConf) { foreach($l in Get-Content $DgbConf){ $t=$l.Trim(); if($t -and -not $t.StartsWith('#')){ $i=$t.IndexOf('='); if($i -gt 0){ $cfg[$t.Substring(0,$i).Trim()]=$t.Substring($i+1).Trim() } } } }
if (-not $cfg['rpcuser']) { throw "Could not read RPC credentials from $DgbConf" }
$rpcPort = 14022; if ($cfg['rpcport']) { try { $rpcPort=[int]$cfg['rpcport'] } catch {} }

function Dgb($method){
    $b64=[Convert]::ToBase64String([Text.Encoding]::ASCII.GetBytes("$($cfg['rpcuser']):$($cfg['rpcpassword'])"))
    return Invoke-RestMethod -Uri "http://127.0.0.1:$rpcPort" -Method Post -ContentType 'text/plain' `
        -Headers @{Authorization="Basic $b64"} -TimeoutSec 15 -Body ('{"jsonrpc":"1.0","id":"snap","method":"'+$method+'","params":[]}')
}

# --- Height + sync check --------------------------------------------------
Say "`nChecking DigiByte sync state..." 'Cyan'
$height = 0; $ver = 'unknown'
try {
    $info = (Dgb 'getblockchaininfo').result
    $height = [int]$info.blocks
    $pct = [math]::Round([double]$info.verificationprogress*100,2)
    try { $ver = ((Dgb 'getnetworkinfo').result.subversion) -replace '[^0-9\.]','' } catch {}
    Say ("  DigiByte height: {0:N0}   synced: {1}%   version: {2}" -f $height,$pct,$ver) 'White'
    if ($pct -lt 99.9) {
        Say "  WARNING: DigiByte is NOT fully synced. A snapshot now will still be behind the tip." 'Yellow'
        if ((Read-Host "  Continue anyway? (y/N)") -notmatch '^[Yy]') { Say "Cancelled."; return }
    }
} catch {
    Say "  Could not reach DigiByte RPC (is the wallet running?). Cannot confirm sync state." 'Yellow'
    if ((Read-Host "  Continue anyway? (y/N)") -notmatch '^[Yy]') { return }
}

# --- Disk space check -----------------------------------------------------
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$dataGB = [math]::Round((Get-ChildItem $DgbData -Recurse -File -ErrorAction SilentlyContinue | Measure-Object Length -Sum).Sum/1GB,1)
$freeGB = [math]::Round((Get-PSDrive -Name ((Split-Path $OutDir -Qualifier).TrimEnd(':'))).Free/1GB,1)
Say ("`nDigiByte data is ~{0} GB; free space on {1} is {2} GB." -f $dataGB,(Split-Path $OutDir -Qualifier),$freeGB) 'White'
if ($freeGB -lt $dataGB) { Say "  WARNING: you may not have enough free space for the archive." 'Yellow'; if ((Read-Host "  Continue? (y/N)") -notmatch '^[Yy]') { return } }

Say "`nThis will STOP the DigiByte wallet + DigiAsset node while it archives (required" 'Yellow'
Say "for a clean, non-corrupt snapshot), then restart them. It can take a while." 'Yellow'
if ((Read-Host "Proceed? (y/N)") -notmatch '^[Yy]') { Say "Cancelled."; return }

# --- Clean stop -----------------------------------------------------------
Say "`nStopping the DigiAsset node (clean shutdown)..." 'Cyan'
if (Get-Process DigiAssetWindows -ErrorAction SilentlyContinue) {
    if (Test-Path $CliExe) { try { Push-Location $DigiAssetDir; & $CliExe shutdown 2>$null | Out-Null; Pop-Location } catch { Pop-Location } }
    for ($i=0; $i -lt 40 -and (Get-Process DigiAssetWindows -ErrorAction SilentlyContinue); $i++) { Start-Sleep -Milliseconds 500 }
    Get-Process DigiAssetWindows -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
}
Say "Stopping DigiByte Core (clean shutdown via RPC)..." 'Cyan'
try { Dgb 'stop' | Out-Null } catch {}
for ($i=0; $i -lt 60 -and ((Get-Process digibyte-qt,digibyted -ErrorAction SilentlyContinue)); $i++) { Start-Sleep -Seconds 1 }
Get-Process digibyte-qt,digibyted -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 3   # let LevelDB flush to disk

# --- Archive (ALLOWLIST only - never touches wallet.dat / digibyte.conf) ---
$dgbArchive   = Join-Path $OutDir "digibyte-$height.tar.gz"
$chainArchive = Join-Path $OutDir "digiasset-chaindb-$height.tar.gz"

Say "`nArchiving the DigiByte blockchain (this is the big one - be patient)..." 'Cyan'
$dgbDirs = @('blocks','chainstate','indexes') | Where-Object { Test-Path (Join-Path $DgbData $_) }
if ($dgbDirs -notcontains 'blocks' -or $dgbDirs -notcontains 'chainstate') { throw "blocks/ or chainstate/ missing under $DgbData - cannot make a valid snapshot." }
& tar.exe -czf "$dgbArchive" -C "$DgbData" $dgbDirs
if ($LASTEXITCODE -ne 0) { throw "tar failed on the DigiByte data (exit $LASTEXITCODE)." }
Say ("  + {0} ({1:N1} GB)" -f (Split-Path $dgbArchive -Leaf),((Get-Item $dgbArchive).Length/1GB)) 'Green'

Say "Archiving chain.db (the DigiAsset analyzer database)..." 'Cyan'
$chainFiles = @('chain.db','chain.db-wal','chain.db-shm') | Where-Object { Test-Path (Join-Path $DigiAssetDir $_) }
& tar.exe -czf "$chainArchive" -C "$DigiAssetDir" $chainFiles
if ($LASTEXITCODE -ne 0) { throw "tar failed on chain.db (exit $LASTEXITCODE)." }
Say ("  + {0} ({1:N1} GB)" -f (Split-Path $chainArchive -Leaf),((Get-Item $chainArchive).Length/1GB)) 'Green'

# --- Checksums ------------------------------------------------------------
Say "`nComputing SHA256 checksums (can take a few minutes on big files)..." 'Cyan'
$dgbHash   = (Get-FileHash $dgbArchive   -Algorithm SHA256).Hash.ToLower()
$chainHash = (Get-FileHash $chainArchive -Algorithm SHA256).Hash.ToLower()

# --- Manifest -------------------------------------------------------------
if (-not $BaseUrl) { $BaseUrl = (Read-Host "`nEnter your R2 public base URL (or press Enter to fill in later)") }
$manifest = [ordered]@{
    baseUrl  = $BaseUrl.TrimEnd('/')
    created  = (Get-Date).ToString('s')
    digibyte = [ordered]@{ file=(Split-Path $dgbArchive -Leaf);   sha256=$dgbHash;   height=$height; version=$ver; sizeBytes=(Get-Item $dgbArchive).Length }
    chaindb  = [ordered]@{ file=(Split-Path $chainArchive -Leaf); sha256=$chainHash; height=$height;                sizeBytes=(Get-Item $chainArchive).Length }
}
$manifestPath = Join-Path $OutDir 'snapshot.json'
($manifest | ConvertTo-Json -Depth 5) | Set-Content -Path $manifestPath -Encoding UTF8
Say "  + snapshot.json" 'Green'

# --- Restart the apps -----------------------------------------------------
Say "`nRestarting DigiByte + the node..." 'Cyan'
$qt = Get-ChildItem $DigiByteDir -Recurse -Filter 'digibyte-qt.exe' -ErrorAction SilentlyContinue | Select-Object -First 1
if ($qt) { Start-Process $qt.FullName -ArgumentList "-datadir=$DgbData" }
Start-Sleep -Seconds 5
if (Test-Path $NodeExe) { Start-Process -FilePath $NodeExe -WorkingDirectory $DigiAssetDir }

# --- Next steps -----------------------------------------------------------
Say "`n===== Snapshot ready =====" 'Green'
Say "Output folder: $OutDir" 'White'
Say "  $(Split-Path $dgbArchive -Leaf)" 'Gray'
Say "  $(Split-Path $chainArchive -Leaf)" 'Gray'
Say "  snapshot.json" 'Gray'
Say "`nNEXT - upload all THREE files to your Cloudflare R2 bucket, then make sure" 'Cyan'
Say "snapshot.json's \"baseUrl\" is your bucket's public URL (e.g. https://pub-xxxx.r2.dev)." 'Cyan'
Say "The installer reads snapshot.json, downloads each archive, verifies its SHA256," 'Gray'
Say "and extracts it before first launch (falling back to normal sync on any problem)." 'Gray'

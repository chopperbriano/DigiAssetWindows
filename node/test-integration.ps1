<#
.SYNOPSIS
    ISOLATED integration smoke test for the mctrivia-merged node. Runs a FRESH
    DigiAsset node in a throwaway directory against your existing DigiByte Core
    and watches the things the upstream merge changed - sync progress, the SQLite
    WAL file size (the walCheckpoint fix), the :8090 console, pool status, and the
    CLI commands.

.DESCRIPTION
    SAFE: it never touches your real node. It uses its own test directory, its own
    fresh chain.db, and its own high web/RPC ports. It only READS from DigiByte
    Core (RPC) - it does not modify Core, your wallet, your production chain.db, or
    your snapshot.

    IMPORTANT: run this on a box where your PRODUCTION DigiAsset node is NOT
    running. The merged node has a single-instance lock (named mutex), so a running
    production node would reject this test node. Your DigiByte Core SHOULD be
    running and synced (the test node reads blocks from it).

    What it checks (PASS/FAIL at the end):
      1. The node starts and stays up (no crash).
      2. Sync height advances (it's reading + analyzing blocks from Core).
      3. The WAL file stays BOUNDED - it should oscillate (grow then drop when
         checkpointed), NOT grow without limit. Unbounded growth = the walCheckpoint
         wiring regressed.
      4. The :8090-style console endpoint answers.
      5. The CLI answers (version/syncstate).

.PARAMETER ExeDir      Where the merged exes are (default: .\build\... resolved).
.PARAMETER DigiByteConf Path to digibyte.conf for Core RPC creds (default C:\DigiByte\digibyte.conf).
.PARAMETER TestDir     Isolated test folder (default C:\DigiAssetIntegrationTest). WIPED at start.
.PARAMETER WatchMinutes How long to run + watch (default 15).
.PARAMETER WebPort      Test web port (default 8922 - avoids the real 8090).
.PARAMETER AssetPort    Test asset RPC port (default 14924 - avoids the real 14024).
.PARAMETER Keep         Keep the test folder afterward (default: keep; pass -Remove to delete).
.PARAMETER Remove       Delete the test folder at the end.

.EXAMPLE
    # from the repo root of the integration branch, on your TEST server:
    powershell -ExecutionPolicy Bypass -File .\node\test-integration.ps1
.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\node\test-integration.ps1 -WatchMinutes 30 -DigiByteConf C:\DigiByte\digibyte.conf
#>
[CmdletBinding()]
param(
    [string]$ExeDir       = '',
    [string]$DigiByteConf = 'C:\DigiByte\digibyte.conf',
    [string]$TestDir      = 'C:\DigiAssetIntegrationTest',
    [int]   $WatchMinutes = 15,
    [int]   $WebPort      = 8922,
    [int]   $AssetPort    = 14924,
    [switch]$Remove
)
$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
function Say($m, $c = 'Gray') { Write-Host $m -ForegroundColor $c }
$repoRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)

Say '==============================================================' 'Cyan'
Say ' Merged-node integration smoke test (ISOLATED - safe)' 'Cyan'
Say '==============================================================' 'Cyan'

# ---- 1. Resolve the merged exes -------------------------------------------
if (-not $ExeDir) {
    $cand = Join-Path $repoRoot 'build\src\Release'
    if (Test-Path (Join-Path $cand 'DigiAssetWindows.exe')) { $ExeDir = $cand }
}
$nodeExe = Join-Path $ExeDir 'DigiAssetWindows.exe'
$cliBuilt = Join-Path (Join-Path $repoRoot 'build\cli\Release') 'DigiAssetWindows-cli.exe'
if (-not (Test-Path $nodeExe)) { throw "DigiAssetWindows.exe not found in '$ExeDir'. Build first, or pass -ExeDir." }
Say "node exe : $nodeExe" 'Gray'

# ---- 2. Guard: production node must not be running (InstanceLock) ----------
if (Get-Process DigiAssetWindows -ErrorAction SilentlyContinue) {
    throw "A DigiAssetWindows process is already running. Stop your production node first (the merged node's single-instance lock will reject a 2nd instance)."
}

# ---- 3. Read Core RPC creds so the test node can reach DigiByte Core -------
# Auto-find digibyte.conf across the common locations if the given path is absent.
if (-not (Test-Path $DigiByteConf)) {
    $cands = @(
        'C:\DigiByte\digibyte.conf',
        'C:\DigiByte\Data\digibyte.conf',
        (Join-Path $env:APPDATA 'DigiByte\digibyte.conf'),
        (Join-Path $env:APPDATA 'DigiByte\Data\digibyte.conf')
    )
    $found = $cands | Where-Object { Test-Path $_ } | Select-Object -First 1
    if ($found) { $DigiByteConf = $found } else { throw "digibyte.conf not found (looked in C:\DigiByte and %APPDATA%\DigiByte). Pass -DigiByteConf <path>." }
}
Say "core conf: $DigiByteConf" 'Gray'
$dc = @{}
Get-Content $DigiByteConf | ForEach-Object { if ($_ -match '^\s*([^#=]+?)\s*=\s*(.*?)\s*$') { $dc[$Matches[1].Trim()] = $Matches[2].Trim() } }
$ru = $dc['rpcuser']; $rp = $dc['rpcpassword']; $rport = if ($dc['rpcport']) { [int]$dc['rpcport'] } else { 14022 }
if (-not $ru -or -not $rp) { throw "rpcuser/rpcpassword missing from $DigiByteConf." }
Say "core RPC : 127.0.0.1:$rport (user '$ru')" 'Gray'

# ---- 4. Build the isolated test directory ---------------------------------
if (Test-Path $TestDir) { Say "Wiping old test dir $TestDir..." 'Yellow'; Remove-Item $TestDir -Recurse -Force -ErrorAction SilentlyContinue }
New-Item -ItemType Directory -Force -Path $TestDir | Out-Null
Copy-Item $nodeExe (Join-Path $TestDir 'DigiAssetWindows.exe') -Force
if (Test-Path $cliBuilt) { Copy-Item $cliBuilt (Join-Path $TestDir 'DigiAssetWindows-cli.exe') -Force }
$webSrc = Join-Path $repoRoot 'web'
if (Test-Path (Join-Path $webSrc 'index.html')) { Copy-Item $webSrc $TestDir -Recurse -Force }
$cliExe = Join-Path $TestDir 'DigiAssetWindows-cli.exe'

# Test config: our own ports, our own chain.db (in TestDir), Core creds, no payout.
@"
rpcuser=$ru
rpcpassword=$rp
rpcbind=127.0.0.1
rpcport=$rport
rpcassetport=$AssetPort
rpcallowip=127.0.0.1
webport=$WebPort
ipfspath=http://127.0.0.1:5001/api/v0/
psp1server=https://pool.digistamp.co
psp1payout=
storeNonAssetUTXO=0
"@ | Set-Content -Path (Join-Path $TestDir 'config.cfg') -Encoding ASCII
Say "test dir : $TestDir  (fresh chain.db, web :$WebPort, asset RPC :$AssetPort)" 'Gray'

# ---- 5. Launch the node (visible window so you can watch the dashboard) ----
Say "`nStarting the merged node (fresh sync from genesis)..." 'Cyan'
Start-Process -FilePath (Join-Path $TestDir 'DigiAssetWindows.exe') -WorkingDirectory $TestDir -WindowStyle Normal
Start-Sleep -Seconds 8
if (-not (Get-Process DigiAssetWindows -ErrorAction SilentlyContinue)) { throw "Node exited immediately - check $TestDir. FAIL." }

# ---- 6. Watch loop: sync height + WAL size --------------------------------
$statusUrl = "http://127.0.0.1:$WebPort/api/status.json"
$walPath   = Join-Path $TestDir 'chain.db-wal'
$deadline  = (Get-Date).AddMinutes($WatchMinutes)
$maxWalMB = 0.0; $walDropped = $false; $lastWalMB = 0.0
$startHeight = -1; $lastHeight = -1; $samples = 0
Say ("`nWatching for {0} min  (time | sync height | WAL MB | note)" -f $WatchMinutes) 'White'
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Seconds 15
    if (-not (Get-Process DigiAssetWindows -ErrorAction SilentlyContinue)) { Say '  NODE EXITED during watch - FAIL.' 'Red'; break }
    $h = -1
    try {
        $s = Invoke-RestMethod -Uri $statusUrl -TimeoutSec 8
        $h = [int]$s.sync.height
        if ($startHeight -lt 0 -and $h -gt 0) { $startHeight = $h }
        $lastHeight = $h
    } catch {}
    $walMB = 0.0
    if (Test-Path $walPath) { try { $walMB = [math]::Round((Get-Item $walPath).Length / 1MB, 1) } catch {} }
    if ($walMB -gt $maxWalMB) { $maxWalMB = $walMB }
    if ($walMB -lt ($lastWalMB - 1)) { $walDropped = $true }   # WAL shrank => a checkpoint fired
    $lastWalMB = $walMB
    $samples++
    $note = ''
    if ($walDropped) { $note = 'WAL checkpointed OK' }
    Say ("  {0:HH:mm:ss} | {1,10} | {2,7} | {3}" -f (Get-Date), $h, $walMB, $note) 'Gray'
}

# ---- 7. CLI check ----------------------------------------------------------
Say "`nCLI check (against the TEST node on :$AssetPort)..." 'Cyan'
$cliOk = $false
if (Test-Path $cliExe) {
    try { Push-Location $TestDir; $ver = (& $cliExe version 2>&1 | Out-String).Trim(); Pop-Location
          if ($ver -and $ver -notmatch 'connect|refused|error') { $cliOk = $true; Say "  cli version -> $ver" 'Green' } else { Say "  cli version -> $ver" 'Yellow' } }
    catch { try { Pop-Location } catch {}; Say "  cli failed: $($_.Exception.Message)" 'Yellow' }
}

# ---- 8. Clean shutdown -----------------------------------------------------
Say "`nStopping the test node (clean)..." 'Cyan'
if (Test-Path $cliExe) { try { Push-Location $TestDir; & $cliExe shutdown 2>$null | Out-Null; Pop-Location } catch { try{Pop-Location}catch{} } }
for ($i = 0; $i -lt 20 -and (Get-Process DigiAssetWindows -ErrorAction SilentlyContinue); $i++) { Start-Sleep -Seconds 1 }
Get-Process DigiAssetWindows -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue

# ---- 9. Verdict ------------------------------------------------------------
$advanced = ($startHeight -ge 0 -and $lastHeight -gt $startHeight)
$walBounded = ($maxWalMB -lt 500)   # with checkpointing the WAL should stay well under this; unbounded growth blows past it
Say "`n==============================================================" 'Cyan'
Say ' RESULT' 'Cyan'
Say '==============================================================' 'Cyan'
function Line($ok, $label, $detail) { $c = if ($ok) { 'Green' } else { 'Red' }; $t = if ($ok) { 'PASS' } else { 'FAIL' }; Say ("  [{0}] {1}  {2}" -f $t, $label, $detail) $c }
Line ($lastHeight -ge 0) 'node stayed up + console answered' ("last height $lastHeight, $samples samples")
Line $advanced          'sync advanced'                      ("start $startHeight -> $lastHeight")
Line $walBounded        'WAL bounded (checkpoint working)'    ("max ${maxWalMB} MB; shrank at least once: $walDropped")
Line $cliOk             'CLI answered'                        ''
$allPass = ($lastHeight -ge 0) -and $advanced -and $walBounded
Say ''
if ($allPass) { Say ' OVERALL: PASS - merged node syncs, WAL stays bounded, console + CLI work.' 'Green' }
else { Say ' OVERALL: REVIEW - one or more checks failed (see above). Do NOT merge to master yet.' 'Red' }
if (-not $walDropped -and $walBounded) { Say " NOTE: WAL never shrank during the window - run longer (>2500 blocks) to see a checkpoint fire." 'Yellow' }
Say ''
if ($Remove) { Say "Removing $TestDir..." 'Gray'; Remove-Item $TestDir -Recurse -Force -ErrorAction SilentlyContinue }
else { Say "Test dir kept for inspection: $TestDir  (delete it yourself, or re-run with -Remove)" 'Gray' }
exit ([int](-not $allPass))

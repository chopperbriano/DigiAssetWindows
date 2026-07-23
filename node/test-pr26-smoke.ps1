<#
.SYNOPSIS
    Runtime smoke test for the PR26 (asset_features) merged Windows build.

    Launches the merged node against your existing DigiByte Core and a chain.db,
    on ISOLATED ports, and verifies:
      * the node starts, connects to Core, opens chain.db, answers RPC
      * the new asset RPC methods are REGISTERED and responding
        (getnewaddress + getwalletbalances actually run; issueasset / reissueasset
         / burnasset / sendasset / sendmanyassets are probed with NO args so they
         return a parameter error - proving they dispatch - WITHOUT mutating chain
         state or spending DGB)
      * the TCP event stream port opens (and prints any event it emits)

    The node's single-instance lock name is hardcoded ("digiasset_core"), so the
    production node cannot run at the same time. This script stops it for the
    duration and restarts it afterward.

.PARAMETER ExeDir    Build folder containing src\Release + cli\Release exes.
.PARAMETER ChainDb   A chain.db to test against (copied into the test dir).
.PARAMETER ProdDir   Production node dir - Core creds are read from its config.cfg
                     and its node is restarted after the test.
.PARAMETER Force     Stop the production node without prompting.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\node\test-pr26-smoke.ps1
#>
[CmdletBinding()]
param(
    [string]$ExeDir    = "C:\repo\DAW-pr26\build",
    [string]$ChainDb   = "C:\DigiAssetWindows\chain.db",
    [string]$ProdDir   = "C:\DigiAssetWindows",
    [int]   $AssetPort = 14824,
    [int]   $WebPort   = 18090,
    [int]   $EventPort = 14825,
    [string]$TestDir   = "$env:TEMP\daw-pr26-smoke",
    [switch]$Force
)
$ErrorActionPreference = 'Stop'
function Say($m,$c='Gray'){ Write-Host $m -ForegroundColor $c }
$script:pass=@(); $script:fail=@()
function Check($name,$ok){ if($ok){ $script:pass+=$name; Say "  PASS  $name" 'Green' } else { $script:fail+=$name; Say "  FAIL  $name" 'Red' } }

$node = Join-Path $ExeDir 'src\Release\DigiAssetWindows.exe'
$cli  = Join-Path $ExeDir 'cli\Release\DigiAssetWindows-cli.exe'
foreach($f in $node,$cli){ if(-not (Test-Path $f)){ throw "missing exe: $f" } }
# --- Read Core RPC creds: prefer the production config.cfg, else digibyte.conf ---
$cfg = @{}
$credSrc = Join-Path $ProdDir 'config.cfg'
if(-not (Test-Path $credSrc)){ $credSrc = Join-Path $env:APPDATA 'DigiByte\digibyte.conf' }
if(-not (Test-Path $credSrc)){ throw "no RPC creds source found ($ProdDir\config.cfg or %APPDATA%\DigiByte\digibyte.conf)" }
Get-Content $credSrc | ForEach-Object { if($_ -match '^\s*([^#=]+)=(.*)$'){ $cfg[$Matches[1].Trim()]=$Matches[2] } }
if(-not $cfg.ContainsKey('rpcport')){ $cfg['rpcport']='14022' }
foreach($k in 'rpcuser','rpcpassword'){ if(-not $cfg.ContainsKey($k)){ throw "$credSrc missing $k" } }
Say "Using Core RPC creds from: $credSrc" 'Gray'

# --- Stop the production node (hardcoded InstanceLock forces this) ---
$prodRunning = [bool](Get-Process DigiAssetWindows -EA SilentlyContinue)
if($prodRunning){
    if(-not $Force){
        Say "The production node is running. This smoke test must stop it briefly (it restarts after)." 'Yellow'
        if((Read-Host "Stop it and run the smoke test? (y/N)") -notmatch '^[Yy]'){ Say 'Cancelled.'; return }
    }
    Say "Stopping production node..." 'Cyan'
    $prodCli = Join-Path $ProdDir 'DigiAssetWindows-cli.exe'
    if(Test-Path $prodCli){ try{ Push-Location $ProdDir; & $prodCli shutdown 2>$null | Out-Null; Pop-Location }catch{ try{Pop-Location}catch{} } }
    for($i=0;$i -lt 30 -and (Get-Process DigiAssetWindows -EA SilentlyContinue);$i++){ Start-Sleep -Milliseconds 500 }
    Get-Process DigiAssetWindows -EA SilentlyContinue | Stop-Process -Force -EA SilentlyContinue
    Start-Sleep -Seconds 2
}

# --- Build an isolated test dir + config + chain.db copy ---
if(Test-Path $TestDir){ Remove-Item $TestDir -Recurse -Force }
New-Item -ItemType Directory -Force -Path $TestDir | Out-Null
Copy-Item $node (Join-Path $TestDir 'DigiAssetWindows.exe')
Copy-Item $cli  (Join-Path $TestDir 'DigiAssetWindows-cli.exe')
if($ChainDb -and (Test-Path $ChainDb)){ Copy-Item $ChainDb (Join-Path $TestDir 'chain.db'); Say "  using chain.db: $ChainDb" 'Gray' }
else { Say "  no chain.db - node will create a fresh one (smoke only, not a sync test)" 'Yellow' }
if(Test-Path (Join-Path $ProdDir 'web')){ Copy-Item (Join-Path $ProdDir 'web') $TestDir -Recurse }

$payout = 'dgb1qh9n2zzuhdd37gyrktjam5uju8gy3f5ems4yna3'
if($cfg.ContainsKey('psp0payout')){ $payout = $cfg['psp0payout'] }
$lines = @(
  "rpcuser=$($cfg['rpcuser'])","rpcpassword=$($cfg['rpcpassword'])","rpcbind=127.0.0.1","rpcport=$($cfg['rpcport'])",
  "rpcassetport=$AssetPort","webport=$WebPort","eventport=$EventPort","rpcallow*=1",
  "bootstrapchainstate=0","pruneage=-1","verifydatabasewrite=0",
  "psp0subscribe=1","psp0payout=$payout","psp1subscribe=0","psp1server=https://pool.digistamp.co","psp1payout=$payout"
)
[System.IO.File]::WriteAllText((Join-Path $TestDir 'config.cfg'), ($lines -join "`n"), (New-Object System.Text.UTF8Encoding($false)))

function RunCli($cmdArgs){ Push-Location $TestDir; $o = (& '.\DigiAssetWindows-cli.exe' @cmdArgs 2>&1 | Out-String); Pop-Location; return $o }

$proc = $null
try {
    Say "Launching merged node (asset:$AssetPort web:$WebPort event:$EventPort)..." 'Cyan'
    $proc = Start-Process -FilePath (Join-Path $TestDir 'DigiAssetWindows.exe') -WorkingDirectory $TestDir -WindowStyle Minimized -PassThru

    # Wait for the asset RPC to answer
    $up=$false
    for($i=0;$i -lt 90 -and -not $up;$i++){
        Start-Sleep -Seconds 2
        $r = RunCli @('getblockcount')
        if($r -match '\d'){ $up=$true }
    }
    Check "node starts + asset RPC answers" $up
    if($up){
        # getnewaddress -> expect an address (needs a loaded wallet on Core)
        $o = RunCli @('getnewaddress'); Check "getnewaddress returns an address" ($o -match '(dgb1|D|S)[0-9A-Za-z]{20,}')
        # getwalletbalances -> registered
        $o = RunCli @('getwalletbalances'); Check "getwalletbalances registered" ($o -notmatch '(?i)method not found|unknown method')
        # mutating methods: registration probe only (no valid args -> param error, NOT method-not-found)
        foreach($m in 'issueasset','reissueasset','burnasset','sendasset','sendmanyassets'){
            $o = RunCli @($m); Check "$m registered (dispatches, not method-not-found)" ($o -notmatch '(?i)method not found|unknown method')
        }
        # event stream: TCP connect
        $tcpOk=$false; $tcp=$null
        try{ $tcp = New-Object System.Net.Sockets.TcpClient; $tcp.Connect('127.0.0.1',$EventPort); $tcpOk=$tcp.Connected }catch{}
        Check "event stream port $EventPort accepts connections" $tcpOk
        if($tcpOk){
            try{ $tcp.ReceiveTimeout=8000; $ns=$tcp.GetStream(); $buf=New-Object byte[] 512; $n=$ns.Read($buf,0,512)
                 if($n -gt 0){ $t=[Text.Encoding]::ASCII.GetString($buf,0,$n).Trim(); Say "  event stream emitted: $($t.Substring(0,[Math]::Min(100,$t.Length)))" 'Gray' } }catch{}
            $tcp.Close()
        }
    }
}
finally {
    Say "Stopping test node..." 'Cyan'
    try{ RunCli @('shutdown') | Out-Null }catch{}
    if($proc){ for($i=0;$i -lt 20 -and (Get-Process -Id $proc.Id -EA SilentlyContinue);$i++){ Start-Sleep -Milliseconds 500 }
               if(Get-Process -Id $proc.Id -EA SilentlyContinue){ Stop-Process -Id $proc.Id -Force -EA SilentlyContinue } }
    $prodNode = Join-Path $ProdDir 'DigiAssetWindows.exe'
    if($prodRunning -and (Test-Path $prodNode)){ Say "Restarting production node..." 'Cyan'; Start-Process -FilePath $prodNode -WorkingDirectory $ProdDir -WindowStyle Normal }
}

Write-Host ""
$col = 'Green'; if($script:fail.Count -gt 0){ $col = 'Red' }
Say "===== PR26 smoke: $($script:pass.Count) passed, $($script:fail.Count) failed =====" $col
if($script:fail.Count){ $script:fail | ForEach-Object { Say "  FAILED: $_" 'Red' } }

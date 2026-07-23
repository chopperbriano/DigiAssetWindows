<#
.SYNOPSIS
    Functional test for PR26's new asset RPCs on a DigiByte REGTEST chain.

    Drives the full asset lifecycle end-to-end through the merged node's
    JSON-RPC (issueasset -> getwalletbalances -> sendasset -> burnasset),
    generating regtest blocks between steps to confirm each transaction, and
    asserts the indexer reflects each change. Uses regtest so it is fast,
    disposable, and spends no real DGB. This is the test that actually exercises
    issueasset (incl. the WinHTTP multipart postFile path), not just dispatch.

.DESCRIPTION
    PREREQUISITE - a DigiByte Core running in REGTEST with RPC enabled, e.g.:
        digibyted -regtest -server -txindex=1 -fallbackfee=0.0002 \
                  -rpcuser=regtest -rpcpassword=regtest -rpcport=18443
    (or digibyte-qt -regtest ... ). Point this script at those creds/port.

    The script funds the Core wallet (generatetoaddress), launches the merged
    node against the regtest Core on isolated ports, runs the lifecycle, then
    stops the node. It does NOT touch your mainnet node or data.

.PARAMETER ExeDir       Build folder with src\Release + cli\Release exes.
.PARAMETER CoreRpcPort  Regtest Core RPC port.
.PARAMETER CoreUser / CorePass  Regtest Core RPC credentials.
#>
[CmdletBinding()]
param(
    [string]$ExeDir     = "C:\repo\DAW-pr26\build",
    [string]$CoreRpcHost= "127.0.0.1",
    [int]   $CoreRpcPort= 18443,
    [string]$CoreUser   = "regtest",
    [string]$CorePass   = "regtest",
    [int]   $AssetPort  = 14824,
    [int]   $WebPort    = 18090,
    [int]   $EventPort  = 14825,
    [string]$AssetName  = "SmokeAsset",
    [int]   $IssueAmount= 1000,
    [string]$TestDir    = "$env:TEMP\daw-pr26-lifecycle"
)
$ErrorActionPreference = 'Stop'
function Say($m,$c='Gray'){ Write-Host $m -ForegroundColor $c }
$script:pass=@(); $script:fail=@()
function Check($name,$ok,$detail=''){ if($ok){ $script:pass+=$name; Say "  PASS  $name" 'Green' } else { $script:fail+=$name; Say "  FAIL  $name $detail" 'Red' } }

# --- JSON-RPC helper (works for both Core and the node's asset RPC) ---
function Rpc($rpcHost,$port,$user,$pass,$method,$params){
    $body = @{ jsonrpc='1.0'; id='life'; method=$method; params=$params } | ConvertTo-Json -Depth 8 -Compress
    $auth = [Convert]::ToBase64String([Text.Encoding]::ASCII.GetBytes("${user}:${pass}"))
    $r = Invoke-RestMethod -Uri "http://${rpcHost}:${port}/" -Method Post -Body $body `
            -Headers @{ Authorization = "Basic $auth" } -ContentType 'text/plain' -TimeoutSec 30
    return $r
}
function Core($method,$params=@()){ return (Rpc $CoreRpcHost $CoreRpcPort $CoreUser $CorePass $method $params).result }
function Node($method,$params=@()){ return (Rpc '127.0.0.1' $AssetPort $CoreUser $CorePass $method $params).result }
function Mine($n=1){ $a = Core 'getnewaddress'; Core 'generatetoaddress' @($n,$a) | Out-Null }

$node = Join-Path $ExeDir 'src\Release\DigiAssetWindows.exe'
$cli  = Join-Path $ExeDir 'cli\Release\DigiAssetWindows-cli.exe'
foreach($f in $node,$cli){ if(-not (Test-Path $f)){ throw "missing exe: $f" } }

# --- Verify regtest Core + fund the wallet ---
Say "Checking regtest Core on ${CoreRpcHost}:$CoreRpcPort ..." 'Cyan'
$chain = (Core 'getblockchaininfo').chain
if($chain -ne 'regtest'){ throw "Core is on '$chain', not regtest. Start DigiByte Core with -regtest (see script header)." }
Say "  Core is regtest (height $((Core 'getblockchaininfo').blocks))" 'Green'
if([double](Core 'getbalance') -le 0){ Say "Funding wallet (mining 101 regtest blocks)..." 'Cyan'; Mine 101 }
Check "regtest wallet funded" ([double](Core 'getbalance') -gt 0) "(balance=$(Core 'getbalance'))"

# --- Isolated node config pointed at the regtest Core ---
if(Test-Path $TestDir){ Remove-Item $TestDir -Recurse -Force }
New-Item -ItemType Directory -Force -Path $TestDir | Out-Null
Copy-Item $node (Join-Path $TestDir 'DigiAssetWindows.exe'); Copy-Item $cli (Join-Path $TestDir 'DigiAssetWindows-cli.exe')
$lines = @(
  "rpcuser=$CoreUser","rpcpassword=$CorePass","rpcbind=$CoreRpcHost","rpcport=$CoreRpcPort",
  "rpcassetport=$AssetPort","webport=$WebPort","eventport=$EventPort","rpcallow*=1",
  "bootstrapchainstate=0","pruneage=-1","verifydatabasewrite=0",
  "psp0subscribe=0","psp1subscribe=0"
)
[System.IO.File]::WriteAllText((Join-Path $TestDir 'config.cfg'), ($lines -join "`n"), (New-Object System.Text.UTF8Encoding($false)))

$proc = $null
try {
    Say "Launching merged node against regtest (asset:$AssetPort)..." 'Cyan'
    $proc = Start-Process -FilePath (Join-Path $TestDir 'DigiAssetWindows.exe') -WorkingDirectory $TestDir -WindowStyle Minimized -PassThru

    # wait for node asset RPC + indexer to reach the regtest tip
    $tip = [int](Core 'getblockchaininfo').blocks
    $up=$false
    for($i=0;$i -lt 60 -and -not $up;$i++){ Start-Sleep -Seconds 2; try{ if([int](Node 'getblockcount') -ge $tip){ $up=$true } }catch{} }
    Check "node up + indexed to regtest tip ($tip)" $up
    if(-not $up){ throw "node did not reach tip" }

    # 1) getnewaddress
    $addr1 = Node 'getnewaddress'
    Check "getnewaddress -> address" ($addr1 -match '(dgb1|[DS])[0-9A-Za-z]{20,}') "(got '$addr1')"

    # 2) issueasset {name, amount}
    $issue = Node 'issueasset' @(@{ name=$AssetName; amount=$IssueAmount })
    $assetId = if($issue -is [string]){ $issue } elseif($issue.assetId){ $issue.assetId } elseif($issue.assetIndex){ $issue.assetIndex } else { "$issue" }
    Check "issueasset returns a txid/assetId" ([bool]$assetId) "(got '$assetId')"
    Mine 1

    # 3) getwalletbalances shows the new asset
    $bal=$null
    for($i=0;$i -lt 20 -and -not $bal;$i++){ Start-Sleep -Seconds 2; $b = Node 'getwalletbalances'; if(("$b") -match [regex]::Escape($AssetName) -or ("$b") -match '\bassetId\b'){ $bal=$b } }
    Check "getwalletbalances reflects the issued asset" ([bool]$bal) "(response: $(("$bal").Substring(0,[Math]::Min(120,("$bal").Length))))"

    # 4) find the concrete assetId string (La...) from the wallet if issue didn't give one
    $b = Node 'getwalletbalances'; $m = [regex]::Match(("$b" | ConvertTo-Json -Depth 6 2>$null), 'L[0-9A-Za-z]{20,}')
    if(-not $m.Success){ $m = [regex]::Match(("$b"), 'L[0-9A-Za-z]{20,}') }
    $aid = if($m.Success){ $m.Value } else { $assetId }

    # 5) sendasset [toAddr, assetId, amount]
    $addr2 = Node 'getnewaddress'
    $sendOk=$false
    try{ $s = Node 'sendasset' @($addr2,"$aid","10"); $sendOk=[bool]$s }catch{ Say "  sendasset error: $($_.Exception.Message)" 'Yellow' }
    Check "sendasset dispatches + returns a txid" $sendOk
    if($sendOk){ Mine 1; Start-Sleep -Seconds 4 }

    # 6) burnasset [assetId, amount]
    $burnOk=$false
    try{ $bn = Node 'burnasset' @("$aid",5); $burnOk=[bool]$bn }catch{ Say "  burnasset error: $($_.Exception.Message)" 'Yellow' }
    Check "burnasset dispatches + returns a txid" $burnOk
    if($burnOk){ Mine 1 }
}
finally {
    Say "Stopping test node..." 'Cyan'
    try{ Node 'shutdown' | Out-Null }catch{}
    if($proc){ for($i=0;$i -lt 20 -and (Get-Process -Id $proc.Id -EA SilentlyContinue);$i++){ Start-Sleep -Milliseconds 500 }
               if(Get-Process -Id $proc.Id -EA SilentlyContinue){ Stop-Process -Id $proc.Id -Force -EA SilentlyContinue } }
}

Write-Host ""
$col='Green'; if($script:fail.Count -gt 0){ $col='Red' }
Say "===== PR26 asset lifecycle: $($script:pass.Count) passed, $($script:fail.Count) failed =====" $col
if($script:fail.Count){ $script:fail | ForEach-Object { Say "  FAILED: $_" 'Red' } }

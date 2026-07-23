<#
.SYNOPSIS
    (Re)capture the chain-split-2026 regression fixtures from a synced DigiByte
    Core: the first N Groestl blocks (the anomalous version 0x00000400 blocks
    from the June 2026 split, starting at 23,751,096) and a current v8/v9
    getrawtransaction scriptPubKey (singular 'address'). Writes to
    tests/fixtures/chain-split-2026/. See that folder's README for context.

.PARAMETER CoreConf   digibyte.conf / config.cfg to read RPC creds from.
.PARAMETER FirstGroestl  Height to start scanning for Groestl blocks.
.PARAMETER Count      How many Groestl blocks to capture.
#>
[CmdletBinding()]
param(
    [string]$CoreConf = "$env:APPDATA\DigiByte\digibyte.conf",
    [int]   $FirstGroestl = 23751096,
    [int]   $Count = 3,
    [string]$OutDir = "$PSScriptRoot\..\tests\fixtures\chain-split-2026"
)
$ErrorActionPreference = 'Stop'
if(-not (Test-Path $CoreConf)){ throw "Core conf not found: $CoreConf (pass -CoreConf)" }
$c=@{}; Get-Content $CoreConf | ForEach-Object { if($_ -match '^\s*(rpcuser|rpcpassword|rpcport)=(.*)$'){ $c[$Matches[1]]=$Matches[2].Trim() } }
$port = if($c['rpcport']){ $c['rpcport'] } else { '14022' }
function Invoke-Dgb($m,$p=@()){ $b=@{jsonrpc='1.0';id='x';method=$m;params=$p}|ConvertTo-Json -Depth 6 -Compress; $a=[Convert]::ToBase64String([Text.Encoding]::ASCII.GetBytes("$($c['rpcuser']):$($c['rpcpassword'])")); (Invoke-RestMethod -Uri "http://127.0.0.1:$port/" -Method Post -Body $b -Headers @{Authorization="Basic $a"} -ContentType 'text/plain' -TimeoutSec 30).result }

New-Item -ItemType Directory -Force -Path "$OutDir\groestl-blocks","$OutDir\scriptpubkey" | Out-Null

Write-Host "Scanning for $Count Groestl blocks from $FirstGroestl ..." -ForegroundColor Cyan
$found=0; $ht=$FirstGroestl
while($found -lt $Count -and $ht -lt ($FirstGroestl+5000)){
    $b = Invoke-Dgb 'getblock' @((Invoke-Dgb 'getblockhash' @([int]$ht)),2)
    if($b.pow_algo -eq 'groestl'){
        ($b | ConvertTo-Json -Depth 12) | Set-Content -Path "$OutDir\groestl-blocks\block-$ht.json" -Encoding UTF8
        Write-Host ("  block-$ht.json  ver=0x{0:x8} nTx=$($b.nTx)" -f $b.version) -ForegroundColor Green
        $found++
    }
    $ht++
}

Write-Host "Capturing a current v8/v9 scriptPubKey ..." -ForegroundColor Cyan
$tip = Invoke-Dgb 'getblock' @((Invoke-Dgb 'getbestblockhash'),2)
$spk=$null; foreach($tx in $tip.tx){ foreach($v in $tx.vout){ if($v.scriptPubKey.address){ $spk=$v.scriptPubKey; break } }; if($spk){break} }
if($spk){ ($spk | ConvertTo-Json -Depth 6) | Set-Content -Path "$OutDir\scriptpubkey\v8-v9-address-singular.json" -Encoding UTF8; Write-Host "  v8-v9-address-singular.json (keys: $(($spk.PSObject.Properties.Name) -join ','))" -ForegroundColor Green }
Write-Host "Done. Fixtures in $OutDir" -ForegroundColor Green

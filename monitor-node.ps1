<#
.SYNOPSIS
    At-a-glance health check for a DigiStamp DigiAsset node: DigiByte Core sync,
    IPFS, the node process, internet reachability (port 4001), and whether the
    pool sees you.

.USAGE
    powershell -ExecutionPolicy Bypass -File .\monitor-node.ps1
    powershell -ExecutionPolicy Bypass -File .\monitor-node.ps1 -Watch          # refresh every 15s
    powershell -ExecutionPolicy Bypass -File .\monitor-node.ps1 -Root C:\DigiAssetWindows
#>
[CmdletBinding()]
param(
    [string]$Root = "C:\DigiAssetWindows",
    [switch]$Watch,
    [int]$Every = 15
)
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

function Read-Cfg([string]$path) {
    $h = @{}
    if (Test-Path $path) {
        foreach ($line in Get-Content $path) {
            $t = $line.Trim()
            if ($t -eq "" -or $t.StartsWith("#")) { continue }
            $i = $t.IndexOf("=")
            if ($i -gt 0) { $h[$t.Substring(0, $i).Trim()] = $t.Substring($i + 1).Trim() }
        }
    }
    return $h
}
function Line($label, $state, $detail) {
    # state: OK / WARN / FAIL / --
    $color = switch ($state) { "OK" { "Green" } "WARN" { "Yellow" } "FAIL" { "Red" } default { "Gray" } }
    $tag = "[{0,-4}]" -f $state
    Write-Host ("  {0,-22}" -f $label) -NoNewline
    Write-Host $tag -ForegroundColor $color -NoNewline
    Write-Host ("  " + $detail)
}
function BarePeer([string]$s) {
    $p = $s.LastIndexOf("/p2p/")
    if ($p -ge 0) { $s = $s.Substring($p + 5) }
    $sl = $s.IndexOf("/"); if ($sl -ge 0) { $s = $s.Substring(0, $sl) }
    return $s
}

function Show-Status {
    $cfg = Read-Cfg (Join-Path $Root "config.cfg")
    $rpcUser = $cfg["rpcuser"]; $rpcPass = $cfg["rpcpassword"]
    $rpcPort = 14022; if ($cfg["rpcport"]) { try { $rpcPort = [int]$cfg["rpcport"] } catch {} }
    $ipfsApi = $cfg["ipfspath"]; if (-not $ipfsApi) { $ipfsApi = "http://localhost:5001/api/v0/" }
    if (-not $ipfsApi.EndsWith("/")) { $ipfsApi += "/" }
    $pool = $cfg["psp1server"]; if (-not $pool) { $pool = "https://pool.digistamp.co" }
    $pool = $pool.TrimEnd("/")
    $issues = @()

    Clear-Host
    Write-Host "===== DigiStamp Node Monitor =====  ($(Get-Date -Format 'yyyy-MM-dd HH:mm:ss'))" -ForegroundColor Cyan
    Write-Host ""

    # --- DigiByte Core ---
    try {
        $b64 = [Convert]::ToBase64String([Text.Encoding]::ASCII.GetBytes("${rpcUser}:${rpcPass}"))
        $r = Invoke-RestMethod -Uri "http://127.0.0.1:$rpcPort" -Method Post -ContentType "text/plain" `
                -Headers @{ Authorization = "Basic $b64" } -TimeoutSec 6 `
                -Body '{"jsonrpc":"1.0","id":"m","method":"getblockchaininfo","params":[]}'
        $bc = $r.result
        $pct = [math]::Round([double]$bc.verificationprogress * 100, 1)
        if ($pct -ge 99.9) { Line "DigiByte Core" "OK" ("synced 100%  block {0:N0}" -f $bc.blocks) }
        else { Line "DigiByte Core" "WARN" ("syncing {0}%  block {1:N0}/{2:N0}" -f $pct, $bc.blocks, $bc.headers); $issues += "DigiByte still syncing - the node can't fully work until it's caught up." }
    } catch {
        Line "DigiByte Core" "FAIL" "not responding (is digibyted running? creds correct?)"
        $issues += "DigiByte Core is down - start it, or check rpcuser/rpcpassword in config.cfg vs digibyte.conf."
    }

    # --- IPFS ---
    $myId = ""
    try {
        $id = Invoke-RestMethod -Uri ($ipfsApi + "id") -Method Post -TimeoutSec 6
        $myId = $id.ID
        $peers = 0
        try { $sw = Invoke-RestMethod -Uri ($ipfsApi + "swarm/peers") -Method Post -TimeoutSec 6; if ($sw.Peers) { $peers = $sw.Peers.Count } } catch {}
        Line "IPFS (kubo)" "OK" ("running, {0} peers" -f $peers)
    } catch {
        Line "IPFS (kubo)" "FAIL" "API not responding on 5001 (is the IPFS daemon running?)"
        $issues += "IPFS is down - hosting + verification won't work. Start the DigiStampIPFS task."
    }

    # --- DigiAsset for Windows process ---
    if (Get-Process DigiAssetWindows -ErrorAction SilentlyContinue) { Line "DigiAsset for Windows" "OK" "running" }
    else { Line "DigiAsset for Windows" "FAIL" "not running"; $issues += "DigiAsset for Windows isn't running - start $Root\DigiAssetWindows.exe." }

    # --- Port 4001 reachability ---
    try {
        $reach = (Invoke-RestMethod "https://ifconfig.co/port/4001" -TimeoutSec 12).reachable
        if ($reach -eq $true) { Line "Port 4001" "OK" "open to the internet" }
        else { Line "Port 4001" "WARN" "NOT reachable - forward TCP 4001 on your router"; $issues += "Port 4001 isn't reachable - forward it on your home router or you may not be verified/paid." }
    } catch { Line "Port 4001" "--" "could not run the online test right now" }

    # --- Pool registration / self-check ---
    try {
        $nodesJson = (Invoke-WebRequest "$pool/nodes.json" -UseBasicParsing -TimeoutSec 12).Content
        $ids = [regex]::Matches($nodesJson, '"id"\s*:\s*"([^"]+)"') | ForEach-Object { BarePeer $_.Groups[1].Value }
        $count = $ids.Count
        if ($myId -and ($ids -contains $myId)) { Line "Pool" "OK" ("REGISTERED - you're in ({0} node(s) online)" -f $count) }
        elseif ($myId) { Line "Pool" "WARN" ("not listed yet ({0} node(s) online)" -f $count); $issues += "Your node isn't in the pool list yet - it registers after DigiByte syncs and port 4001 is open." }
        else { Line "Pool" "--" ("{0} node(s) online (can't self-check without IPFS)" -f $count) }
    } catch { Line "Pool" "--" "pool /nodes.json unreachable" }

    # --- Payout address ---
    if ($cfg["psp1payout"]) { Line "Payout address" "OK" $cfg["psp1payout"] }
    else { Line "Payout address" "WARN" "not set in config.cfg"; $issues += "No payout address set - you won't be paid. Set psp1payout in config.cfg." }

    Write-Host ""
    if ($issues.Count -eq 0) { Write-Host "Everything looks healthy. Leave it running." -ForegroundColor Green }
    else {
        Write-Host "Things to fix:" -ForegroundColor Yellow
        foreach ($i in $issues) { Write-Host "  - $i" }
    }
    Write-Host ""
    Write-Host "Pool + earnings: $pool"
}

if ($Watch) {
    while ($true) { Show-Status; Write-Host "(refreshing every $Every s - press Ctrl+C to stop)" -ForegroundColor DarkGray; Start-Sleep -Seconds $Every }
} else {
    Show-Status
}

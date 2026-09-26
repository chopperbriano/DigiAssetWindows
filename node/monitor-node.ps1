<#
.SYNOPSIS
    At-a-glance health check for a DigiStamp DigiAsset node: DigiByte Core sync
    (and whether it agrees with the public network - height + block hash, so a
    stale fork shows up), the DigiAsset node's own sync + IPFS backlog, IPFS peers /
    version / whether it serves content, the pool's permanent list and how much of
    it is pinned here, internet reachability (port 4001), and whether the pool sees you.
    In -Watch mode it also flags a DigiAsset sync that has stopped advancing.

    The node checks use its RPC (port 14024) and need rpcallow for syncstate,
    getnodestats and getipfscount (rpcallow*=1 covers them).

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
$ScriptVersion = '1.3.0'

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
    Write-Host ("  {0,-22}" -f $label) -ForegroundColor White -NoNewline
    Write-Host $tag -ForegroundColor $color -NoNewline
    Write-Host ("  " + $detail) -ForegroundColor Gray
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
    # New nodes are configured on the psp2 (DigiStamp) slot; psp1 is the legacy
    # slot kept only for old installs. Read psp2 first, fall back to psp1, then the
    # public default - otherwise a correctly-configured node looks misconfigured.
    $pool = $cfg["psp2server"]; if (-not $pool) { $pool = $cfg["psp1server"] }; if (-not $pool) { $pool = "https://pool.digistamp.co" }
    $pool = $pool.TrimEnd("/")
    $issues = @()

    Clear-Host
    Write-Host "===== DigiStamp Node Monitor  (v$ScriptVersion) =====  ($(Get-Date -Format 'yyyy-MM-dd HH:mm:ss'))" -ForegroundColor Cyan
    Write-Host ""

    # --- DigiByte Core ---
    $b64 = [Convert]::ToBase64String([Text.Encoding]::ASCII.GetBytes("${rpcUser}:${rpcPass}"))
    function Dgb($method, $params = '[]') {
        (Invoke-RestMethod -Uri "http://127.0.0.1:$rpcPort" -Method Post -ContentType "text/plain" `
            -Headers @{ Authorization = "Basic $b64" } -TimeoutSec 6 `
            -Body ('{"jsonrpc":"1.0","id":"m","method":"' + $method + '","params":' + $params + '}')).result
    }
    $dgbHeight = 0
    try {
        $bc = Dgb 'getblockchaininfo'
        $dgbHeight = [int]$bc.blocks
        $pct = [math]::Round([double]$bc.verificationprogress * 100, 1)
        if ($pct -ge 99.9) { Line "DigiByte Core" "OK" ("synced 100%  block {0:N0}" -f $bc.blocks) }
        else { Line "DigiByte Core" "WARN" ("syncing {0}%  block {1:N0}/{2:N0}" -f $pct, $bc.blocks, $bc.headers); $issues += "DigiByte still syncing - the node can't fully work until it's caught up." }
    } catch {
        Line "DigiByte Core" "FAIL" "not responding (is the DigiByte wallet running? creds correct?)"
        $issues += "DigiByte Core is down - open the DigiByte wallet, or check rpcuser/rpcpassword in config.cfg vs digibyte.conf."
    }

    if ($dgbHeight -gt 0) {
        # Version + peers. Older than 9.26.4 stalls on the version-0x400 Groestl
        # blocks after the 2026 chain split (block 23,751,096), which "synced 100%"
        # above can't show - the node just thinks it is at the tip of a shorter chain.
        try {
            $ni = Dgb 'getnetworkinfo'
            $ver = ($ni.subversion -replace '[^0-9\.]', '').Trim('.')
            $conns = [int]$ni.connections
            $old = $false; try { $old = ([version]$ver -lt [version]'9.26.4') } catch {}
            if ($old) { Line "DigiByte version" "FAIL" "$ver - too old, stalls on post-split Groestl blocks"; $issues += "DigiByte Core $ver predates the 2026 chain split fix - update to 9.26.4 or newer (re-run the installer)." }
            elseif ($conns -lt 4) { Line "DigiByte peers" "WARN" "$ver, only $conns connection(s)"; $issues += "DigiByte has only $conns peer connection(s) - it can fall behind or follow a stale tip. Check the internet connection / port 12024." }
            else { Line "DigiByte peers" "OK" "$ver, $conns connections" }
        } catch {}

        # Compare against public explorers: same height AND the same block hash a
        # few blocks back. A node on a stale fork can be "fully synced" by its own
        # measure while every other node disagrees - this is the only check that
        # sees that. Cached ~3 min in -Watch mode so the explorers aren't hammered.
        if (-not $script:refCache -or ((Get-Date) - $script:refCache.time).TotalSeconds -ge 180) {
            $ref = 0
            foreach ($u in 'https://digiexplorer.info/api/blocks/tip/height','https://chainz.cryptoid.info/dgb/api.dws?q=getblockcount') {
                try { $v = [int]("$(Invoke-RestMethod $u -TimeoutSec 10)".Trim()); if ($v -gt $ref) { $ref = $v } } catch {}
            }
            $script:refCache = @{ height = $ref; time = (Get-Date) }
        }
        $ref = $script:refCache.height
        if ($ref -gt 0) {
            $gap = $ref - $dgbHeight
            $hashNote = ''
            $checkAt = [math]::Min($dgbHeight, $ref) - 6
            try {
                $mine = Dgb 'getblockhash' "[$checkAt]"
                $theirs = "$(Invoke-RestMethod "https://digiexplorer.info/api/block-height/$checkAt" -TimeoutSec 10)".Trim()
                if ($theirs -match '^[0-9a-f]{64}$') {
                    if ($mine -eq $theirs) { $hashNote = ", block $checkAt hash matches" }
                    else { $hashNote = 'MISMATCH' }
                }
            } catch {}
            if ($hashNote -eq 'MISMATCH') {
                Line "Chain vs network" "FAIL" "block $checkAt hash differs from digiexplorer.info - this node is on a different fork"
                $issues += "DigiByte is on a different fork from the network (block $checkAt hash mismatch). Update DigiByte Core, then reconsiderblock/reindex - assets it indexes won't match other nodes."
            } elseif ($gap -gt 20) {
                Line "Chain vs network" "WARN" ("{0:N0} blocks behind the network ({1:N0}){2}" -f $gap, $ref, $hashNote)
                $issues += "DigiByte is $gap blocks behind public explorers - if this doesn't shrink it is stuck (check peers, version)."
            } else {
                Line "Chain vs network" "OK" ("at network tip ({0:N0}){1}" -f $ref, $hashNote)
            }
        } else { Line "Chain vs network" "--" "public explorers unreachable right now" }
    }

    # --- IPFS ---
    $myId = ""
    try {
        $id = Invoke-RestMethod -Uri ($ipfsApi + "id") -Method Post -TimeoutSec 6
        $myId = $id.ID
        $peers = 0
        try { $sw = Invoke-RestMethod -Uri ($ipfsApi + "swarm/peers") -Method Post -TimeoutSec 6; if ($sw.Peers) { $peers = $sw.Peers.Count } } catch {}
        if ($peers -ge 10) { Line "IPFS Desktop" "OK" ("running, {0} peers" -f $peers) }
        elseif ($peers -gt 0) { Line "IPFS Desktop" "WARN" ("running, only {0} peers" -f $peers); $issues += "IPFS has only $peers peers - content fetches will be slow or time out. Usually port 4001 isn't reachable." }
        else { Line "IPFS Desktop" "WARN" "running but 0 peers - still connecting to the IPFS network"; $issues += "IPFS has 0 peers - it can't host content yet; usually connects within a few minutes of starting." }
    } catch {
        Line "IPFS Desktop" "FAIL" "API not responding on 5001 (is the IPFS daemon running?)"
        $issues += "IPFS is down - hosting + verification won't work. Open IPFS Desktop (tray icon)."
    }
    # IPFS Desktop version: the installer's floor is 0.50.1 (older copies get upgraded at logon).
    $ipfsExe = Get-ChildItem 'C:\Users\*\AppData\Local\Programs\IPFS Desktop\IPFS Desktop.exe' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($ipfsExe) {
        $iv = [regex]::Match("$($ipfsExe.VersionInfo.ProductVersion)", '^\d+\.\d+\.\d+').Value
        $ivOld = $false; try { $ivOld = ([version]$iv -lt [version]'0.50.1') } catch {}
        if ($ivOld) { Line "IPFS Desktop version" "WARN" "$iv (0.50.1+ expected - upgraded at next logon)" }
        elseif ($iv) { Line "IPFS Desktop version" "OK" $iv }
    }
    # Serving: bitswap counters show whether other nodes actually fetch from us.
    # 0 blocks sent after a long uptime means nobody can reach this node's content.
    if ($myId) {
        try {
            $bs = Invoke-RestMethod -Uri ($ipfsApi + "stats/bitswap") -Method Post -TimeoutSec 8
            $sent = [int64]$bs.BlocksSent
            $detail = "{0:N0} blocks / {1:N1} MB served to other nodes since IPFS started" -f $sent, ([double]$bs.DataSent / 1MB)
            if ($sent -gt 0) { Line "IPFS serving" "OK" $detail } else { Line "IPFS serving" "--" "$detail (normal right after a restart)" }
        } catch {}
    }

    # --- DigiAsset for Windows process (accept the legacy exe name too) ---
    if (Get-Process DigiAssetWindows,DigiAssetCore -ErrorAction SilentlyContinue) { Line "DigiAsset for Windows" "OK" "running" }
    else { Line "DigiAsset for Windows" "FAIL" "not running"; $issues += "DigiAsset for Windows isn't running - start $Root\DigiAssetWindows.exe." }

    # --- DigiAsset node: RPC, sync behind DigiByte, IPFS backlog ---
    $assetPort = 14024; if ($cfg["rpcassetport"]) { try { $assetPort = [int]$cfg["rpcassetport"] } catch {} }
    function Node($method) {
        $r = Invoke-RestMethod -Uri "http://127.0.0.1:$assetPort" -Method Post -ContentType "application/json" `
                -Headers @{ Authorization = "Basic $b64" } -TimeoutSec 8 `
                -Body ('{"jsonrpc":"2.0","id":1,"method":"' + $method + '","params":[]}')
        if ($r.error) { throw "$($r.error.message)" }
        return $r.result
    }
    try {
        $st = Node 'syncstate'
        $state = [int]$st.sync
        $h = 0; try { $h = [int](Node 'getnodestats').syncHeight } catch {}
        if ($h -le 0 -and $state -eq 0) { $h = [int]$st.count }
        $names = @{ 1 = 'stopped'; 2 = 'initializing'; 3 = 'rewinding (reorg)'; 4 = 'optimizing' }
        $behind = if ($dgbHeight -gt 0 -and $h -gt 0) { $dgbHeight - $h } else { -$state }
        # Stuck = behind and not moving between -Watch refreshes for 10+ minutes.
        $stuck = $false
        if ($script:lastSync -and $h -gt 0 -and $h -eq $script:lastSync.height -and $behind -gt 2) {
            $stuck = ((Get-Date) - $script:lastSync.since).TotalMinutes -ge 10
        } else { $script:lastSync = @{ height = $h; since = (Get-Date) } }
        if ($names.ContainsKey($state)) {
            $lvl = if ($state -eq 1) { 'FAIL' } else { 'WARN' }
            Line "DigiAsset sync" $lvl ("{0} at block {1:N0}" -f $names[$state], $h)
            if ($state -eq 1) { $issues += "The DigiAsset chain analyzer is STOPPED - restart DigiAssetWindows.exe and read the reason in its window." }
        } elseif ($stuck) {
            Line "DigiAsset sync" "FAIL" ("stuck at block {0:N0} for {1:N0} min, {2:N0} behind DigiByte" -f $h, ((Get-Date) - $script:lastSync.since).TotalMinutes, $behind)
            $issues += "The DigiAsset node stopped advancing at block $h - check its window for the sync error; asset data from here on is missing."
        } elseif ($behind -gt 120) {
            Line "DigiAsset sync" "WARN" ("block {0:N0}, {1:N0} behind DigiByte (catching up)" -f $h, $behind)
            $issues += "The DigiAsset node is $behind blocks behind - asset data is not safe to use until it is under 120."
        } else {
            Line "DigiAsset sync" "OK" ("block {0:N0}{1}" -f $h, $(if ($behind -gt 0) { ", $behind behind" } else { ', in step with DigiByte' }))
        }
        try {
            $q = [int](Node 'getipfscount')
            if ($q -gt 500) { Line "IPFS job queue" "WARN" "$q waiting (pins/fetches backing up - each unavailable CID can hold a worker for 20 min)"; $issues += "$q IPFS jobs are queued - IPFS is slow, has few peers, or is being asked for content nobody provides." }
            else { Line "IPFS job queue" "OK" "$q waiting" }
        } catch {}
    } catch {
        # PS 5.1 puts a non-2xx reply's body in ErrorDetails, not the exception text.
        $why = "$($_.ErrorDetails.Message) $($_.Exception.Message)".Trim()
        if ($why -match 'forbidden') { Line "DigiAsset RPC" "WARN" "syncstate is blocked by rpcallow in config.cfg"; $issues += "Allow the health RPCs: add rpcallowsyncstate=1, rpcallowgetnodestats=1, rpcallowgetipfscount=1 to config.cfg (or rpcallow*=1) and restart the node." }
        else { Line "DigiAsset RPC" "FAIL" "not answering on port $assetPort ($why)"; $issues += "The DigiAsset node's RPC (port $assetPort) isn't answering - it may be starting, hung, or waiting on DigiByte." }
    }

    # --- Hosting ports (must accept INBOUND so others can connect to you) ---
    # Local Windows firewall rules (opened by the installer).
    $fwMissing = @()
    foreach ($r in "DigiStamp IPFS swarm (TCP 4001)","DigiStamp IPFS swarm (UDP 4001)","DigiByte P2P (TCP 12024)") {
        if (-not (Get-NetFirewallRule -DisplayName $r -ErrorAction SilentlyContinue)) { $fwMissing += $r }
    }
    if ($fwMissing.Count -eq 0) { Line "Local firewall" "OK" "hosting ports open (4001 TCP/UDP, 12024 TCP)" }
    else { Line "Local firewall" "WARN" ("{0} rule(s) missing - re-run the installer" -f $fwMissing.Count); $issues += "Local firewall is missing a hosting rule - re-run setup-digiasset.ps1 to re-open 4001/12024." }

    # The reachability test uses an external service (ifconfig.co). In -Watch mode
    # don't hammer it every refresh (it will rate-limit and then always read "--");
    # cache each port's result for ~3 minutes.
    function Test-Reach($port) {
        if (-not $script:reachCache) { $script:reachCache = @{} }
        $c = $script:reachCache["$port"]
        if ($c -and ((Get-Date) - $c.time).TotalSeconds -lt 180) { return $c.val }
        $v = $null
        try { $v = (Invoke-RestMethod "https://ifconfig.co/port/$port" -TimeoutSec 12).reachable } catch { $v = $null }
        $script:reachCache["$port"] = @{ val = $v; time = (Get-Date) }
        return $v
    }

    $r4001 = Test-Reach 4001
    $port4001ok = ($r4001 -eq $true)
    if ($r4001 -eq $true) { Line "Port 4001 (DigiAsset)" "OK" "reachable - hosting IPFS/DigiAsset content" }
    elseif ($r4001 -eq $false) { Line "Port 4001 (DigiAsset)" "WARN" "NOT reachable - forward TCP+UDP 4001 on your router"; $issues += "Port 4001 (DigiAsset/IPFS hosting) isn't reachable - forward TCP+UDP 4001 on your router or you may not be verified/paid." }
    else { Line "Port 4001 (DigiAsset)" "--" "could not run the online test right now" }

    $r12024 = Test-Reach 12024
    if ($r12024 -eq $true) { Line "Port 12024 (DigiByte)" "OK" "reachable - hosting DigiByte peers" }
    elseif ($r12024 -eq $false) { Line "Port 12024 (DigiByte)" "WARN" "NOT reachable - forward TCP 12024 on your router (recommended)" }
    else { Line "Port 12024 (DigiByte)" "--" "could not run the online test right now" }

    # --- Pool list: is the pool serving its permanent list, and are we pinning it? ---
    # This is what went wrong on 2026-09-26: the pool's front end stayed up (200 on
    # the homepage) while the pool app behind it returned 502 for the list, so every
    # node silently stopped pinning. Checking the list itself is the only way to see it.
    $slot = if ($cfg["psp2server"] -or $cfg["psp2permanentpage"]) { 'psp2' } else { 'psp1' }
    $page = 23; if ($cfg["${slot}permanentpage"]) { try { $page = [int]$cfg["${slot}permanentpage"] } catch {} }
    $listUrl = "$pool/permanent/$page.json"
    try {
        $resp = Invoke-WebRequest $listUrl -UseBasicParsing -TimeoutSec 15
        $cids = @([regex]::Matches($resp.Content, '"((?:Qm[1-9A-HJ-NP-Za-km-z]{44})|(?:bafy[a-z2-7]{50,})|(?:bafk[a-z2-7]{50,}))"') | ForEach-Object { $_.Groups[1].Value } | Select-Object -Unique)
        Line "Pool list" "OK" ("page $page served, {0} CIDs" -f $cids.Count)
        # Sample up to 10 of those CIDs and ask OUR IPFS whether it has them pinned.
        # pin/ls is local-only, so this is quick and says whether pinning is keeping up.
        if ($myId -and $cids.Count -gt 0) {
            $sample = $cids | Get-Random -Count ([math]::Min(10, $cids.Count))
            $pinned = 0
            foreach ($c in $sample) {
                try { Invoke-RestMethod -Uri ($ipfsApi + "pin/ls?arg=$c&type=recursive") -Method Post -TimeoutSec 8 | Out-Null; $pinned++ } catch {}
            }
            $n = @($sample).Count
            if ($pinned -eq $n) { Line "Pool CIDs pinned" "OK" "$pinned/$n sampled from page $page are pinned here" }
            elseif ($pinned -gt 0) { Line "Pool CIDs pinned" "WARN" "$pinned/$n sampled from page $page are pinned here (still working through the list, or some content has no provider)" }
            else { Line "Pool CIDs pinned" "WARN" "0/$n sampled from page $page are pinned here"; $issues += "None of the sampled pool CIDs are pinned here - check the IPFS job queue and peer count above; a fresh node needs a few hours." }
        }
    } catch {
        $code = 0; try { $code = [int]$_.Exception.Response.StatusCode } catch {}
        if ($code -ge 502 -and $code -le 504) {
            Line "Pool list" "FAIL" "HTTP $code - pool front end up, pool server behind it down"
            $issues += "The pool's permanent list returns HTTP $code - the pool server is down, so NO node is pinning anything new. Restart it on the pool box (pool\deploy\start-digistamp.ps1)."
        } elseif ($code) { Line "Pool list" "WARN" "HTTP $code from $listUrl" }
        else { Line "Pool list" "FAIL" "could not reach $listUrl ($($_.Exception.Message))"; $issues += "Can't reach the pool's permanent list from this box - check its internet connection / DNS / proxy." }
    }

    # --- Pool registration / self-check ---
    $poolRegistered = $false
    try {
        $nodesJson = (Invoke-WebRequest "$pool/nodes.json" -UseBasicParsing -TimeoutSec 12).Content
        $ids = [regex]::Matches($nodesJson, '"id"\s*:\s*"([^"]+)"') | ForEach-Object { BarePeer $_.Groups[1].Value }
        $count = $ids.Count
        if ($myId -and ($ids -contains $myId)) { $poolRegistered = $true; Line "Pool" "OK" ("REGISTERED - you're in ({0} node(s) online)" -f $count) }
        elseif ($myId) { Line "Pool" "WARN" ("not listed yet ({0} node(s) online)" -f $count); $issues += "Your node isn't in the pool list yet - it registers after DigiByte syncs and port 4001 is open." }
        else { Line "Pool" "--" ("{0} node(s) online (can't self-check without IPFS)" -f $count) }
    } catch { Line "Pool" "--" "pool /nodes.json unreachable" }

    # --- Payout address --- (psp2 slot for new nodes; psp1 for legacy installs)
    $payout = $cfg["psp2payout"]; if (-not $payout) { $payout = $cfg["psp1payout"] }
    $payoutSet = [bool]$payout
    if ($payoutSet) { Line "Payout address" "OK" $payout }
    else { Line "Payout address" "WARN" "not set in config.cfg"; $issues += "No payout address set - you won't be paid. Set psp2payout in config.cfg." }

    Write-Host ""
    if ($issues.Count -eq 0) { Write-Host "Everything looks healthy. Leave it running." -ForegroundColor Green }
    else {
        Write-Host "Things to fix:" -ForegroundColor Yellow
        foreach ($i in $issues) { Write-Host "  - $i" -ForegroundColor White }
    }

    # --- Plain-English "will I get paid?" verdict (the whole point) ---
    Write-Host ""
    if ($poolRegistered -and $port4001ok -and $payoutSet) {
        Write-Host "PAYOUT READINESS: you're set to be paid - registered with the pool, port 4001 open, and a payout address is set." -ForegroundColor Green
    } else {
        $need = @()
        if (-not $payoutSet)      { $need += 'set your payout address' }
        if (-not $port4001ok)     { $need += 'forward port 4001 on your router' }
        if (-not $poolRegistered) { $need += 'get listed in the pool (needs a full sync + port 4001)' }
        Write-Host ("PAYOUT READINESS: not yet - still need to " + ($need -join '; ') + ".") -ForegroundColor Yellow
    }
    Write-Host ""
    Write-Host "Pool + earnings: $pool" -ForegroundColor Gray
}

if ($Watch) {
    while ($true) { Show-Status; Write-Host "(refreshing every $Every s - press Ctrl+C to stop)" -ForegroundColor DarkGray; Start-Sleep -Seconds $Every }
} else {
    Show-Status
}

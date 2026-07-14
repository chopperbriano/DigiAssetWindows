<#
.SYNOPSIS
    Test the peer link between independent, peer-aware pools. Run ON a pool box.

.DESCRIPTION
    Reads pool.cfg (poolport, poolpeers, poolpeertoken) and checks, end to end:
      1. this pool serves the /peer/* API locally (the exe is built with peers),
      2. the shared token gates /peer/* (a wrong token returns 403),
      3. each PEER is reachable over its public HTTPS URL AND its Caddy proxies
         /peer/* (returns JSON, not a 404/HTML page) AND the token matches,
      4. this pool's background sync sees the peers (the "network" block in
         /pool/stats.json), and
      5. the permanent lists are converging (asset counts line up; the sync runs
         about every 15 min, so a fresh pair may still be catching up).

    Exit 1 if any hard check fails.

.PARAMETER PoolCfg  Path to pool.cfg (default C:\DigiAssetWindows\pool.cfg).
.PARAMETER PeerUrl  Test THIS url instead of the peers in pool.cfg. Point it at your
                    OWN pool's public URL to self-test the /peer/* API + Caddy route
                    + token end-to-end BEFORE a second pool exists.
.PARAMETER Token    Override the token (else read poolpeertoken from pool.cfg).

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\verify-peers.ps1
.EXAMPLE
    # self-test against your own public URL before you have a second pool:
    .\verify-peers.ps1 -PeerUrl https://pool-a.digistamp.co -Token "shared-secret"
#>
[CmdletBinding()]
param(
    [string]$PoolCfg = 'C:\DigiAssetWindows\pool.cfg',
    [string]$PeerUrl = '',
    [string]$Token   = '',
    # -TestAnnounce: force ONE on-chain announcement now (spends a tiny fee) and
    # report the txid, so you can verify the on-chain path without the weekly wait.
    [switch]$TestAnnounce
)
$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$script:pass = 0; $script:warn = 0; $script:fail = 0
function Ok($m)   { $script:pass++; Write-Host "[PASS] $m" -ForegroundColor Green }
function Warn($m) { $script:warn++; Write-Host "[WARN] $m" -ForegroundColor Yellow }
function Bad($m)  { $script:fail++; Write-Host "[FAIL] $m" -ForegroundColor Red }
function Section($t) { Write-Host ""; Write-Host "=== $t ===" -ForegroundColor Cyan }

function Read-Cfg($path) {
    $h = @{}
    if (Test-Path $path) {
        foreach ($l in Get-Content $path) {
            $t = $l.Trim()
            if ($t -eq '' -or $t.StartsWith('#')) { continue }
            $i = $l.IndexOf('=')
            if ($i -gt 0) { $h[$l.Substring(0, $i).Trim()] = $l.Substring($i + 1) }
        }
    }
    return $h
}

# GET a URL. Returns @{ code; json; text; err }. Never throws.
function Get-Url($url) {
    $r = @{ code = 0; json = $null; text = ''; err = '' }
    try {
        $resp = Invoke-WebRequest -Uri $url -UseBasicParsing -TimeoutSec 15
        $r.code = [int]$resp.StatusCode
        $r.text = "$($resp.Content)"
        try { $r.json = $r.text | ConvertFrom-Json } catch {}
    } catch {
        $resp = $_.Exception.Response
        if ($resp) { try { $r.code = [int]$resp.StatusCode.value__ } catch {} }
        $r.err = $_.Exception.Message
    }
    return $r
}

if (-not (Test-Path $PoolCfg)) { Bad "pool.cfg not found: $PoolCfg (pass -PoolCfg)"; exit 1 }
$cfg = Read-Cfg $PoolCfg
$poolPort = 14028; if ($cfg['poolport']) { try { $poolPort = [int]$cfg['poolport'].Trim() } catch {} }
$token = if ($cfg['poolpeertoken']) { $cfg['poolpeertoken'].Trim() } else { '' }
if ($Token) { $token = $Token.Trim() }   # override
$peers = @()
if ($PeerUrl) {
    # Ad-hoc / self-test: test just this URL, ignore pool.cfg's peer list.
    $u = $PeerUrl.Trim(); if ($u -notmatch '^[A-Za-z]+://') { $u = "https://$u" }
    $peers = @($u.TrimEnd('/'))
    Write-Host "(testing -PeerUrl override; ignoring poolpeers in pool.cfg)" -ForegroundColor DarkGray
} else {
    $peersRaw = if ($cfg['poolpeers']) { $cfg['poolpeers'] } else { '' }
    foreach ($p in ($peersRaw -split ',')) { $u = $p.Trim().TrimEnd('/'); if ($u) { $peers += $u } }
}

Write-Host "=== Verify peer pools ===" -ForegroundColor White
Write-Host "pool.cfg : $PoolCfg"
Write-Host "poolport : $poolPort   token: $(if ($token) { 'set' } else { 'NOT set' })   peers: $($peers.Count)"
if ($peers.Count -eq 0) { Warn "poolpeers is empty - this pool has no peers configured. Nothing to test."; exit 0 }
$tq = if ($token) { "?token=$token" } else { '' }

# ---- 1. local /peer/status -------------------------------------------------
Section "This pool's /peer/* API (local)"
$localBase = "http://127.0.0.1:$poolPort"
$loc = Get-Url "$localBase/peer/status$tq"
$localAssets = $null
if ($loc.code -eq 200 -and $loc.json -and ($null -ne $loc.json.nodesActive)) {
    $localAssets = [int]$loc.json.permanentAssets
    Ok "local /peer/status OK (version $($loc.json.version), nodesActive $($loc.json.nodesActive), permanentAssets $localAssets)"
} elseif ($loc.code -eq 0) {
    Bad "local pool not answering on $localBase - is DigiAssetPoolServer.exe running (win.94+)?"
} else {
    Bad "local /peer/status returned code $($loc.code) - is this exe win.94+ with peers built in? $($loc.err)"
}

# ---- 2. token gating -------------------------------------------------------
if ($token) {
    $badtok = Get-Url "$localBase/peer/status?token=WRONG-$([guid]::NewGuid().ToString('N').Substring(0,8))"
    if ($badtok.code -eq 403) { Ok "token gating works (wrong token -> 403)" }
    elseif ($badtok.code -eq 200) { Bad "wrong token was ACCEPTED (200) - token gating not enforced?" }
    else { Warn "wrong-token check got code $($badtok.code) (expected 403)" }
} else {
    Warn "poolpeertoken not set - /peer/* is OPEN (read-only). Set a shared token on BOTH pools."
}

# ---- 3. each peer over its public HTTPS URL --------------------------------
Section "Peers (public HTTPS)"
foreach ($peer in $peers) {
    $s = Get-Url "$peer/peer/status$tq"
    if ($s.code -eq 200 -and $s.json -and ($null -ne $s.json.nodesActive)) {
        $pa = [int]$s.json.permanentAssets
        Ok "$peer up (version $($s.json.version), nodesActive $($s.json.nodesActive), permanentAssets $pa)"
        # list-sync convergence vs local
        if ($null -ne $localAssets) {
            $diff = [math]::Abs($pa - $localAssets)
            if ($diff -eq 0) { Ok "  permanent lists match ($pa) - synced" }
            elseif ($diff -le 5) { Ok "  permanent lists within $diff (effectively synced)" }
            else { Warn "  permanent lists differ by $diff (local $localAssets / peer $pa) - sync runs ~15 min; re-run later" }
        }
        # token gating on the peer
        if ($token) {
            $pb = Get-Url "$peer/peer/status?token=WRONG-$([guid]::NewGuid().ToString('N').Substring(0,8))"
            if ($pb.code -eq 403) { Ok "  peer enforces the token (wrong -> 403)" }
            elseif ($pb.code -eq 200) { Warn "  peer accepted a WRONG token - its poolpeertoken differs or is unset" }
        }
    } elseif ($s.code -eq 403) {
        Bad "$peer returned 403 - the poolpeertoken does NOT match between the pools"
    } elseif ($s.code -eq 200 -and -not $s.json) {
        Bad "$peer returned 200 but not JSON - its Caddy is serving /peer/* as a file, not proxying. Update its Caddyfile (add /peer/*) and reload Caddy."
    } elseif ($s.code -eq 404) {
        Bad "$peer /peer/status -> 404 - its Caddy isn't proxying /peer/* (update Caddyfile + reload), or the peer exe is pre-win.94."
    } elseif ($s.code -eq 0) {
        Bad "$peer unreachable (timeout/DNS/down): $($s.err)"
    } else {
        Bad "$peer /peer/status -> code $($s.code): $($s.err)"
    }
}

# ---- 4. this pool's merged network view -----------------------------------
Section "Unified network view (this pool's /pool/stats.json)"
$stats = Get-Url "$localBase/pool/stats.json"
if ($stats.json -and $stats.json.network) {
    $net = $stats.json.network
    Ok "network block present: pools=$($net.pools), nodesActive=$($net.nodesActive), paidTotal=$($net.paidTotal)"
    foreach ($pe in @($net.peers)) {
        if ($pe.up) { Ok "  sees peer $($pe.url) UP (nodesActive $($pe.nodesActive))" }
        else { Warn "  sees peer $($pe.url) DOWN - background sync hasn't reached it yet (runs ~15 min) or it's unreachable" }
    }
} else {
    Warn "no 'network' block in stats.json yet - the background sync runs ~15 min after start; re-run later."
}

# ---- 5. discovery directory ------------------------------------------------
Section "Discovery (open, display-only directory)"
$list = Get-Url "$localBase/peer/list"
if ($list.json -and ($null -ne $list.json.pools)) {
    Ok "local /peer/list returns $(@($list.json.pools).Count) pool(s)"
} else {
    Warn "local /peer/list empty/unavailable (set poolpublicurl + poolseed to join discovery)"
}
if ($stats.json -and $stats.json.network -and ($null -ne $stats.json.network.totalPools)) {
    Ok "network knows $($stats.json.network.totalPools) pool(s) total (trusted + discovered)"
    foreach ($d in @($stats.json.network.directory)) {
        if ($d.up) { Ok "  discovered: $($d.url) (nodesActive $($d.nodesActive))" }
    }
} else {
    Warn "no network.totalPools in stats.json yet - discovery gossip runs ~10 min after start."
}

# ---- 6. optional on-chain announce test ------------------------------------
if ($TestAnnounce) {
    Section "On-chain announce test (-TestAnnounce)"
    if (-not $token) {
        Bad "poolpeertoken must be set to use -TestAnnounce (it gates this fee-spending call)"
    } else {
        Say "Forcing one on-chain announcement (this spends a tiny tx fee)..."
        try {
            $r = Invoke-WebRequest -Uri "$localBase/peer/testannounce?token=$token" -Method Post -UseBasicParsing -TimeoutSec 60
            $j = $r.Content | ConvertFrom-Json
            if ($j.ok) { Ok "announced on-chain - txid $($j.txid)" ; Say "  Look for OP_RETURN 'DGSP' in a block, or this txid in the wallet. Peers pick it up on their next scan." 'Gray' }
            else { Bad "announce did not send: $($j.result)" }
        } catch {
            $code = 0; try { $code = [int]$_.Exception.Response.StatusCode.value__ } catch {}
            if ($code -eq 403) { Bad "testannounce -> 403 (token mismatch)" } else { Bad "testannounce failed: $($_.Exception.Message)" }
        }
    }
}

# ---- summary ---------------------------------------------------------------
Section "Summary"
Write-Host ("PASS {0}   WARN {1}   FAIL {2}" -f $script:pass, $script:warn, $script:fail)
if ($script:fail -gt 0) { Write-Host "Peer link has FAILURES - fix the [FAIL] items above." -ForegroundColor Red; exit 1 }
if ($script:warn -gt 0) { Write-Host "Peer link is working, with warnings (often just 'still converging' on a fresh pair)." -ForegroundColor Yellow; exit 0 }
Write-Host "Peer link fully healthy." -ForegroundColor Green
exit 0

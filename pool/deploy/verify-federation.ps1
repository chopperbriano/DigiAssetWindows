<#
.SYNOPSIS
    Validates DECENTRALISED POOL DISCOVERY end to end: that this pool announces itself in a
    DigiByte OP_RETURN, that the announcement is really on-chain and decodes back to the right
    URL, and that a SECOND pool finds it by scanning the chain rather than from any config.

.DESCRIPTION
    verify-peers.ps1 already checks that two pools which KNOW about each other can talk. This
    script checks the layer underneath: that they can find each other with no shared config at
    all, which is the part the "nobody depends on one operator" design rests on and the part
    that has never been demonstrated.

    It is read-only unless you pass -Announce, which spends one small transaction fee.

    Phases:
      1. Preflight     - both pools answer, poolpublicurl is set, wallet can fund a tx
      2. Announce      - (optional) force one announcement now and capture the txid
      3. On-chain      - fetch that tx from DigiByte Core, find the OP_RETURN, decode DGSP1
                         and assert the URL matches. This is the real proof; a pool claiming
                         it announced proves nothing on its own.
      4. Confirmation  - wait for the tx to make it into a block
      5. Discovery     - poll pool B until pool A shows up in its directory[] (discovered) or
                         peers[] (already promoted to trusted)
      6. Trust         - report which list it landed in and what that means

.PARAMETER PoolA      Base URL of the pool doing the announcing. Default: local loopback.
.PARAMETER PoolB      Base URL of the OTHER pool, the one that should discover A. Optional -
                      without it the script still proves phases 1-4.
.PARAMETER Token      poolpeertoken. Required only for -Announce (it gates the fee-spending call).
.PARAMETER Announce   Force one on-chain announcement now. SPENDS A SMALL FEE.
.PARAMETER TxId       Skip the announce and verify an announcement you already made.
.PARAMETER WaitMinutes How long to wait for confirmation and for B to discover A. Default 25.
.PARAMETER Root       Pool box data folder, used to read digibyte.conf. Default C:\DigiAssetWindows.

.EXAMPLE
    # read-only: is discovery already working between these two?
    .\verify-federation.ps1 -PoolB https://pool2.example.com

.EXAMPLE
    # full lifecycle, spends one fee
    .\verify-federation.ps1 -PoolB https://pool2.example.com -Token <poolpeertoken> -Announce

.EXAMPLE
    # verify an announcement made earlier
    .\verify-federation.ps1 -PoolB https://pool2.example.com -TxId abc123...
#>
[CmdletBinding()]
param(
    [string]$PoolA = 'http://127.0.0.1:14028',
    [string]$PoolB = '',
    [string]$Token = '',
    [switch]$Announce,
    [string]$TxId = '',
    [int]$WaitMinutes = 25,
    [string]$Root = 'C:\DigiAssetWindows'
)
$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$script:pass = 0; $script:warn = 0; $script:fail = 0
function Ok($m)      { $script:pass++; Write-Host "[PASS] $m" -ForegroundColor Green }
function Warn($m)    { $script:warn++; Write-Host "[WARN] $m" -ForegroundColor Yellow }
function Bad($m)     { $script:fail++; Write-Host "[FAIL] $m" -ForegroundColor Red }
function Say($m,$c='Gray') { Write-Host $m -ForegroundColor $c }
function Section($t) { Write-Host ""; Write-Host "=== $t ===" -ForegroundColor Cyan }

# GET returning @{code;json;text}. Never throws, so one unreachable pool cannot abort the run.
function Get-Url($url) {
    try {
        $r = Invoke-WebRequest -Uri $url -UseBasicParsing -TimeoutSec 20
        $j = $null; try { $j = $r.Content | ConvertFrom-Json } catch {}
        return @{ code = [int]$r.StatusCode; json = $j; text = $r.Content }
    } catch {
        $c = 0; try { $c = [int]$_.Exception.Response.StatusCode.value__ } catch {}
        return @{ code = $c; json = $null; text = "$($_.Exception.Message)" }
    }
}

# Returns @{ values = @{...}; error = '' }. digibyte.conf holds the RPC password and is
# deliberately ACL'd to SYSTEM + Administrators by the installer (Protect-SecretFile), so a
# non-elevated run gets Access Denied. Report that as "run elevated" rather than throwing -
# every on-chain phase depends on it and the operator needs to know which of the two it is.
function Read-Conf($path) {
    $h = @{}
    if (-not (Test-Path $path)) { return @{ values = $h; error = "not found at $path" } }
    try {
        foreach ($l in Get-Content -LiteralPath $path -ErrorAction Stop) {
            $t = $l.Trim()
            if ($t -eq '' -or $t.StartsWith('#')) { continue }
            $i = $t.IndexOf('='); if ($i -lt 1) { continue }
            $h[$t.Substring(0,$i).Trim()] = $t.Substring($i+1).Trim()
        }
    } catch [System.UnauthorizedAccessException] {
        return @{ values = $h; error = "access denied - re-run this script as Administrator (digibyte.conf is restricted to SYSTEM + Administrators)" }
    } catch {
        return @{ values = $h; error = "$($_.Exception.Message)" }
    }
    return @{ values = $h; error = '' }
}

function Invoke-DgbRpc([string]$method, [string]$paramsJson = '[]') {
    $body = '{"jsonrpc":"1.0","id":"verify-federation","method":"' + $method + '","params":' + $paramsJson + '}'
    $b64  = [Convert]::ToBase64String([Text.Encoding]::ASCII.GetBytes("$($script:rpcUser):$($script:rpcPass)"))
    $r = Invoke-RestMethod -Uri "http://127.0.0.1:$($script:rpcPort)" -Method Post -Body $body `
            -Headers @{ Authorization = "Basic $b64" } -ContentType 'text/plain' -TimeoutSec 25
    return $r.result
}

# "DGSP1" as hex - the magic the pool writes and scans for (PoolServer.cpp).
$DGSP1_HEX = '4447535031'

# Pull the announced URL back out of a raw scriptPubKey hex, the same way the pool does.
function ConvertFrom-Dgsp([string]$scriptHex) {
    if (-not $scriptHex) { return '' }
    $i = $scriptHex.ToLower().IndexOf($DGSP1_HEX)
    if ($i -lt 0) { return '' }
    $payload = $scriptHex.Substring($i + $DGSP1_HEX.Length)
    if ($payload.Length % 2 -ne 0) { $payload = $payload.Substring(0, $payload.Length - 1) }
    $sb = New-Object System.Text.StringBuilder
    for ($p = 0; $p + 1 -lt $payload.Length; $p += 2) {
        $b = [Convert]::ToByte($payload.Substring($p,2), 16)
        if ($b -lt 32 -or $b -gt 126) { break }   # stop at the first non-printable byte
        [void]$sb.Append([char]$b)
    }
    return $sb.ToString()
}

Write-Host "=== Decentralised pool discovery check ===" -ForegroundColor Cyan
Say "  Pool A (announcer): $PoolA"
Say "  Pool B (discoverer): $(if ($PoolB) { $PoolB } else { '(none given - phases 1-4 only)' })"

# ---- 1. preflight ----------------------------------------------------------
Section "1. Preflight"

$aStats = Get-Url "$PoolA/pool/stats.json"
if ($aStats.json) { Ok "pool A reachable (version $($aStats.json.version))" }
else { Bad "pool A not reachable at $PoolA/pool/stats.json - $($aStats.text)"; }

$aPeerStatus = Get-Url "$PoolA/peer/status"
$aPublicUrl = ''
if ($aPeerStatus.json) {
    $aPublicUrl = "$($aPeerStatus.json.url)"
    if ($aPublicUrl) { Ok "pool A announces itself as: $aPublicUrl" }
    else { Bad "pool A has no poolpublicurl set - it cannot announce. Set it in pool.cfg and restart." }
} else {
    Warn "pool A /peer/status unavailable - cannot confirm its public URL"
}

if ($aPublicUrl -and $aPublicUrl -match '127\.0\.0\.1|localhost') {
    Bad "poolpublicurl is a loopback address. Other pools cannot reach it, so announcing is pointless."
}
if ($aPublicUrl -and ("DGSP1$aPublicUrl").Length -gt 78) {
    Bad "poolpublicurl is too long: DGSP1+url must fit an OP_RETURN (78 bytes). Shorten the hostname."
} elseif ($aPublicUrl) {
    Ok "URL fits in an OP_RETURN ($("DGSP1$aPublicUrl".Length)/78 bytes)"
}

if ($PoolB) {
    $bStats = Get-Url "$PoolB/pool/stats.json"
    if ($bStats.json) { Ok "pool B reachable (version $($bStats.json.version))" }
    else { Bad "pool B not reachable at $PoolB/pool/stats.json - $($bStats.text)" }
}

# DigiByte Core RPC - needed to prove the announcement is genuinely on-chain.
$dgbConf = Join-Path (Split-Path $Root -Parent) 'DigiByte\digibyte.conf'
if (-not (Test-Path $dgbConf)) { $dgbConf = 'C:\DigiByte\digibyte.conf' }
$confRead = Read-Conf $dgbConf
$cfg = $confRead.values
$script:rpcUser = $cfg['rpcuser']; $script:rpcPass = $cfg['rpcpassword']
$script:rpcPort = if ($cfg['rpcport']) { [int]$cfg['rpcport'] } else { 14022 }
$coreOk = $false
if ($script:rpcUser) {
    try { $bc = Invoke-DgbRpc 'getblockcount'; $coreOk = $true; Ok "DigiByte Core RPC OK (height $bc)" }
    catch { Bad "DigiByte Core RPC failed: $($_.Exception.Message)" }
} elseif ($confRead.error) {
    Warn "cannot read $dgbConf - $($confRead.error)"
    Say "  On-chain phases (3 and 4) will be skipped. Everything else still runs." 'Yellow'
} else {
    Warn "no rpcuser in $dgbConf - on-chain verification will be skipped"
}

# A funded wallet is required for the announce; say so BEFORE spending time.
if ($coreOk -and $Announce) {
    try {
        $bal = Invoke-DgbRpc 'getbalance'
        if ([double]$bal -gt 0) { Ok "pool wallet funded ($bal DGB) - can pay the announcement fee" }
        else { Bad "pool wallet balance is 0 - fundrawtransaction will fail. Fund it before -Announce." }
    } catch { Warn "could not read wallet balance: $($_.Exception.Message)" }
}

# ---- 2. announce -----------------------------------------------------------
Section "2. Announcement"
$txid = $TxId
if ($txid) {
    Ok "using the txid you supplied: $txid"
} elseif ($Announce) {
    if (-not $Token) {
        Bad "-Announce needs -Token (poolpeertoken gates this fee-spending call)"
    } else {
        Say "Forcing one on-chain announcement (spends a small fee)..."
        try {
            $r = Invoke-WebRequest -Uri "$PoolA/peer/testannounce?token=$Token" -Method Post -UseBasicParsing -TimeoutSec 60
            $j = $r.Content | ConvertFrom-Json
            if ($j.ok -and $j.txid) { $txid = "$($j.txid)"; Ok "announced - txid $txid" }
            else { Bad "announce did not send: $($j.result)" }
        } catch {
            $c = 0; try { $c = [int]$_.Exception.Response.StatusCode.value__ } catch {}
            if ($c -eq 403) { Bad "testannounce -> 403 (token mismatch)" } else { Bad "testannounce failed: $($_.Exception.Message)" }
        }
    }
} else {
    Say "  (no -Announce and no -TxId: skipping. Announcements are weekly-gated, so a recent one may already be on-chain.)"
}

# ---- 3. prove it is really on-chain ---------------------------------------
Section "3. On-chain verification"
$confirmedInBlock = $false
if (-not $txid) {
    Warn "no txid to verify - pass -Announce or -TxId to exercise this phase"
} elseif (-not $coreOk) {
    Warn "no Core RPC - cannot verify the transaction"
} else {
    try {
        $tx = Invoke-DgbRpc 'getrawtransaction' ('["' + $txid + '",true]')
        $found = ''
        foreach ($vout in @($tx.vout)) {
            $decoded = ConvertFrom-Dgsp "$($vout.scriptPubKey.hex)"
            if ($decoded) { $found = $decoded; break }
        }
        if (-not $found) {
            Bad "transaction $txid carries no DGSP1 OP_RETURN - the announcement did not encode correctly"
        } else {
            Ok "OP_RETURN decodes to: $found"
            if ($aPublicUrl -and $found -ne $aPublicUrl) {
                Bad "decoded URL does not match pool A's poolpublicurl ($aPublicUrl) - peers would be sent to the wrong place"
            } elseif ($aPublicUrl) {
                Ok "decoded URL matches pool A's poolpublicurl exactly"
            }
        }
        $conf = 0; try { $conf = [int]$tx.confirmations } catch {}
        if ($conf -gt 0) { $confirmedInBlock = $true; Ok "confirmed in a block ($conf confirmation(s))" }
        else { Warn "still unconfirmed - waiting below" }
    } catch {
        Bad "could not fetch $txid : $($_.Exception.Message)"
    }
}

# ---- 4. wait for confirmation ---------------------------------------------
if ($txid -and $coreOk -and -not $confirmedInBlock) {
    Section "4. Waiting for confirmation"
    $deadline = (Get-Date).AddMinutes([Math]::Min($WaitMinutes, 15))
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Seconds 20
        try {
            $tx = Invoke-DgbRpc 'getrawtransaction' ('["' + $txid + '",true]')
            $conf = 0; try { $conf = [int]$tx.confirmations } catch {}
            if ($conf -gt 0) { $confirmedInBlock = $true; Ok "confirmed ($conf confirmation(s))"; break }
        } catch {}
        Write-Host "  ...still in the mempool" -ForegroundColor DarkGray
    }
    if (-not $confirmedInBlock) { Warn "not confirmed within the wait - a peer cannot see it until it is in a block" }
}

# ---- 5. does pool B discover it? ------------------------------------------
Section "5. Discovery by pool B"
if (-not $PoolB) {
    Say "  (no -PoolB given. Phases 1-4 prove the announcement side; discovery needs a second pool.)"
} else {
    $deadline = (Get-Date).AddMinutes($WaitMinutes)
    $seenIn = ''
    Say "  Polling $PoolB for up to $WaitMinutes min (its chain scan is periodic, not instant)..."
    while ((Get-Date) -lt $deadline -and -not $seenIn) {
        $s = Get-Url "$PoolB/pool/stats.json"
        if ($s.json -and $s.json.network) {
            foreach ($p in @($s.json.network.peers))     { if ("$($p.url)".TrimEnd('/') -eq "$aPublicUrl".TrimEnd('/')) { $seenIn = 'peers' } }
            foreach ($d in @($s.json.network.directory)) { if ("$($d.url)".TrimEnd('/') -eq "$aPublicUrl".TrimEnd('/')) { if (-not $seenIn) { $seenIn = 'directory' } } }
        }
        if (-not $seenIn) { Start-Sleep -Seconds 30; Write-Host "  ...not yet" -ForegroundColor DarkGray }
    }
    if ($seenIn -eq 'directory') {
        Ok "pool B DISCOVERED pool A from the chain - it appears in directory[]"
        Say "  This is the decentralised path working: B learned about A with no shared config." 'Green'
    } elseif ($seenIn -eq 'peers') {
        Ok "pool A appears in pool B's peers[] (already a TRUSTED peer)"
        Warn "that means it was configured via poolpeers, so this run did not prove chain discovery."
        Say "  To test discovery itself, remove A from B's poolpeers and re-run." 'Yellow'
    } else {
        Bad "pool B never saw pool A within $WaitMinutes min"
        Say "  Check: was the tx confirmed? is B's chain analyzer synced past that block? is A's URL reachable from B?" 'Yellow'
    }
}

# ---- 6. trust boundary -----------------------------------------------------
Section "6. Trust boundary"
if ($PoolB) {
    $s = Get-Url "$PoolB/pool/stats.json"
    if ($s.json -and $s.json.network) {
        $np = 0; try { $np = [int]$s.json.network.pools } catch {}
        $tp = 0; try { $tp = [int]$s.json.network.totalPools } catch {}
        Ok "pool B network view: pools=$np (trusted), totalPools=$tp (trusted + discovered)"
        if ($tp -gt $np) { Ok "discovered pools are counted separately from trusted ones - correct separation" }
    }
}
Say "  Reminder: a DISCOVERED pool is display-only. It is never mirrored and never used for" 'Gray'
Say "  payout dedup until an operator adds it to poolpeers by hand. That is deliberate -" 'Gray'
Say "  anyone can put a DGSP1 OP_RETURN on-chain for the price of one transaction." 'Gray'

# ---- summary ---------------------------------------------------------------
Section "Summary"
Write-Host ("PASS {0}   WARN {1}   FAIL {2}" -f $script:pass, $script:warn, $script:fail)
if ($script:fail -gt 0) {
    Write-Host "Decentralised discovery is NOT proven - fix the [FAIL] items above." -ForegroundColor Red
    exit 1
}
if ($script:warn -gt 0) {
    Write-Host "No failures, but some phases were skipped or still converging - see [WARN] above." -ForegroundColor Yellow
    exit 0
}
Write-Host "Decentralised pool discovery verified end to end." -ForegroundColor Green
exit 0

<#
.SYNOPSIS
    verify-pool-stack.ps1 - one-command health check for the whole stack:
    DigiByte Core + all indexes, the DigiAsset (DigiAssetWindows) node, IPFS,
    and (if present) the pool server. Confirms the three config files agree and
    that every index a full DigiByte node needs is ENABLED and CURRENT.

.DESCRIPTION
    A DigiAsset node is also a full DigiByte node. To support every DigiByte
    feature - old SPV wallets (BIP37 bloom), modern light clients (BIP157/158
    block filters), OP_RETURN/DigiAsset data, coin-stats, and DigiDollar - the
    matching indexes must be turned on in digibyte.conf AND synced to the tip.

    This script:
      1. Parses digibyte.conf, config.cfg and pool.cfg and cross-checks that the
         RPC user/password/port (and the node<->pool port) all agree.
      2. Calls DigiByte Core RPC to confirm it is reachable, synced, and that
         every required index reports synced=True.
      3. Checks the payout wallet, IPFS API, the node's own API port, and the
         pool port.
      4. Flags any config value that looks corrupted by an inline '# comment'
         or trailing whitespace (the parser keeps everything after '=').

    With -Fix it APPENDS any missing required setting to digibyte.conf (never
    overwrites an existing value) and tells you to restart Core so the new
    indexes build. Re-running setup-digiasset.ps1 does the same top-up.

.PARAMETER DigiByteDir  Folder holding digibyte.conf (default C:\DigiByte).
.PARAMETER NodeDir      Folder holding config.cfg / pool.cfg (default C:\DigiAssetWindows).
.PARAMETER Fix          Append missing required digibyte.conf settings (backs up first).

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\verify-pool-stack.ps1
.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\verify-pool-stack.ps1 -Fix
#>
[CmdletBinding()]
param(
    [string]$DigiByteDir = 'C:\DigiByte',
    [string]$NodeDir     = 'C:\DigiAssetWindows',
    [switch]$Fix
)

$ErrorActionPreference = 'Stop'

# The canonical "full DigiByte node supporting all aspects, new and old" set.
# Keep this in sync with $required in setup-digiasset.ps1. Value shown is what
# a missing key is set to by -Fix; an existing-but-different value is only
# WARNED about (never overwritten), because it may be intentional.
$requiredConf = [ordered]@{
    'server'               = '1'          # accept RPC
    'listen'               = '1'          # accept inbound P2P peers
    'prune'                = '0'          # MUST be 0 - pruning is incompatible with txindex
    'txindex'              = '1'          # full tx lookup by id (DigiAsset needs this)
    'coinstatsindex'       = '1'          # fast UTXO-set / supply stats
    'blockfilterindex'     = 'basic'      # BIP157/158 compact block filters (modern light clients)
    'peerblockfilters'     = '1'          # serve those filters to peers (new)
    'peerbloomfilters'     = '1'          # BIP37 bloom filters (old SPV wallets)
    'datacarrier'          = '1'          # relay OP_RETURN - where DigiAsset data lives
    'deprecatedrpc'        = 'addresses'  # DigiAsset reads the "addresses" field from RPC
    'digidollar'           = '1'          # DigiDollar consensus tracking (newest feature)
    'digidollarstatsindex' = '1'          # DigiDollar stats index
    'rpcthreads'           = '16'         # keep up with the node's parallel RPC calls
    'rpcworkqueue'         = '128'        # avoid 503 "work queue depth exceeded" under load
    'disablewallet'        = '0'          # pool box receives + pays DGB - wallet must stay on
}

# Indexes we expect getindexinfo to report (name substrings, case-insensitive).
$expectedIndexes = @('txindex', 'coinstatsindex', 'block filter', 'digidollar')

# ---- result tracking -------------------------------------------------------
$script:pass = 0; $script:warn = 0; $script:fail = 0
function Ok  ($n, $d) { $script:pass++; Write-Host ("[PASS] " + $n) -ForegroundColor Green;  if ($d) { Write-Host ("       " + $d) -ForegroundColor DarkGray } }
function Warn($n, $d) { $script:warn++; Write-Host ("[WARN] " + $n) -ForegroundColor Yellow; if ($d) { Write-Host ("       " + $d) -ForegroundColor DarkGray } }
function Bad ($n, $d) { $script:fail++; Write-Host ("[FAIL] " + $n) -ForegroundColor Red;    if ($d) { Write-Host ("       " + $d) -ForegroundColor DarkGray } }
function Section($t)  { Write-Host ""; Write-Host ("=== " + $t + " ===") -ForegroundColor Cyan }

# ---- config parser (mirrors the C++/app parser: value = everything after the
#      first '=', last duplicate key wins, full-line '#' comments skipped) -----
function Read-Conf($path) {
    $h = [ordered]@{}
    if (-not (Test-Path $path)) { return $h }
    foreach ($line in (Get-Content -Path $path -Encoding ASCII)) {
        $t = $line.Trim()
        if ($t -eq '' -or $t.StartsWith('#')) { continue }
        $i = $line.IndexOf('=')
        if ($i -lt 1) { continue }
        $k = $line.Substring(0, $i).Trim()
        $v = $line.Substring($i + 1)      # NOT trimmed - matches how the app reads it
        $h[$k] = $v
    }
    return $h
}

# A value is "suspicious" if it carries an inline comment or trailing whitespace,
# because the parser keeps that as part of the value (this is the bug that broke
# poolwalletpassphrase).
function Test-Suspicious($v) {
    if ($null -eq $v) { return $false }
    if ($v -match '\s#') { return $true }
    if ($v -ne $v.TrimEnd()) { return $true }
    return $false
}

function New-RpcCaller($user, $pass, $port) {
    $bytes = [Text.Encoding]::ASCII.GetBytes($user.TrimEnd() + ':' + $pass.TrimEnd())
    $auth  = 'Basic ' + [Convert]::ToBase64String($bytes)
    return {
        param($method, $params = @())
        $body = @{ jsonrpc = '1.0'; id = 'verify'; method = $method; params = $params } | ConvertTo-Json -Compress
        $r = Invoke-RestMethod -Uri ("http://127.0.0.1:" + $port + "/") -Method Post `
                -Headers @{ Authorization = $auth } -Body $body -ContentType 'text/plain' -TimeoutSec 30
        return $r.result
    }.GetNewClosure()
}

function Test-Port($port) {
    try {
        $c   = New-Object System.Net.Sockets.TcpClient
        $iar = $c.BeginConnect('127.0.0.1', [int]$port, $null, $null)
        $ok  = $iar.AsyncWaitHandle.WaitOne(2000)
        if ($ok -and $c.Connected) { $c.EndConnect($iar); $c.Close(); return $true }
        $c.Close(); return $false
    } catch { return $false }
}

# ===========================================================================
Write-Host ""
Write-Host "DigiAsset / DigiByte / Pool stack verification" -ForegroundColor White
Write-Host ("digibyte.conf : " + (Join-Path $DigiByteDir 'digibyte.conf'))
Write-Host ("config.cfg    : " + (Join-Path $NodeDir 'config.cfg'))
Write-Host ("pool.cfg      : " + (Join-Path $NodeDir 'pool.cfg'))

$dgbPath  = Join-Path $DigiByteDir 'digibyte.conf'
$nodePath = Join-Path $NodeDir 'config.cfg'
$poolPath = Join-Path $NodeDir 'pool.cfg'

$dgb  = Read-Conf $dgbPath
$node = Read-Conf $nodePath
$pool = Read-Conf $poolPath
$havePool = (Test-Path $poolPath)

# ---- 1. config files exist + agree ----------------------------------------
Section "Config files agree"
if ($dgb.Count -eq 0)  { Bad "digibyte.conf not found or empty" $dgbPath }
if ($node.Count -eq 0) { Bad "config.cfg not found or empty" $nodePath }

$rpcUser = $dgb['rpcuser']; $rpcPass = $dgb['rpcpassword']
$rpcPort = $dgb['rpcport']; if (-not $rpcPort) { $rpcPort = '14022' }

if (-not $rpcUser -or -not $rpcPass) {
    Bad "digibyte.conf missing rpcuser/rpcpassword" "The node and pool cannot authenticate without these."
} else {
    # node must match core
    if ($node['rpcuser'] -eq $rpcUser -and $node['rpcpassword'] -eq $rpcPass -and (($node['rpcport'] -eq $rpcPort) -or (-not $node['rpcport']))) {
        Ok "config.cfg RPC creds match digibyte.conf"
    } else {
        Bad "config.cfg RPC creds DO NOT match digibyte.conf" ("node sees user='" + $node['rpcuser'] + "' port='" + $node['rpcport'] + "'")
    }
    if ($havePool) {
        if ($pool['rpcuser'] -eq $rpcUser -and $pool['rpcpassword'] -eq $rpcPass -and (($pool['rpcport'] -eq $rpcPort) -or (-not $pool['rpcport']))) {
            Ok "pool.cfg RPC creds match digibyte.conf"
        } else {
            Bad "pool.cfg RPC creds DO NOT match digibyte.conf" ("pool sees user='" + $pool['rpcuser'] + "' port='" + $pool['rpcport'] + "'")
        }
    }
}

# node<->pool wiring: config.cfg psp1server port must equal pool.cfg poolport
if ($havePool) {
    $poolPort = $pool['poolport']; if (-not $poolPort) { $poolPort = '14028' }
    $psp1 = $node['psp1server']
    if ($psp1 -and ($psp1 -match (':' + [regex]::Escape($poolPort) + '(/|$)'))) {
        Ok "config.cfg psp1server points at pool.cfg poolport" ($psp1 + " -> " + $poolPort)
    } elseif ($psp1 -and ($psp1 -match '^https?://(127\.0\.0\.1|localhost)')) {
        Warn "psp1server is loopback but port may not match poolport" ("psp1server=" + $psp1 + "  poolport=" + $poolPort)
    } elseif ($psp1) {
        Ok "config.cfg psp1server set (remote pool URL)" $psp1
    } else {
        Warn "config.cfg has no psp1server" "This node will not join any remote pool."
    }
}

# ---- inline-comment / trailing-space corruption ---------------------------
$suspect = @()
foreach ($pair in @(@{f='config.cfg';h=$node}, @{f='pool.cfg';h=$pool})) {
    foreach ($k in $pair.h.Keys) {
        if (Test-Suspicious $pair.h[$k]) { $suspect += ($pair.f + ": " + $k) }
    }
}
if ($suspect.Count -gt 0) {
    Warn "Value(s) look corrupted by an inline comment or trailing space" ($suspect -join ', ')
    Write-Host "       Fix: put every # comment on its OWN line; no text after the value." -ForegroundColor DarkGray
} else {
    Ok "No inline-comment / trailing-space corruption detected"
}

# ---- 2. required digibyte.conf settings -----------------------------------
Section "Full-node settings present in digibyte.conf"
$missing = [ordered]@{}
foreach ($k in $requiredConf.Keys) {
    $want = $requiredConf[$k]
    $have = $dgb[$k]
    if ($null -eq $have) {
        $missing[$k] = $want
    } elseif ($k -eq 'deprecatedrpc') {
        if ($have -notmatch 'addresses') { Warn ("deprecatedrpc does not include 'addresses'") ("have: " + $have) }
    } elseif (($k -eq 'rpcthreads' -or $k -eq 'rpcworkqueue')) {
        $hv = 0; [void][int]::TryParse($have.Trim(), [ref]$hv)
        if ($hv -lt [int]$want) { Warn ($k + " is lower than recommended") ("have=" + $have + " recommended>=" + $want) }
    } elseif ($have.Trim() -ne $want) {
        Warn ($k + " is set but not to the recommended value") ("have=" + $have + " recommended=" + $want)
    }
}
if ($missing.Count -eq 0) {
    Ok "All required full-node settings are present"
} else {
    Bad ("Missing " + $missing.Count + " required setting(s): " + ($missing.Keys -join ', ')) "Run with -Fix (or re-run setup-digiasset.ps1) to add them, then restart DigiByte Core."
}

# ---- 3. DigiByte Core live checks -----------------------------------------
Section "DigiByte Core (RPC 127.0.0.1:$rpcPort)"
$rpc = $null
if ($rpcUser -and $rpcPass) { $rpc = New-RpcCaller $rpcUser $rpcPass $rpcPort }

$coreUp = $false
if ($rpc) {
    try {
        $chain = & $rpc 'getblockchaininfo'
        $coreUp = $true
        $vp = [double]$chain.verificationprogress
        $detail = ("chain=" + $chain.chain + " blocks=" + $chain.blocks + " headers=" + $chain.headers + (" progress={0:P2}" -f $vp))
        if ($chain.pruned) { Bad "Core is PRUNED" "txindex/indexes cannot work on a pruned node. Set prune=0 and reindex." }
        if ($vp -ge 0.9999) { Ok "Core reachable and synced" $detail } else { Warn "Core reachable but still syncing" $detail }
    } catch [System.Net.WebException] {
        $resp = $_.Exception.Response
        if ($resp -and [int]$resp.StatusCode -eq 401) { Bad "Core RPC rejected the credentials (401)" "rpcuser/rpcpassword in digibyte.conf do not match what you think." }
        else { Bad "Could not reach Core RPC" $_.Exception.Message }
    } catch {
        Bad "Could not reach Core RPC" $_.Exception.Message
    }
}

if ($coreUp) {
    # indexes: enabled AND synced
    try {
        $idx = & $rpc 'getindexinfo'
        $names = @()
        foreach ($p in $idx.PSObject.Properties) {
            $names += $p.Name.ToLower()
            $s = $p.Value.synced
            $bh = $p.Value.best_block_height
            if ($s) { Ok ("index: " + $p.Name + " synced") ("height=" + $bh) }
            else    { Warn ("index: " + $p.Name + " still building") ("height=" + $bh + " - node works, some queries slower until done") }
        }
        foreach ($exp in $expectedIndexes) {
            $found = $false
            foreach ($n in $names) { if ($n -like ('*' + $exp + '*')) { $found = $true; break } }
            if (-not $found) { Bad ("expected index not enabled: " + $exp) "Enable it in digibyte.conf (-Fix), restart, and let it build." }
        }
    } catch {
        Warn "getindexinfo unavailable" $_.Exception.Message
    }

    # network / version
    try {
        $ni = & $rpc 'getnetworkinfo'
        if ([int]$ni.connections -gt 0) { Ok "Core has peers" (($ni.subversion) + " connections=" + $ni.connections) }
        else { Warn "Core has 0 peers" "Check listen=1 and that P2P 12024 is forwarded." }
    } catch { Warn "getnetworkinfo unavailable" $_.Exception.Message }

    # wallet (needed for payouts)
    try {
        $wi = & $rpc 'getwalletinfo'
        $lock = "n/a"
        if ($null -ne $wi.unlocked_until) { if ([int]$wi.unlocked_until -eq 0) { $lock = "encrypted+locked" } else { $lock = "unlocked" } }
        Ok "Payout wallet loaded" ("name='" + $wi.walletname + "' balance=" + $wi.balance + " DGB (" + $lock + ")")
    } catch {
        if ($dgb['disablewallet'] -eq '1') { Warn "No wallet (disablewallet=1)" "This box cannot receive or pay DGB." }
        else { Warn "No wallet loaded" "Payouts and payout-address generation need a loaded wallet." }
    }
}

# ---- 4. IPFS / node API / pool port ---------------------------------------
Section "IPFS, node API, and pool"
$ipfs = $node['ipfspath']; if (-not $ipfs) { $ipfs = 'http://localhost:5001/api/v0/' }
try {
    $ver = Invoke-RestMethod -Uri ($ipfs.TrimEnd('/') + '/version') -Method Post -TimeoutSec 5
    Ok "IPFS API reachable" ("version " + $ver.Version)
} catch {
    Bad "IPFS API not reachable" ($ipfs + " - is the IPFS daemon running?")
}

$assetPort = $node['rpcassetport']; if (-not $assetPort) { $assetPort = '14024' }
if (Test-Port $assetPort) { Ok ("Node API port " + $assetPort + " is listening") } else { Warn ("Node API port " + $assetPort + " not listening") "Is DigiAssetWindows.exe running?" }

if ($havePool) {
    $poolPort = $pool['poolport']; if (-not $poolPort) { $poolPort = '14028' }
    if (Test-Port $poolPort) { Ok ("Pool port " + $poolPort + " is listening") "DigiAssetPoolServer.exe is up." }
    else { Warn ("Pool port " + $poolPort + " not listening") "Is DigiAssetPoolServer.exe running?" }
}

# ---- 5. -Fix: append missing required settings ----------------------------
if ($Fix -and $missing.Count -gt 0) {
    Section "Repairing digibyte.conf (-Fix)"
    $isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltinRole]::Administrator)
    if (-not $isAdmin) {
        Bad "-Fix needs an elevated PowerShell" "Right-click PowerShell -> Run as administrator, then re-run with -Fix."
    } else {
        $bak = $dgbPath + '.bak'
        Copy-Item -Path $dgbPath -Destination $bak -Force
        # guarantee the file ends with a newline before appending
        $raw = Get-Content -Path $dgbPath -Raw
        if ($raw.Length -gt 0 -and -not $raw.EndsWith("`n")) { Add-Content -Path $dgbPath -Value "" -Encoding ASCII }
        Add-Content -Path $dgbPath -Value "" -Encoding ASCII
        Add-Content -Path $dgbPath -Value "# --- added by verify-pool-stack.ps1 (full-node settings) ---" -Encoding ASCII
        foreach ($k in $missing.Keys) {
            Add-Content -Path $dgbPath -Value ($k + '=' + $missing[$k]) -Encoding ASCII
        }
        Ok ("Appended " + $missing.Count + " setting(s)") ("backup: " + $bak)
        Write-Host "       RESTART DigiByte Core now so the new indexes build (one-time)." -ForegroundColor Yellow
    }
} elseif ($missing.Count -gt 0) {
    Write-Host ""
    Write-Host "Tip: re-run with -Fix (elevated) to add the missing settings automatically." -ForegroundColor DarkGray
}

# ---- summary --------------------------------------------------------------
Section "Summary"
Write-Host ("PASS " + $script:pass + "   WARN " + $script:warn + "   FAIL " + $script:fail)
if ($script:fail -gt 0) {
    Write-Host "Stack has FAILURES - fix the [FAIL] items above." -ForegroundColor Red
    exit 1
} elseif ($script:warn -gt 0) {
    Write-Host "Stack is working, with warnings to review." -ForegroundColor Yellow
    exit 0
} else {
    Write-Host "Stack is fully healthy." -ForegroundColor Green
    exit 0
}

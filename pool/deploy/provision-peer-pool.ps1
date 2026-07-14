<#
.SYNOPSIS
    All-in-one: turn a box that already has the base installed (DigiByte + IPFS +
    node, from setup-digiasset.ps1, fully synced) into a public POOL and pair it
    with an existing pool - in one command. Does steps 2-8 of the POOL-SETUP guide.

.DESCRIPTION
    Run on the NEW pool box in an Administrator PowerShell. It:
      1. checks DigiByte Core RPC is up (the base must be installed + synced),
      2. downloads DigiAssetPoolServer.exe + the deploy scripts if missing,
      3. writes a complete pool.cfg (RPC creds from digibyte.conf; a treasury
         address generated from the wallet; payouts on; discovery + peer wired),
      4. sets up Caddy (website + HTTPS + /peer/* proxy) for your domain,
      5. starts the stack, and
      6. runs verify-peers.ps1.
    Prompts for anything you don't pass. Prints the shared token + the exact
    command to run on the OTHER pool so both become aware of each other.

.PARAMETER Domain           This pool's domain (e.g. yourpool.com). DNS A record must point here.
.PARAMETER PeerUrl          An existing pool to pair with (e.g. https://pool.digistamp.co). Blank = skip pairing.
.PARAMETER Token            Shared secret (SAME on both pools). Generated + printed if blank.
.PARAMETER TreasuryAddress  Donation/treasury DGB address. Generated from the wallet if blank.
.PARAMETER NoPayouts        Leave payouts OFF (default turns poolpayouts=1; safe even before funding).

.EXAMPLE
    .\provision-peer-pool.ps1 -Domain yourpool.com -PeerUrl https://pool.digistamp.co
#>
[CmdletBinding()]
param(
    [string]$Domain,
    [string]$PeerUrl,
    [string]$Token,
    [string]$TreasuryAddress,
    [int]   $PoolPort   = 14028,
    [string]$PoolDir    = 'C:\DigiAssetWindows',
    [string]$DigiByteDir = 'C:\DigiByte',
    [string]$CaddyDir   = 'C:\DigiStampPool',
    [switch]$NoPayouts
)
$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$Repo = 'chopperbriano/DigiAssetWindows'
$Raw  = "https://raw.githubusercontent.com/$Repo/master"
function Say($m, $c = 'Gray') { Write-Host $m -ForegroundColor $c }
function Head($m) { Write-Host "`n=== $m ===" -ForegroundColor Cyan }

# --- elevate, preserving args --------------------------------------------------
$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) {
    if ($PSCommandPath) {
        $a = "-ExecutionPolicy Bypass -File `"$PSCommandPath`" -PoolPort $PoolPort -PoolDir `"$PoolDir`" -DigiByteDir `"$DigiByteDir`" -CaddyDir `"$CaddyDir`""
        if ($Domain)          { $a += " -Domain `"$Domain`"" }
        if ($PeerUrl)         { $a += " -PeerUrl `"$PeerUrl`"" }
        if ($Token)           { $a += " -Token `"$Token`"" }
        if ($TreasuryAddress) { $a += " -TreasuryAddress `"$TreasuryAddress`"" }
        if ($NoPayouts)       { $a += ' -NoPayouts' }
        Start-Process powershell.exe -Verb RunAs -ArgumentList $a
        return
    }
    throw 'Run this in an elevated (Administrator) PowerShell.'
}

Say "=== Provision peer pool ===" 'White'

# --- collect inputs ------------------------------------------------------------
if (-not $Domain) { $Domain = Read-Host 'Your pool DOMAIN (e.g. yourpool.com) - its DNS A record must point at THIS PC' }
$Domain = $Domain.Trim() -replace '^https?://', '' -replace '/.*$', ''
if (-not $Domain) { throw 'A domain is required.' }
$myUrl = "https://$Domain"

if (-not $PSBoundParameters.ContainsKey('PeerUrl') -and -not $PeerUrl) {
    $PeerUrl = Read-Host 'An existing pool to PAIR with (e.g. https://pool.digistamp.co) - blank to skip'
}
$PeerUrl = $PeerUrl.Trim()
if ($PeerUrl -and $PeerUrl -notmatch '^https?://') { $PeerUrl = "https://$PeerUrl" }
$PeerUrl = $PeerUrl.TrimEnd('/')

if (-not $Token) { $Token = [Convert]::ToBase64String((1..24 | ForEach-Object { Get-Random -Maximum 256 })) }

# --- 1. base check: DigiByte Core RPC --------------------------------------------
Head "1. Checking the base (DigiByte Core RPC)"
$dgbConf = Join-Path $DigiByteDir 'digibyte.conf'
if (-not (Test-Path $dgbConf)) { throw "digibyte.conf not found at $dgbConf. Run setup-digiasset.ps1 first (and let DigiByte sync)." }
$dc = @{}
foreach ($l in Get-Content $dgbConf) { $t = $l.Trim(); if ($t -and -not $t.StartsWith('#')) { $i = $t.IndexOf('='); if ($i -gt 0) { $dc[$t.Substring(0, $i).Trim()] = $t.Substring($i + 1).Trim() } } }
$ru = $dc['rpcuser']; $rp = $dc['rpcpassword']; $rport = if ($dc['rpcport']) { [int]$dc['rpcport'] } else { 14022 }
if (-not $ru -or -not $rp) { throw "rpcuser/rpcpassword missing from digibyte.conf." }
function Rpc($method, $paramsJson = '[]') {
    $b64 = [Convert]::ToBase64String([Text.Encoding]::ASCII.GetBytes("${ru}:${rp}"))
    $body = '{"jsonrpc":"1.0","id":"prov","method":"' + $method + '","params":' + $paramsJson + '}'
    return Invoke-RestMethod -Uri "http://127.0.0.1:$rport" -Method Post -Body $body -Headers @{ Authorization = "Basic $b64" } -ContentType 'text/plain' -TimeoutSec 20
}
try { $bi = (Rpc 'getblockchaininfo').result; Say ("  Core OK - chain=$($bi.chain), synced {0:P1}" -f [double]$bi.verificationprogress) 'Green' }
catch { throw "DigiByte Core RPC not answering on 127.0.0.1:$rport. Start the DigiByte wallet and let it sync, then re-run." }

# --- 2. download pool exe + deploy scripts if missing --------------------------
Head "2. Getting pool binaries + scripts"
New-Item -ItemType Directory -Force -Path $PoolDir, (Join-Path $PoolDir 'pool\deploy') | Out-Null
function Dl($url, $dst) { for ($i = 1; $i -le 3; $i++) { try { Invoke-WebRequest -Uri $url -OutFile $dst -UseBasicParsing -TimeoutSec 180; return $true } catch { Start-Sleep (2 * $i) } } return $false }
foreach ($exe in 'DigiAssetPoolServer.exe', 'DigiAssetWindows.exe', 'DigiAssetWindows-cli.exe') {
    $dst = Join-Path $PoolDir $exe
    if (-not (Test-Path $dst)) { if (Dl "https://github.com/$Repo/releases/latest/download/$exe" $dst) { Say "  downloaded $exe" 'Green' } else { Say "  WARN could not download $exe" 'Yellow' } }
}
$deploy = Join-Path $PoolDir 'pool\deploy'
foreach ($s in 'setup-caddy.ps1', 'Caddyfile', 'start-digistamp.ps1', 'add-peer.ps1', 'verify-peers.ps1', 'diagnose-website.ps1') {
    Dl "$Raw/pool/deploy/$s" (Join-Path $deploy $s) | Out-Null
}
# landing page (setup-caddy copies from here)
New-Item -ItemType Directory -Force -Path (Join-Path $deploy 'site') | Out-Null
foreach ($f in 'index.html', 'favicon.svg', 'qrcode.min.js') { Dl "$Raw/pool/deploy/site/$f" (Join-Path $deploy "site\$f") | Out-Null }
Say "  scripts + landing page in $deploy" 'Green'

# --- 3. treasury address -------------------------------------------------------
Head "3. Treasury (donation) address"
if (-not $TreasuryAddress) {
    try { $TreasuryAddress = (Rpc 'getnewaddress' '["treasury"]').result } catch {}
}
if ($TreasuryAddress) { Say "  treasury address: $TreasuryAddress" 'Green' }
else { Say "  (could not generate a treasury address - set pooldonationaddress in pool.cfg later)" 'Yellow' }

# --- 4. write pool.cfg ---------------------------------------------------------
Head "4. Writing pool.cfg"
$poolCfg = Join-Path $PoolDir 'pool.cfg'
if (Test-Path $poolCfg) { Copy-Item $poolCfg "$poolCfg.bak" -Force; Say "  backed up existing pool.cfg -> pool.cfg.bak" 'Gray' }
$payouts = if ($NoPayouts) { '0' } else { '1' }
$lines = @(
    "poolport=$PoolPort",
    "pooldbpath=pool.db",
    "rpcuser=$ru",
    "rpcpassword=$rp",
    "rpcport=$rport",
    "ipfspath=http://localhost:5001/api/v0/",
    "pooldonationaddress=$TreasuryAddress",
    "pooladdrapiprefix=https://digiexplorer.info/api/address/",
    "poolexplorertxprefix=https://digiexplorer.info/tx/",
    "poolpayouts=$payouts",
    "poolpayoutpercent=10",
    "poolpayoutperiodhours=24",
    "poolpublicurl=$myUrl",
    "poolseed=https://pool.digistamp.co",
    "poolonchain=1",
    "poolpeertoken=$Token"
)
if ($PeerUrl) { $lines += "poolpeers=$PeerUrl"; $lines += "poolpeerpayoutdedupe=1" }
Set-Content -Path $poolCfg -Value $lines -Encoding ASCII
Say "  wrote $poolCfg (payouts=$payouts, discovery on, publicurl=$myUrl)" 'Green'

# --- 5. Caddy (website + HTTPS + /peer/*) --------------------------------------
Head "5. Website + HTTPS for $Domain"
& (Join-Path $deploy 'setup-caddy.ps1') -Domain $Domain -PoolPort $PoolPort -InstallDir $CaddyDir -PoolCfg $poolCfg

# --- 6. start the stack --------------------------------------------------------
Head "6. Starting the stack"
& (Join-Path $deploy 'start-digistamp.ps1')
Start-Sleep 3

# --- 7. verify -----------------------------------------------------------------
Head "7. Verifying"
& (Join-Path $deploy 'verify-peers.ps1') -PoolCfg $poolCfg

# --- done ----------------------------------------------------------------------
Head "DONE"
Say "This pool is live at: $myUrl" 'Green'
Say "Shared token (KEEP IT): $Token" 'Yellow'
if ($PeerUrl) {
    Say "`nNow run this ON THE OTHER POOL so it's aware of you too:" 'White'
    Say "  add-peer.ps1 -PeerUrl $myUrl -Token `"$Token`"" 'Cyan'
} else {
    Say "`nNo peer set. To pair later: add-peer.ps1 -PeerUrl <other pool> -Token `"$Token`" (same token on both)." 'White'
}
if (-not $NoPayouts) { Say "`nFUND IT: send DGB to this box's DigiByte wallet - that's the treasury payouts come from. Until it has funds, nobody is paid (which is fine)." 'Yellow' }
Say "Router: forward TCP 80+443, TCP+UDP 4001, TCP 12024 to this PC if you haven't." 'Gray'

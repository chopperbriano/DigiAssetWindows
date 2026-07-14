<#
.SYNOPSIS
    Wire THIS pool to an already-deployed peer pool, in one step. Edits pool.cfg
    (poolpeers + poolpeertoken + poolpeerpayoutdedupe), makes sure Caddy proxies
    /peer/*, restarts the pool exe, and runs verify-peers.ps1.

.DESCRIPTION
    Run on each pool box that should be aware of the other. Both pools must use
    the SAME poolpeertoken. If the peer is your own second box, run this on both
    (each with the other's URL). If someone else runs the peer, agree on the token
    and have them add your URL too.

.PARAMETER PeerUrl   The peer pool's public HTTPS base URL (e.g. https://pool-b.digistamp.co).
.PARAMETER Token     The shared secret both pools present on /peer/* calls.
.PARAMETER PoolCfg   pool.cfg path (default C:\DigiAssetWindows\pool.cfg).
.PARAMETER PoolDir   Folder with DigiAssetPoolServer.exe (default C:\DigiAssetWindows).
.PARAMETER CaddyDir  Caddy folder (default C:\DigiStampPool).
.PARAMETER NoRestart Don't restart the pool exe (you'll do it yourself).
.PARAMETER NoVerify  Skip the verify-peers.ps1 check at the end.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\add-peer.ps1 -PeerUrl https://pool-b.digistamp.co -Token "shared-secret"
#>
[CmdletBinding()]
param(
    [string]$PeerUrl,
    [string]$Token,
    [string]$PoolCfg  = 'C:\DigiAssetWindows\pool.cfg',
    [string]$PoolDir  = 'C:\DigiAssetWindows',
    [string]$CaddyDir = 'C:\DigiStampPool',
    [switch]$NoRestart,
    [switch]$NoVerify
)
$ErrorActionPreference = 'Stop'
function Say($m, $c = 'Gray') { Write-Host $m -ForegroundColor $c }

# Elevate (edit pool.cfg / restart the pool + Caddy task).
$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) {
    if ($PSCommandPath) {
        $a = "-ExecutionPolicy Bypass -File `"$PSCommandPath`" -PoolCfg `"$PoolCfg`" -PoolDir `"$PoolDir`" -CaddyDir `"$CaddyDir`""
        if ($PeerUrl)   { $a += " -PeerUrl `"$PeerUrl`"" }
        if ($Token)     { $a += " -Token `"$Token`"" }
        if ($NoRestart) { $a += ' -NoRestart' }
        if ($NoVerify)  { $a += ' -NoVerify' }
        Start-Process powershell.exe -Verb RunAs -ArgumentList $a
        return
    }
    throw 'Run this in an elevated (Administrator) PowerShell.'
}

Say "=== Add peer pool ===" 'Cyan'
if (-not (Test-Path $PoolCfg)) { throw "pool.cfg not found: $PoolCfg (pass -PoolCfg)" }

# Prompt for anything missing.
if (-not $PeerUrl) { $PeerUrl = Read-Host 'Peer pool public HTTPS URL (e.g. https://pool-b.digistamp.co)' }
$PeerUrl = $PeerUrl.Trim()
if ($PeerUrl -and $PeerUrl -notmatch '^[A-Za-z]+://') { $PeerUrl = "https://$PeerUrl" }
$PeerUrl = $PeerUrl.TrimEnd('/')
if (-not $PeerUrl) { throw 'A peer URL is required.' }
if (-not $Token) { $Token = (Read-Host 'Shared peer token (SAME on both pools)').Trim() }
if (-not $Token) { throw 'A shared token is required (use the same value on both pools).' }

# --- pool.cfg: set keys as clean whole-line key=value (no inline comments) ----
$lines = @(Get-Content -Path $PoolCfg)
function Get-Key([string[]]$ls, [string]$k) {
    foreach ($l in $ls) { if ($l -match "^\s*$([regex]::Escape($k))\s*=(.*)$") { return $Matches[1] } }
    return $null
}
function Set-Key([string[]]$ls, [string]$k, [string]$v) {
    $out = @(); $found = $false
    foreach ($l in $ls) {
        if ($l -match "^\s*$([regex]::Escape($k))\s*=") { $out += "$k=$v"; $found = $true } else { $out += $l }
    }
    if (-not $found) { $out += "$k=$v" }
    return , $out
}

# poolpeers: append PeerUrl to the comma list if not already present.
$curPeers = Get-Key $lines 'poolpeers'
$peerList = @()
if ($curPeers) { foreach ($p in ($curPeers -split ',')) { $u = $p.Trim().TrimEnd('/'); if ($u) { $peerList += $u } } }
if ($peerList -notcontains $PeerUrl) { $peerList += $PeerUrl }
$lines = Set-Key $lines 'poolpeers' ($peerList -join ',')
$lines = Set-Key $lines 'poolpeertoken' $Token
if (-not (Get-Key $lines 'poolpeerpayoutdedupe')) { $lines = Set-Key $lines 'poolpeerpayoutdedupe' '1' }

Copy-Item -Path $PoolCfg -Destination "$PoolCfg.bak" -Force
Set-Content -Path $PoolCfg -Value $lines -Encoding ASCII
Say "  pool.cfg updated: poolpeers=$($peerList -join ','), poolpeertoken set (backup: $PoolCfg.bak)" 'Green'

# --- Caddy: make sure it proxies /peer/* --------------------------------------
$caddyfile = Join-Path $CaddyDir 'Caddyfile'
if (Test-Path $caddyfile) {
    $raw = Get-Content -Path $caddyfile -Raw
    if ($raw -match '/peer/\*') {
        Say "  Caddy already proxies /peer/*." 'Green'
    } else {
        $patched = [regex]::Replace($raw, '(@api\s+path[^\r\n]*)', '$1 /peer/*')
        if ($patched -ne $raw) {
            Copy-Item -Path $caddyfile -Destination "$caddyfile.bak" -Force
            Set-Content -Path $caddyfile -Value $patched -Encoding ASCII
            Say "  Caddyfile patched to proxy /peer/* (backup: $caddyfile.bak) - restarting Caddy..." 'Yellow'
            try { Stop-ScheduledTask -TaskName DigiStampCaddy -ErrorAction SilentlyContinue } catch {}
            Get-Process caddy -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
            Start-Sleep -Seconds 2
            try { Start-ScheduledTask -TaskName DigiStampCaddy } catch {}
        } else {
            Say "  Could not find the @api line to patch. Re-run setup-caddy.ps1 to regenerate the Caddyfile with /peer/*." 'Yellow'
        }
    }
} else {
    Say "  Caddyfile not found at $caddyfile - if this box serves the site, run setup-caddy.ps1 (it now includes /peer/*)." 'Yellow'
}

# --- restart the pool exe so it reads the new poolpeers ------------------------
if (-not $NoRestart) {
    $poolExe = Join-Path $PoolDir 'DigiAssetPoolServer.exe'
    if (Test-Path $poolExe) {
        Say "  Restarting DigiAssetPoolServer.exe..." 'Cyan'
        Get-Process DigiAssetPoolServer -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
        Start-Sleep -Seconds 2
        Start-Process -FilePath $poolExe -WorkingDirectory $PoolDir -WindowStyle Normal
        Start-Sleep -Seconds 3
    } else {
        Say "  DigiAssetPoolServer.exe not found in $PoolDir - restart the pool yourself so it reads poolpeers." 'Yellow'
    }
}

# --- verify ------------------------------------------------------------------
if (-not $NoVerify) {
    $vp = Join-Path (Split-Path -Parent $PSCommandPath) 'verify-peers.ps1'
    if (-not (Test-Path $vp)) {
        $vp = Join-Path $env:TEMP 'verify-peers.ps1'
        try { Invoke-WebRequest 'https://raw.githubusercontent.com/chopperbriano/DigiAssetWindows/master/pool/deploy/verify-peers.ps1' -OutFile $vp -UseBasicParsing -TimeoutSec 30 } catch {}
    }
    if (Test-Path $vp) {
        Say "`n  Running verify-peers.ps1..." 'Cyan'
        & $vp -PoolCfg $PoolCfg
    } else {
        Say "  verify-peers.ps1 not available - run it manually to check the link." 'Yellow'
    }
}

Say "`nDone. Run this on the OTHER pool too (with THIS pool's URL + the same token) so both are aware of each other." 'Green'

<#
.SYNOPSIS
    Update a DigiStamp POOL box in one step: refresh the pool deploy scripts,
    pull the latest node + pool exes from the GitHub release, then start the
    stack (node + pool + Caddy website, with a website health-check/restart).

.DESCRIPTION
    Run this on the pool machine. It:
      1. downloads the current deploy scripts (start-digistamp, verify-pool-stack,
         setup-caddy, backup-digistamp) and update-binaries.ps1 from master,
      2. runs update-binaries.ps1 -Force -IncludePool to swap in the latest
         DigiAssetWindows.exe + DigiAssetPoolServer.exe (+ cli) from the release, and
      3. runs start-digistamp.ps1 to bring the stack up and heal the website
         (skip with -NoStart).

    Self-elevates. The exes come from the release; the .ps1 scripts come from
    master (they are not shipped as release assets).

.PARAMETER Root     Pool box data folder (default C:\DigiAssetWindows).
.PARAMETER NoStart  Update files only; do not start/heal the stack afterward.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\update-pool.ps1
.EXAMPLE
    # bootstrap one-liner (fetch this script, then run it):
    iwr https://raw.githubusercontent.com/chopperbriano/DigiAssetWindows/master/pool/deploy/update-pool.ps1 -OutFile "$env:TEMP\update-pool.ps1" -UseBasicParsing; powershell -ExecutionPolicy Bypass -File "$env:TEMP\update-pool.ps1"
#>
[CmdletBinding()]
param(
    [string]$Root = 'C:\DigiAssetWindows',
    [string]$CaddyDir = 'C:\DigiStampPool',   # where Caddy serves the site from
    [switch]$NoStart
)
$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$RawBase = 'https://raw.githubusercontent.com/chopperbriano/DigiAssetWindows/master'

function Say($m, $c = 'Gray') { Write-Host $m -ForegroundColor $c }

# Elevation: updating exes + restarting the Caddy task needs admin.
$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) {
    if ($PSCommandPath) {
        $a = "-ExecutionPolicy Bypass -File `"$PSCommandPath`" -Root `"$Root`""
        if ($NoStart) { $a += ' -NoStart' }
        Start-Process powershell.exe -Verb RunAs -ArgumentList $a
        return
    }
    throw 'Run this in an elevated (Administrator) PowerShell.'
}

# Download one raw file to a destination, creating its folder, with a short retry.
function Get-Raw($rel, $dst) {
    $url = "$RawBase/$rel"
    $dir = Split-Path -Parent $dst
    if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
    for ($i = 1; $i -le 3; $i++) {
        try { Invoke-WebRequest -Uri $url -OutFile $dst -UseBasicParsing -TimeoutSec 60; Say "  updated $(Split-Path -Leaf $dst)" 'Green'; return }
        catch { Start-Sleep -Seconds (2 * $i) }
    }
    throw "Could not download $rel"
}

Say "=== Update DigiStamp pool box  ($Root) ===" 'Cyan'
if (-not (Test-Path $Root)) { throw "Pool folder not found: $Root (pass -Root)." }
$deploy = Join-Path $Root 'pool\deploy'

# 1) Refresh the scripts from master.
Say "Refreshing scripts..." 'White'
Get-Raw 'node/update-binaries.ps1'          (Join-Path $Root   'update-binaries.ps1')
Get-Raw 'pool/deploy/start-digistamp.ps1'   (Join-Path $deploy 'start-digistamp.ps1')
Get-Raw 'pool/deploy/verify-pool-stack.ps1' (Join-Path $deploy 'verify-pool-stack.ps1')
Get-Raw 'pool/deploy/verify-peers.ps1'      (Join-Path $deploy 'verify-peers.ps1')
Get-Raw 'pool/deploy/add-peer.ps1'          (Join-Path $deploy 'add-peer.ps1')
Get-Raw 'pool/deploy/diagnose-website.ps1'  (Join-Path $deploy 'diagnose-website.ps1')
Get-Raw 'pool/deploy/setup-caddy.ps1'       (Join-Path $deploy 'setup-caddy.ps1')
Get-Raw 'pool/deploy/Caddyfile'             (Join-Path $deploy 'Caddyfile')
Get-Raw 'pool/deploy/backup-digistamp.ps1'  (Join-Path $deploy 'backup-digistamp.ps1')

# Refresh the LIVE website page (Caddy serves it straight from disk, so the new
# page is live on the next request - no restart needed).
$liveSite = Join-Path $CaddyDir 'site'
if (Test-Path $liveSite) {
    Get-Raw 'pool/deploy/site/index.html' (Join-Path $liveSite 'index.html')
    Say "  live website page refreshed at $liveSite" 'Green'
} else {
    Say "  ($liveSite not found - skipping live site refresh; pass -CaddyDir if it's elsewhere)" 'Yellow'
}

# 2) Update the exes (node + pool) from the latest release. Run INLINE (& ) so
#    its progress shows in THIS window - a separate -Wait window looked "hung".
Say "Updating binaries (node + pool) from the latest release..." 'White'
$ub = Join-Path $Root 'update-binaries.ps1'
try { & $ub -Force -IncludePool }
catch { Say "  update-binaries reported: $($_.Exception.Message)" 'Yellow' }

# 3) Start the stack + restart the website, unless -NoStart. Run start-digistamp
#    LAST and inline: it launches node/pool/website in their own windows and then
#    exits, which ends this script too - so nothing sits open when it's done.
if ($NoStart) {
    Say "`nFiles updated. -NoStart given, so the stack was not started." 'Green'
    Say "Start it with:  $deploy\start-digistamp.ps1   (add -WebsiteOnly to just restart the site)" 'Gray'
    exit 0
}
Say "`nBinaries + scripts updated. Starting the stack + website now..." 'Green'
$sd = Join-Path $deploy 'start-digistamp.ps1'
& $sd   # starts everything in its own windows, then exits (which ends this script)

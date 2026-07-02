<#
.SYNOPSIS
    One-click setup for a DigiAsset Core for Windows node that earns DGB from the
    DigiStamp Permanent Storage Pool.

.WHAT IT DOES
    1. Downloads DigiAsset Core (node + cli) from the latest GitHub release.
    2. Installs IPFS (kubo): downloads it, initialises it, and runs it as a
       background service that starts on boot.
    3. Opens the LOCAL Windows firewall for the ports that must be reachable.
    4. Writes config.cfg (pool = pool.digistamp.co, your payout address) and, if
       needed, a digibyte.conf with matching RPC credentials.
    5. Tests whether your node is reachable from the internet (port 4001) and
       tells you exactly what to forward on your HOME ROUTER.

.WHAT YOU STILL NEED
    DigiByte Core (the wallet) must be installed and synced - it's a big download
    the script can't do for you. The script detects it and gives you the exact
    steps + a ready-made digibyte.conf if it's missing.

.USAGE
    Right-click > Run with PowerShell (as Administrator), or:
    powershell -ExecutionPolicy Bypass -File .\install-node.ps1
#>
[CmdletBinding()]
param(
    [string]$Root = "C:\DigiAssetWindows",
    [string]$PayoutAddress = "",
    [string]$PoolServer = "https://pool.digistamp.co"
)
$ErrorActionPreference = "Stop"
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

# --- Self-elevate to Administrator (needed for firewall + services) --------
$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) {
    if ($PSCommandPath) {
        Write-Host "Elevating to Administrator..." -ForegroundColor Yellow
        Start-Process powershell.exe -Verb RunAs -ArgumentList "-ExecutionPolicy Bypass -File `"$PSCommandPath`""
        return
    } else {
        throw "Please run this in an ELEVATED (Administrator) PowerShell window."
    }
}

$repo = "chopperbriano/DigiAsset_Core_Windows"
function Step($n, $msg) { Write-Host "`n[$n] $msg" -ForegroundColor Cyan }

Write-Host "===== DigiAsset Core for Windows - node installer =====" -ForegroundColor Green
Write-Host @"

This sets your PC up to HOST DigiAsset content and EARN DGB from the DigiStamp
pool. Here is exactly what it will do:

  * Download DigiAsset Core into $Root
  * Download + start IPFS (file storage), running automatically on boot
  * Open your local Windows firewall for port 4001 (IPFS) and 12024 (DigiByte)
  * Save a config pointed at the DigiStamp pool with YOUR payout address
  * Test whether your node is reachable from the internet

You will also need the DigiByte Core wallet installed + synced (this script
checks for it and guides you), and you will forward port 4001 on your home
router. This does NOT spend money or touch your wallet's coins - it only sets up
hosting so the pool can pay you.

"@ -ForegroundColor Gray
$go = Read-Host "Press Enter to continue, or type N then Enter to cancel"
if ($go -match '^[Nn]') { Write-Host "Cancelled - nothing was changed."; return }

New-Item -ItemType Directory -Force -Path $Root | Out-Null
Write-Host "Install folder: $Root"

# --- 0. Payout address -----------------------------------------------------
if (-not $PayoutAddress) {
    Write-Host "`n--- Your payout address ---" -ForegroundColor Cyan
    Write-Host "This is the DigiByte address where the pool sends your hosting earnings."
    Write-Host "Use an address from a wallet YOU control (your DigiByte Core wallet is fine)."
    Write-Host "It can start with D..., S..., or dgb1..."
    $PayoutAddress = Read-Host "  Paste your DGB payout address"
}
if ($PayoutAddress -notmatch '^(D|S|dgb1)[0-9A-Za-z]{6,90}$') {
    throw "That does not look like a DigiByte address. Re-run and paste a valid D..., S..., or dgb1... address."
}

# --- 1. DigiAsset Core binaries -------------------------------------------
Step 1 "Downloading DigiAsset Core (latest release)..."
foreach ($f in "DigiAssetCore.exe", "DigiAssetCore-cli.exe") {
    $url = "https://github.com/$repo/releases/latest/download/$f"
    Invoke-WebRequest -Uri $url -OutFile (Join-Path $Root $f) -UseBasicParsing
    Write-Host "  + $f"
}

# --- 2. IPFS (kubo) --------------------------------------------------------
Step 2 "Installing IPFS (kubo)..."
$ipfsExe  = Join-Path $Root "ipfs.exe"
$ipfsRepo = Join-Path $Root "ipfs-repo"
if (-not (Test-Path $ipfsExe)) {
    $versions = (Invoke-WebRequest "https://dist.ipfs.tech/kubo/versions" -UseBasicParsing).Content -split "`n"
    $ver = ($versions | Where-Object { $_ -and ($_ -notmatch "rc") } | Select-Object -Last 1).Trim()
    Write-Host "  downloading kubo $ver ..."
    $zip = Join-Path $env:TEMP "kubo.zip"
    Invoke-WebRequest "https://dist.ipfs.tech/kubo/$ver/kubo_${ver}_windows-amd64.zip" -OutFile $zip -UseBasicParsing
    $tmp = Join-Path $env:TEMP "kubo_extract"
    if (Test-Path $tmp) { Remove-Item $tmp -Recurse -Force }
    Expand-Archive $zip -DestinationPath $tmp -Force
    Copy-Item (Join-Path $tmp "kubo\ipfs.exe") -Destination $ipfsExe -Force
    Remove-Item $zip, $tmp -Recurse -Force
    Write-Host "  + ipfs.exe ($ver)"
} else {
    Write-Host "  ipfs.exe already present."
}

# Point IPFS at a repo inside our folder (machine-wide so the service sees it).
[Environment]::SetEnvironmentVariable("IPFS_PATH", $ipfsRepo, "Machine")
$env:IPFS_PATH = $ipfsRepo
if (-not (Test-Path (Join-Path $ipfsRepo "config"))) {
    Write-Host "  initialising IPFS repo..."
    & $ipfsExe init | Out-Null
}

# Announce our public IP so NAT'd nodes are still findable (best effort).
try {
    $pubip = (Invoke-RestMethod "https://api.ipify.org" -TimeoutSec 10).Trim()
    if ($pubip) { & $ipfsExe config --json Addresses.Announce "[`"/ip4/$pubip/tcp/4001`"]" | Out-Null; Write-Host "  announce set to $pubip:4001" }
} catch { Write-Host "  (could not auto-detect public IP; the app's [F] key can set this later)" }

# Run the IPFS daemon as a boot task, and start it now.
$ipfsTask = "DigiStampIPFS"
$act = New-ScheduledTaskAction -Execute $ipfsExe -Argument "daemon --enable-gc"
$trg = New-ScheduledTaskTrigger -AtLogOn
$prin = New-ScheduledTaskPrincipal -UserId ([Security.Principal.WindowsIdentity]::GetCurrent().Name) -LogonType Interactive -RunLevel Highest
$set = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -RestartCount 3 -RestartInterval (New-TimeSpan -Minutes 1)
Register-ScheduledTask -TaskName $ipfsTask -Action $act -Trigger $trg -Principal $prin -Settings $set -Force | Out-Null
if (-not (Get-Process ipfs -ErrorAction SilentlyContinue)) {
    Start-Process -FilePath $ipfsExe -ArgumentList "daemon --enable-gc" -WindowStyle Hidden
}
Write-Host "  IPFS daemon running (boot task '$ipfsTask' registered)."

# --- 3. Local Windows firewall --------------------------------------------
Step 3 "Opening local Windows firewall for the ports that need it..."
function Open-Port($name, $proto, $port) {
    if (-not (Get-NetFirewallRule -DisplayName $name -ErrorAction SilentlyContinue)) {
        New-NetFirewallRule -DisplayName $name -Direction Inbound -Action Allow -Protocol $proto -LocalPort $port | Out-Null
        Write-Host "  + allowed inbound $proto $port  ($name)"
    } else { Write-Host "  $name already exists" }
}
Open-Port "DigiStamp IPFS swarm (TCP 4001)" TCP 4001
Open-Port "DigiStamp IPFS swarm (UDP 4001)" UDP 4001
Open-Port "DigiByte P2P (TCP 12024)"        TCP 12024
Write-Host "  (Ports 5001/14022/8090 are intentionally NOT opened - they stay localhost-only.)"

# --- 4. DigiByte Core check + credentials ---------------------------------
Step 4 "Checking DigiByte Core..."
$dgbConfDir = Join-Path $env:APPDATA "DigiByte"
$dgbConf    = Join-Path $dgbConfDir "digibyte.conf"
$rpcUser = ""; $rpcPass = ""
if (Test-Path $dgbConf) {
    foreach ($line in Get-Content $dgbConf) {
        if ($line -match '^\s*rpcuser\s*=\s*(.+)$')     { $rpcUser = $Matches[1].Trim() }
        if ($line -match '^\s*rpcpassword\s*=\s*(.+)$') { $rpcPass = $Matches[1].Trim() }
    }
}
if (-not $rpcUser -or -not $rpcPass) {
    # Generate credentials and write a ready-to-use digibyte.conf.
    $rpcUser = "digiasset"
    $rpcPass = -join ((48..57) + (65..90) + (97..122) | Get-Random -Count 24 | ForEach-Object { [char]$_ })
    New-Item -ItemType Directory -Force -Path $dgbConfDir | Out-Null
    $addnodes = @("191.81.59.115","175.45.182.173","45.76.235.153","24.74.186.115","24.101.88.154","8.214.25.169","47.75.38.245")
    $conf = @("rpcuser=$rpcUser","rpcpassword=$rpcPass","rpcbind=127.0.0.1","rpcport=14022","rpcallowip=127.0.0.1","whitelist=127.0.0.1","listen=1","server=1","txindex=1","deprecatedrpc=addresses")
    $conf += ($addnodes | ForEach-Object { "addnode=$_" })
    Set-Content -Path $dgbConf -Value $conf -Encoding ASCII
    Write-Host "  Wrote $dgbConf with fresh RPC credentials." -ForegroundColor Yellow
    Write-Host "  You must (RE)START DigiByte Core for these to take effect." -ForegroundColor Yellow
} else {
    Write-Host "  Found existing RPC credentials in digibyte.conf."
}

# Is DigiByte Core actually responding?
$coreUp = $false
try {
    $b64 = [Convert]::ToBase64String([Text.Encoding]::ASCII.GetBytes("${rpcUser}:${rpcPass}"))
    $null = Invoke-RestMethod -Uri "http://127.0.0.1:14022" -Method Post -ContentType "text/plain" `
        -Headers @{ Authorization = "Basic $b64" } -TimeoutSec 6 `
        -Body '{"jsonrpc":"1.0","id":"chk","method":"getblockchaininfo","params":[]}'
    $coreUp = $true
} catch { $coreUp = $false }
if ($coreUp) { Write-Host "  DigiByte Core is running and reachable." -ForegroundColor Green }
else {
    Write-Host "  DigiByte Core is NOT responding yet." -ForegroundColor Yellow
    Write-Host "  Install it from https://github.com/DigiByte-Core/digibyte/releases (win64 setup),"
    Write-Host "  start it, let it sync, then re-run this script (it will pick up from here)."
}

# --- 5. config.cfg for DigiAsset Core -------------------------------------
Step 5 "Writing config.cfg..."
$cfg = Join-Path $Root "config.cfg"
if (Test-Path $cfg) {
    Write-Host "  config.cfg already exists - leaving it untouched."
} else {
    $lines = @(
        "rpcbind=127.0.0.1", "rpcport=14022", "rpcuser=$rpcUser", "rpcpassword=$rpcPass",
        "ipfspath=http://localhost:5001/api/v0/",
        "psp1server=$PoolServer", "psp1subscribe=1", "psp1payout=$PayoutAddress",
        "pruneage=5760", "bootstrapchainstate=1"
    )
    Set-Content -Path $cfg -Value $lines -Encoding ASCII
    Write-Host "  + config.cfg (pool=$PoolServer, payout=$PayoutAddress, pruned+bootstrap)"
}

# --- 6. Reachability test (home router) -----------------------------------
Step 6 "Testing whether port 4001 is reachable from the internet..."
Start-Sleep -Seconds 2
$reachable = $null
try {
    $r = Invoke-RestMethod "https://ifconfig.co/port/4001" -TimeoutSec 15
    $reachable = $r.reachable
} catch { Write-Host "  (could not run the online port test right now)" }
if ($reachable -eq $true) {
    Write-Host "  SUCCESS: port 4001 is OPEN from the internet. You're set to be verified + paid." -ForegroundColor Green
} else {
    Write-Host "  Port 4001 is NOT reachable from the internet yet." -ForegroundColor Yellow
    Write-Host "  --> On your HOME ROUTER, forward TCP (and ideally UDP) port 4001 to this PC's local IP,"
    Write-Host "      then re-run this script to re-test. Without it your node registers but may not verify."
}

# --- Summary ---------------------------------------------------------------
Write-Host "`n===== Setup summary =====" -ForegroundColor Green
Write-Host "Local firewall: opened TCP/UDP 4001 (IPFS) and TCP 12024 (DigiByte)."
Write-Host "HOME ROUTER - forward to this PC:"
Write-Host "   TCP 4001   (REQUIRED - IPFS swarm; how the pool verifies you)"
Write-Host "   UDP 4001   (recommended - IPFS QUIC)"
Write-Host "   TCP 12024  (optional  - more DigiByte peers)"
Write-Host "Do NOT forward 5001 / 14022 / 8090 (keep them local)."
Write-Host ""
Write-Host "Next:"
if (-not $coreUp) { Write-Host "  1. Install + start + sync DigiByte Core, then re-run this script." }
Write-Host "  * Start the node:  $Root\DigiAssetCore.exe"
Write-Host "  * In its dashboard press [N] to confirm you're registered with the pool,"
Write-Host "    and [P] to re-test port 4001. Watch the Payment row for your status."
Write-Host "  * Track the pool + your earnings at $PoolServer"

<#
.SYNOPSIS
    One-click setup for a DigiAsset Core for Windows node that earns DGB from the
    DigiStamp Permanent Storage Pool. Installs and auto-starts the WHOLE stack.

.WHAT IT DOES (all automatic)
    1. Downloads DigiAsset Core (node + cli) from the latest GitHub release.
    2. Installs DigiByte Core (the wallet) if missing, writes digibyte.conf, and
       runs it in the background - starting on every boot.
    3. Installs IPFS (kubo) and runs it in the background - starting on every boot.
    4. Opens the LOCAL Windows firewall for the ports that must be reachable.
    5. Writes config.cfg (pool = pool.digistamp.co, your payout address).
    6. Sets DigiAsset Core to start (with its dashboard) at every logon.
    7. Tests whether your node is reachable from the internet and tells you what
       to forward on your home router.

.NOTE
    DigiByte's blockchain sync takes hours the first time - that runs in the
    background after this script finishes. Re-running the script is safe; it
    skips anything already done.

.USAGE
    Right-click > Run with PowerShell (as Administrator), or:
    powershell -ExecutionPolicy Bypass -File .\install-node.ps1
#>
[CmdletBinding()]
param(
    [string]$Root = "C:\DigiAssetWindows",
    [string]$PayoutAddress = "",
    [string]$PoolServer = "https://pool.digistamp.co",
    [switch]$SkipDigiByte
)
$ErrorActionPreference = "Stop"
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

# --- Self-elevate to Administrator (needed for firewall + boot tasks) ------
$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) {
    if ($PSCommandPath) {
        Write-Host "Elevating to Administrator..." -ForegroundColor Yellow
        Start-Process powershell.exe -Verb RunAs -ArgumentList "-ExecutionPolicy Bypass -File `"$PSCommandPath`""
        return
    } else { throw "Please run this in an ELEVATED (Administrator) PowerShell window." }
}

$repo = "chopperbriano/DigiAsset_Core_Windows"
function Step($n, $msg) { Write-Host "`n[$n] $msg" -ForegroundColor Cyan }

function Register-DaemonTask($name, $exe, $arguments) {
    # Background service: runs as SYSTEM (hidden), starts at boot, auto-restarts.
    $a = New-ScheduledTaskAction -Execute $exe -Argument $arguments
    $t = New-ScheduledTaskTrigger -AtStartup
    $p = New-ScheduledTaskPrincipal -UserId "SYSTEM" -LogonType ServiceAccount -RunLevel Highest
    $s = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -RestartCount 3 -RestartInterval (New-TimeSpan -Minutes 2) -ExecutionTimeLimit ([TimeSpan]::Zero)
    Register-ScheduledTask -TaskName $name -Action $a -Trigger $t -Principal $p -Settings $s -Force | Out-Null
}
function Register-VisibleTask($name, $exe, $workdir) {
    # Runs in the user's session (visible window) at logon.
    $a = New-ScheduledTaskAction -Execute $exe -WorkingDirectory $workdir
    $t = New-ScheduledTaskTrigger -AtLogOn
    $u = [Security.Principal.WindowsIdentity]::GetCurrent().Name
    $p = New-ScheduledTaskPrincipal -UserId $u -LogonType Interactive -RunLevel Highest
    $s = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit ([TimeSpan]::Zero)
    Register-ScheduledTask -TaskName $name -Action $a -Trigger $t -Principal $p -Settings $s -Force | Out-Null
}

Write-Host "===== DigiAsset Core for Windows - node installer =====" -ForegroundColor Green
Write-Host @"

This sets your PC up to HOST DigiAsset content and EARN DGB from the DigiStamp
pool. It installs and auto-starts everything:

  * DigiAsset Core (the node)        -> into $Root
  * DigiByte Core (the wallet)       -> runs in the background, starts on boot
  * IPFS (file storage)              -> runs in the background, starts on boot
  * Opens your local firewall for port 4001 (IPFS) and 12024 (DigiByte)
  * Saves a config pointed at the pool with YOUR payout address
  * Tests whether your node is reachable from the internet

You will forward ONE port (4001) on your home router - the script shows you how.
DigiByte's blockchain sync takes hours the first time and runs in the background.

Heads up: payouts are SMALL, and you're only paid when the pool has DGB to give
(a shared tip jar funded by asset fees + donations, split among verified nodes).
Do this to help the network, not to get rich. Nothing here spends your coins.

"@ -ForegroundColor Gray
$go = Read-Host "Press Enter to continue, or type N then Enter to cancel"
if ($go -match '^[Nn]') { Write-Host "Cancelled - nothing was changed."; return }

New-Item -ItemType Directory -Force -Path $Root | Out-Null
Write-Host "Install folder: $Root"

# --- 0. Payout address -----------------------------------------------------
if (-not $PayoutAddress) {
    Write-Host "`n--- Your payout address ---" -ForegroundColor Cyan
    Write-Host "The DigiByte address where the pool sends your hosting earnings."
    Write-Host "Use an address from a wallet YOU control. It can start D..., S..., or dgb1..."
    $PayoutAddress = Read-Host "  Paste your DGB payout address"
}
if ($PayoutAddress -notmatch '^(D|S|dgb1)[0-9A-Za-z]{6,90}$') {
    throw "That does not look like a DigiByte address. Re-run and paste a valid D..., S..., or dgb1... address."
}

# --- 1. DigiAsset Core binaries -------------------------------------------
Step 1 "Downloading DigiAsset Core (latest release)..."
foreach ($f in "DigiAssetCore.exe", "DigiAssetCore-cli.exe") {
    Invoke-WebRequest -Uri "https://github.com/$repo/releases/latest/download/$f" -OutFile (Join-Path $Root $f) -UseBasicParsing
    Write-Host "  + $f"
}

# --- 2. DigiByte Core (wallet) --------------------------------------------
Step 2 "DigiByte Core..."
$dgbData = Join-Path $env:APPDATA "DigiByte"
$dgbConf = Join-Path $dgbData "digibyte.conf"
$digibyted = Get-ChildItem "C:\Program Files\DigiByte" -Recurse -Filter "digibyted.exe" -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty FullName
if (-not $digibyted -and -not $SkipDigiByte) {
    try {
        Write-Host "  fetching latest DigiByte Core release..."
        $rel = Invoke-RestMethod "https://api.github.com/repos/DigiByte-Core/digibyte/releases/latest" -Headers @{ "User-Agent" = "digistamp-installer" }
        $asset = $rel.assets | Where-Object { $_.name -like "*win64-setup.exe" } | Select-Object -First 1
        if (-not $asset) { throw "no win64 setup asset found" }
        $inst = Join-Path $env:TEMP $asset.name
        Write-Host "  downloading $($asset.name) ..."
        Invoke-WebRequest $asset.browser_download_url -OutFile $inst -UseBasicParsing
        Write-Host "  installing silently (this can take a minute)..."
        Start-Process $inst -ArgumentList "/S" -Wait
        Start-Sleep -Seconds 5
        $digibyted = Get-ChildItem "C:\Program Files\DigiByte" -Recurse -Filter "digibyted.exe" -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty FullName
    } catch {
        Write-Host "  Could not auto-install DigiByte Core: $($_.Exception.Message)" -ForegroundColor Yellow
        Write-Host "  Install it from https://github.com/DigiByte-Core/digibyte/releases and re-run." -ForegroundColor Yellow
    }
}

# Ensure digibyte.conf exists with RPC credentials (read existing or generate).
New-Item -ItemType Directory -Force -Path $dgbData | Out-Null
$rpcUser = ""; $rpcPass = ""
if (Test-Path $dgbConf) {
    foreach ($line in Get-Content $dgbConf) {
        if ($line -match '^\s*rpcuser\s*=\s*(.+)$')     { $rpcUser = $Matches[1].Trim() }
        if ($line -match '^\s*rpcpassword\s*=\s*(.+)$') { $rpcPass = $Matches[1].Trim() }
    }
}
if (-not $rpcUser -or -not $rpcPass) {
    $rpcUser = "digiasset"
    $rpcPass = -join ((48..57) + (65..90) + (97..122) | Get-Random -Count 24 | ForEach-Object { [char]$_ })
    $addnodes = @("191.81.59.115","175.45.182.173","45.76.235.153","24.74.186.115","24.101.88.154","8.214.25.169","47.75.38.245")
    $conf = @("rpcuser=$rpcUser","rpcpassword=$rpcPass","rpcbind=127.0.0.1","rpcport=14022","rpcallowip=127.0.0.1","whitelist=127.0.0.1","listen=1","server=1","txindex=1","deprecatedrpc=addresses")
    $conf += ($addnodes | ForEach-Object { "addnode=$_" })
    Set-Content -Path $dgbConf -Value $conf -Encoding ASCII
    Write-Host "  wrote digibyte.conf with fresh RPC credentials."
} else {
    Write-Host "  digibyte.conf already has RPC credentials."
}

if ($digibyted) {
    Register-DaemonTask "DigiStampDigiByte" $digibyted "-datadir=`"$dgbData`""
    if (-not (Get-Process digibyted -ErrorAction SilentlyContinue)) {
        Start-Process $digibyted -ArgumentList "-datadir=`"$dgbData`"" -WindowStyle Hidden
    }
    Write-Host "  DigiByte daemon running + set to start on boot. (Blockchain syncs in the background - hours.)" -ForegroundColor Green
    Write-Host "  NOTE: don't also open the DigiByte-Qt wallet - this runs it headless for you."
} else {
    Write-Host "  DigiByte Core not present - install it, then re-run this script." -ForegroundColor Yellow
}

# --- 3. IPFS (kubo) --------------------------------------------------------
Step 3 "Installing IPFS (kubo)..."
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
} else { Write-Host "  ipfs.exe already present." }

[Environment]::SetEnvironmentVariable("IPFS_PATH", $ipfsRepo, "Machine")
$env:IPFS_PATH = $ipfsRepo
if (-not (Test-Path (Join-Path $ipfsRepo "config"))) { & $ipfsExe init | Out-Null; Write-Host "  IPFS repo initialised." }
try {
    $pubip = (Invoke-RestMethod "https://api.ipify.org" -TimeoutSec 10).Trim()
    if ($pubip) { & $ipfsExe config --json Addresses.Announce "[`"/ip4/$pubip/tcp/4001`"]" | Out-Null; Write-Host "  announce set to $pubip:4001" }
} catch { Write-Host "  (public IP not detected; the app's [F] key can set announce later)" }
Register-DaemonTask "DigiStampIPFS" $ipfsExe "daemon --enable-gc"
if (-not (Get-Process ipfs -ErrorAction SilentlyContinue)) { Start-Process -FilePath $ipfsExe -ArgumentList "daemon --enable-gc" -WindowStyle Hidden }
Write-Host "  IPFS running + set to start on boot." -ForegroundColor Green

# --- 4. Local Windows firewall --------------------------------------------
Step 4 "Opening local Windows firewall..."
function Open-Port($name, $proto, $port) {
    if (-not (Get-NetFirewallRule -DisplayName $name -ErrorAction SilentlyContinue)) {
        New-NetFirewallRule -DisplayName $name -Direction Inbound -Action Allow -Protocol $proto -LocalPort $port | Out-Null
        Write-Host "  + allowed inbound $proto $port"
    } else { Write-Host "  $proto $port already allowed" }
}
Open-Port "DigiStamp IPFS swarm (TCP 4001)" TCP 4001
Open-Port "DigiStamp IPFS swarm (UDP 4001)" UDP 4001
Open-Port "DigiByte P2P (TCP 12024)"        TCP 12024
Write-Host "  (5001 / 14022 / 8090 intentionally NOT opened - they stay local.)"

# --- 5. config.cfg for DigiAsset Core -------------------------------------
Step 5 "Writing config.cfg..."
$cfg = Join-Path $Root "config.cfg"
if (Test-Path $cfg) {
    Write-Host "  config.cfg already exists - leaving it untouched."
} else {
    $lines = @(
        "rpcbind=127.0.0.1","rpcport=14022","rpcuser=$rpcUser","rpcpassword=$rpcPass",
        "ipfspath=http://localhost:5001/api/v0/",
        "psp1server=$PoolServer","psp1subscribe=1","psp1payout=$PayoutAddress",
        "pruneage=5760","bootstrapchainstate=1"
    )
    Set-Content -Path $cfg -Value $lines -Encoding ASCII
    Write-Host "  + config.cfg (pool=$PoolServer, payout=$PayoutAddress)"
}

# --- 6. DigiAsset Core: run + restart on boot -----------------------------
Step 6 "Setting DigiAsset Core to start on boot..."
$coreExe = Join-Path $Root "DigiAssetCore.exe"
Register-VisibleTask "DigiStampNode" $coreExe $Root
if (-not (Get-Process DigiAssetCore -ErrorAction SilentlyContinue)) {
    Start-Process -FilePath $coreExe -WorkingDirectory $Root
}
Write-Host "  DigiAsset Core started + will open at every logon (task 'DigiStampNode')." -ForegroundColor Green

# --- 7. Reachability test -------------------------------------------------
Step 7 "Testing whether port 4001 is reachable from the internet..."
Start-Sleep -Seconds 2
$reachable = $null
try { $reachable = (Invoke-RestMethod "https://ifconfig.co/port/4001" -TimeoutSec 15).reachable } catch {}
if ($reachable -eq $true) {
    Write-Host "  SUCCESS: port 4001 is OPEN. You're set to be verified + paid." -ForegroundColor Green
} else {
    Write-Host "  Port 4001 is NOT reachable yet - forward it on your router (below), then re-test." -ForegroundColor Yellow
}

# --- Summary ---------------------------------------------------------------
Write-Host "`n===== Done =====" -ForegroundColor Green
Write-Host "Everything is installed and set to start on boot."
Write-Host ""
Write-Host "ON YOUR HOME ROUTER, forward to this PC:" -ForegroundColor Cyan
Write-Host "   TCP 4001   (REQUIRED - how the pool verifies + pays you)"
Write-Host "   UDP 4001   (recommended)"
Write-Host "   TCP 12024  (optional - more DigiByte peers)"
Write-Host "   Do NOT forward 5001 / 14022 / 8090."
Write-Host ""
Write-Host "WHAT HAPPENS NOW:" -ForegroundColor Cyan
Write-Host "  * DigiByte is syncing the blockchain in the background (hours the first time)."
Write-Host "  * Once synced, DigiAsset Core registers with the pool automatically."
Write-Host "  * In the DigiAsset Core window: press [N] to see the pool nodes (you should"
Write-Host "    appear), and [P] to re-test port 4001."
Write-Host "  * Check status any time with:  monitor-node.ps1"
Write-Host "  * Watch the pool + your earnings at $PoolServer"

<#
.SYNOPSIS
    One-command DigiStamp POOL deployment for Windows. Rebuild a whole pool host
    (node stack + pool server + HTTPS website + auto-start) in a single command.

.DESCRIPTION
    Run this in an Administrator PowerShell. It:
      1. Ensures the node stack is installed (DigiByte + IPFS + node) by running
         the standard node installer (setup-digiasset.ps1) if it isn't already.
      2. Downloads DigiAssetPoolServer.exe into C:\DigiAssetWindows.
      3. Writes pool.cfg (reusing the DigiByte RPC creds), payouts OFF by default.
      4. Stands up the Caddy HTTPS website for your domain.
      5. Installs start/backup helpers and an auto-start-at-logon task.
      6. Starts the stack.

    Node data lives in C:\DigiAssetWindows; DigiByte lives in C:\DigiByte.

.EXAMPLE
    # interactive (prompts for payout, treasury, domain)
    iwr https://raw.githubusercontent.com/chopperbriano/DigiAssetWindows/master/setup-pool.ps1 -OutFile "$env:TEMP\setup-pool.ps1" -UseBasicParsing; powershell -ExecutionPolicy Bypass -File "$env:TEMP\setup-pool.ps1"

    # unattended
    powershell -ExecutionPolicy Bypass -File .\setup-pool.ps1 -PayoutAddress dgb1... -TreasuryAddress dgb1... -Domain pool.example.com
#>
[CmdletBinding()]
param(
    [string]$PayoutAddress   = '',   # your node's payout address (passed to the node installer)
    [string]$TreasuryAddress = '',   # pool's public donation/treasury address (shown on the site)
    [string]$Domain          = '',   # pool domain with an A record -> this box (blank = skip website)
    [switch]$SkipNodeInstall,        # skip step 1 if the node stack is already installed
    [string]$Root            = 'C:\DigiAssetWindows',
    [string]$DigiByteDir     = 'C:\DigiByte'
)
$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$SCRIPT_VERSION = '1.0.0'
$Repo = 'https://raw.githubusercontent.com/chopperbriano/DigiAssetWindows/master'
$Rel  = 'https://github.com/chopperbriano/DigiAssetWindows/releases/latest/download'

function Say($m,$c='Gray'){ Write-Host $m -ForegroundColor $c }
function Step($n,$m){ Write-Host ''; Write-Host "[$n] $m" -ForegroundColor Cyan }
function Get-File($url,$dest){ Invoke-WebRequest -Uri $url -OutFile $dest -UseBasicParsing }
function Read-Conf($path){
    $h=@{}; if(Test-Path $path){ foreach($l in Get-Content $path){ $t=$l.Trim()
        if($t -eq '' -or $t.StartsWith('#')){continue}; $i=$t.IndexOf('=')
        if($i -gt 0){ $h[$t.Substring(0,$i).Trim()]=$t.Substring($i+1).Trim() } } }
    return $h
}

# --- Elevate (all steps need admin) ---------------------------------------
$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) {
    if ($PSCommandPath) {
        Say 'Pool setup needs Administrator - approve the UAC prompt that appears...' 'Yellow'
        $fwd = "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`""
        if ($PayoutAddress)   { $fwd += " -PayoutAddress `"$PayoutAddress`"" }
        if ($TreasuryAddress) { $fwd += " -TreasuryAddress `"$TreasuryAddress`"" }
        if ($Domain)          { $fwd += " -Domain `"$Domain`"" }
        if ($SkipNodeInstall) { $fwd += ' -SkipNodeInstall' }
        Start-Process powershell.exe -Verb RunAs -ArgumentList $fwd; return
    } else { throw 'Run this in an elevated (Administrator) PowerShell.' }
}

Say '==============================================================' 'Cyan'
Say " DigiStamp POOL setup for Windows   (v$SCRIPT_VERSION)" 'Cyan'
Say '==============================================================' 'Cyan'
Say " Node data : $Root" 'Gray'
Say " DigiByte  : $DigiByteDir" 'Gray'
$tmp = $env:TEMP
if (-not (Test-Path $Root)) { New-Item -ItemType Directory -Force -Path $Root | Out-Null }

# --- 1. Node stack ---------------------------------------------------------
Step 1 'Node stack (DigiByte + IPFS + node)'
$nodeExe = Join-Path $Root 'DigiAssetWindows.exe'
if ($SkipNodeInstall -or (Test-Path $nodeExe)) {
    Say '  node stack already present - skipping node install.' 'Green'
} else {
    Say '  installing the node stack via the standard node installer...' 'White'
    $ni = Join-Path $tmp 'setup-digiasset.ps1'
    Get-File "$Repo/setup-digiasset.ps1" $ni
    $niArg = "-NoProfile -ExecutionPolicy Bypass -File `"$ni`""
    if ($PayoutAddress) { $niArg += " -PayoutAddress `"$PayoutAddress`"" }
    Start-Process powershell.exe -ArgumentList $niArg -Wait
    if (-not (Test-Path $nodeExe)) { throw "Node install did not produce $nodeExe. Fix the node install, then re-run with -SkipNodeInstall." }
}

# --- 2. Pool server exe ----------------------------------------------------
Step 2 'DigiAssetPoolServer.exe'
Get-File "$Rel/DigiAssetPoolServer.exe" (Join-Path $Root 'DigiAssetPoolServer.exe')
Say '  + DigiAssetPoolServer.exe (latest release)' 'Green'

# --- 3. pool.cfg -----------------------------------------------------------
Step 3 'pool.cfg'
$poolCfg = Join-Path $Root 'pool.cfg'
if (Test-Path $poolCfg) {
    Say '  pool.cfg already exists - leaving it untouched.' 'Yellow'
} else {
    $c = Read-Conf (Join-Path $DigiByteDir 'digibyte.conf')
    $ru = $c['rpcuser']; $rp = $c['rpcpassword']
    $rport = if ($c['rpcport']) { $c['rpcport'] } else { '14022' }
    if (-not $ru -or -not $rp) { Say '  WARNING: could not read RPC creds from digibyte.conf - edit rpcuser/rpcpassword in pool.cfg by hand.' 'Yellow' }
    if (-not $TreasuryAddress) {
        Say '  The treasury (donation) address is shown on your pool page; its balance is read' 'Gray'
        Say '  from a public explorer, so it can live in ANY wallet.' 'Gray'
        $TreasuryAddress = Read-Host '  Treasury (donation) DGB address'
    }
    $lines = @(
        '# DigiAssetPoolServer config (written by setup-pool.ps1)',
        'poolport=14028',
        'pooldbpath=pool.db',
        'ipfspath=http://localhost:5001/api/v0/',
        '',
        '# DigiByte Core RPC (copied from digibyte.conf)',
        "rpcuser=$ru",
        "rpcpassword=$rp",
        "rpcport=$rport",
        '',
        "pooldonationaddress=$TreasuryAddress",
        'pooladdrapiprefix=https://digiexplorer.info/api/address/',
        'poolexplorertxprefix=https://digiexplorer.info/tx/',
        '',
        '# Payouts OFF until you fund a wallet + smoke-test (POOL-SETUP.md step 6).',
        'poolpayouts=0',
        'poolpayoutpercent=10',
        'poolpayoutperiodhours=24'
    )
    Set-Content -Path $poolCfg -Value $lines -Encoding ASCII
    Say "  + pool.cfg (treasury=$TreasuryAddress, payouts OFF)" 'Green'
}

# --- 4. Caddy HTTPS website ------------------------------------------------
Step 4 'HTTPS website (Caddy)'
if (-not $Domain) {
    Say '  Nodes connect over HTTPS. Enter your pool domain (an A record must already' 'Gray'
    Say '  point it at this box). Leave blank to skip and set it up later.' 'Gray'
    $Domain = Read-Host '  Pool domain (e.g. pool.example.com)'
}
if ($Domain) {
    $deploy = Join-Path $Root 'deploy'
    New-Item -ItemType Directory -Force -Path (Join-Path $deploy 'site') | Out-Null
    foreach ($rel in 'setup-caddy.ps1','Caddyfile','site/index.html','site/favicon.svg') {
        try { Get-File "$Repo/pool/deploy/$rel" (Join-Path $deploy ($rel -replace '/','\')) } catch { Say "  (could not fetch $rel)" 'Yellow' }
    }
    Start-Process powershell.exe -ArgumentList ("-NoProfile -ExecutionPolicy Bypass -File `"$($deploy)\setup-caddy.ps1`" -Domain `"$Domain`" -PoolPort 14028") -Wait
    Say "  Caddy configured for https://$Domain" 'Green'
} else {
    Say '  skipped - run pool\deploy\setup-caddy.ps1 later. Nodes need HTTPS to join.' 'Yellow'
}

# --- 5. Operational helpers + auto-start -----------------------------------
Step 5 'Start/backup helpers + auto-start at logon'
foreach ($f in 'start-digistamp.ps1','backup-digistamp.ps1') {
    try { Get-File "$Repo/pool/deploy/$f" (Join-Path $Root $f); Say "  + $f" 'Green' } catch { Say "  (could not fetch $f)" 'Yellow' }
}
$startPs = Join-Path $Root 'start-digistamp.ps1'
try {
    $a = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument "-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File `"$startPs`" -Root `"$Root`""
    $t = New-ScheduledTaskTrigger -AtLogOn
    $u = [Security.Principal.WindowsIdentity]::GetCurrent().Name
    $p = New-ScheduledTaskPrincipal -UserId $u -LogonType Interactive -RunLevel Highest
    $s = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit ([TimeSpan]::Zero)
    Register-ScheduledTask -TaskName 'DigiStampPool' -Action $a -Trigger $t -Principal $p -Settings $s -Force | Out-Null
    Say '  auto-start task "DigiStampPool" registered (runs start-digistamp at logon).' 'Green'
} catch { Say "  could not register auto-start task: $($_.Exception.Message)" 'Yellow' }

# --- 6. Start now ----------------------------------------------------------
Step 6 'Starting the pool stack'
if (Test-Path $startPs) {
    try { & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $startPs -Root $Root } catch { Say "  start-digistamp reported: $($_.Exception.Message)" 'Yellow' }
}

Write-Host ''
Say '===================== POOL SETUP DONE =====================' 'Green'
Say " Pool config : $poolCfg" 'White'
if ($Domain) { Say " Website     : https://$Domain/" 'White' }
Say ' Payouts are OFF. To go live: fund the pool wallet, set poolpayouts=1 in' 'White'
Say ' pool.cfg, restart the pool, then use [P] preview / [E] execute in its dashboard.' 'White'
Say ' Router: forward 80+443 (website), 4001 TCP/UDP (hosting), 12024 TCP (DigiByte).' 'White'
Say ' Full guide: POOL-SETUP.md' 'Gray'
Write-Host ''
if ([Environment]::UserInteractive) { try { Read-Host 'Press Enter to close' | Out-Null } catch {} }

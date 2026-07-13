<#
.SYNOPSIS
    Start the DigiStamp stack: DigiAsset for Windows (node), DigiAssetPoolServer (pool),
    and the Caddy website. Waits for DigiByte Core to be ready first.

.DESCRIPTION
    You start the DigiByte Core Windows client (wallet) yourself. This script
    then:
      1. waits for DigiByte Core's RPC to respond,
      2. checks the IPFS API is up (warns if not),
      3. launches DigiAssetWindows.exe and DigiAssetPoolServer.exe (each in its own
         window, from the data folder so they read config.cfg / pool.cfg), and
      4. HEALTH-CHECKS the Caddy website (caddy.exe running AND 443 listening) and
         restarts it if it is actually down - not just "is the task Running".

    Self-elevates (restarting Caddy + killing a stray caddy.exe needs admin).

.PARAMETER WebsiteOnly          Skip Core/IPFS/node/pool; only heal + restart Caddy.
.PARAMETER ForceRestartWebsite  Restart Caddy even if it currently looks healthy.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\start-digistamp.ps1
.EXAMPLE
    # Site is timing out - just fix the website, fast:
    powershell -ExecutionPolicy Bypass -File .\start-digistamp.ps1 -WebsiteOnly
.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\start-digistamp.ps1 -ForceRestartWebsite
#>
[CmdletBinding()]
param(
    # Prefer the current layout (C:\DigiAssetWindows); fall back to the old folder if
    # that is where this box's data actually lives, so an existing pool keeps working.
    [string]$Root = $(if (Test-Path 'C:\DigiAssetWindows\config.cfg') { 'C:\DigiAssetWindows' } elseif (Test-Path 'C:\DigiAsset\config.cfg') { 'C:\DigiAsset' } else { 'C:\DigiAssetWindows' }),
    [int]   $WaitForCoreSeconds = 300,
    # -WebsiteOnly: skip Core/IPFS/node/pool and only health-check + restart Caddy.
    [switch]$WebsiteOnly,
    # -ForceRestartWebsite: restart Caddy even if it currently looks healthy.
    [switch]$ForceRestartWebsite
)
$ErrorActionPreference = "Stop"
$ScriptVersion = '1.3.0'

# Restarting the Caddy task + killing a stray caddy.exe needs admin. Elevate,
# preserving args, so the website heal below actually works.
$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) {
    if ($PSCommandPath) {
        $a = "-ExecutionPolicy Bypass -File `"$PSCommandPath`" -Root `"$Root`" -WaitForCoreSeconds $WaitForCoreSeconds"
        if ($WebsiteOnly)         { $a += ' -WebsiteOnly' }
        if ($ForceRestartWebsite) { $a += ' -ForceRestartWebsite' }
        Start-Process powershell.exe -Verb RunAs -ArgumentList $a
        return
    }
    Write-Warning "Not elevated - Caddy restart may fail. Re-run in an Administrator PowerShell."
}

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

function Invoke-DgbRpc([string]$user, [string]$pass, [int]$port, [string]$method, [string]$paramsJson = "[]") {
    $body = '{"jsonrpc":"1.0","id":"digistamp","method":"' + $method + '","params":' + $paramsJson + '}'
    $b64 = [Convert]::ToBase64String([Text.Encoding]::ASCII.GetBytes("${user}:${pass}"))
    return Invoke-RestMethod -Uri "http://127.0.0.1:$port" -Method Post -Body $body `
        -Headers @{ Authorization = "Basic $b64" } -ContentType "text/plain" -TimeoutSec 6
}

Write-Host "=== Start DigiStamp stack  (v$ScriptVersion) ===" -ForegroundColor Cyan
Write-Host "Data folder: $Root" -ForegroundColor Gray

if (-not (Test-Path $Root)) { throw "Data folder not found: $Root" }

if (-not $WebsiteOnly) {
# --- 1. Wait for DigiByte Core (you start it manually) --------------------
$cfg  = Read-Cfg (Join-Path $Root "config.cfg")
$user = $cfg["rpcuser"]
$pass = $cfg["rpcpassword"]
$port = 14022
if ($cfg["rpcport"]) { try { $port = [int]$cfg["rpcport"] } catch {} }

if (-not $user -or -not $pass) {
    Write-Warning "rpcuser/rpcpassword not found in config.cfg; cannot verify DigiByte Core. Make sure the wallet is running with server=1."
} else {
    Write-Host "Waiting for DigiByte Core RPC on 127.0.0.1:$port (up to $WaitForCoreSeconds s)..." -ForegroundColor White
    $deadline = (Get-Date).AddSeconds($WaitForCoreSeconds)
    $ready = $false
    while ((Get-Date) -lt $deadline) {
        try { $null = Invoke-DgbRpc $user $pass $port "getblockchaininfo"; $ready = $true; break }
        catch { Start-Sleep -Seconds 3 }
    }
    if (-not $ready) {
        Write-Warning "DigiByte Core did not respond. Start the DigiByte Core Windows client (with server=1) and re-run this script."
        return
    }
    Write-Host "DigiByte Core is ready." -ForegroundColor Green
}

# --- 2. Check IPFS ---------------------------------------------------------
try {
    $null = Invoke-RestMethod -Uri "http://127.0.0.1:5001/api/v0/id" -Method Post -TimeoutSec 5
    Write-Host "IPFS API is up." -ForegroundColor Green
} catch {
    Write-Warning "IPFS API (port 5001) not responding. Start IPFS Desktop, or hosting/verification won't work."
}

# --- 3. Launch the node + pool exes ---------------------------------------
function Start-Exe([string]$name) {
    $exe = Join-Path $Root $name
    if (-not (Test-Path $exe)) { Write-Warning "$name not found in $Root - skipping."; return }
    $proc = [IO.Path]::GetFileNameWithoutExtension($name)
    if (Get-Process -Name $proc -ErrorAction SilentlyContinue) {
        Write-Host "$name already running." -ForegroundColor Gray
        return
    }
    # -WindowStyle Normal forces a VISIBLE console. Without it the dashboards
    # inherit the hidden show-state if this script is run from a hidden task.
    Start-Process -FilePath $exe -WorkingDirectory $Root -WindowStyle Normal
    Write-Host "Started $name" -ForegroundColor Green
}
Start-Exe "DigiAssetWindows.exe"
Start-Exe "DigiAssetPoolServer.exe"
}  # end: if (-not $WebsiteOnly)

# --- 4. Caddy website (health-check + heal) --------------------------------
# The website is served by Caddy (TLS on 443, reverse-proxy to the pool exe). A
# scheduled task can show "Running" while caddy.exe has actually died or isn't
# listening, so check the PROCESS + port 443 - not just the task state - and
# restart it when it is really down.
function Test-Listening([int]$p) {
    try { return [bool](Get-NetTCPConnection -LocalPort $p -State Listen -ErrorAction SilentlyContinue) } catch { return $false }
}

$caddyTask = Get-ScheduledTask -TaskName "DigiStampCaddy" -ErrorAction SilentlyContinue
if (-not $caddyTask) {
    Write-Warning "Caddy task 'DigiStampCaddy' not found. Run setup-caddy.ps1 once to install the website."
} else {
    $caddyUp = [bool](Get-Process -Name caddy -ErrorAction SilentlyContinue)
    $port443 = Test-Listening 443
    $healthy = ($caddyUp -and $port443)

    if ($healthy -and -not $ForceRestartWebsite) {
        Write-Host "Caddy website healthy (caddy running, 443 listening)." -ForegroundColor Green
    } else {
        if ($ForceRestartWebsite) {
            Write-Host "Restarting Caddy website (forced)..." -ForegroundColor Cyan
        } else {
            Write-Warning "Caddy website is DOWN (caddy running: $caddyUp, 443 listening: $port443) - restarting it."
        }
        # Stop the task + kill any stray caddy so we relaunch clean.
        try { Stop-ScheduledTask -TaskName "DigiStampCaddy" -ErrorAction SilentlyContinue } catch {}
        Get-Process -Name caddy -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
        Start-Sleep -Seconds 2
        Start-ScheduledTask -TaskName "DigiStampCaddy"
        $ok = $false
        for ($i = 0; $i -lt 20; $i++) { Start-Sleep -Seconds 1; if (Test-Listening 443) { $ok = $true; break } }
        if ($ok) {
            Write-Host "Caddy restarted - 443 is now listening." -ForegroundColor Green
        } else {
            Write-Warning "Caddy started but 443 is still NOT listening. Check the Caddy config/logs (a bad Caddyfile or the cert step failing); re-run setup-caddy.ps1 if needed."
        }
    }

    # If 443 IS listening locally but the site still times out from the internet,
    # the block is the router/firewall - not Caddy. Point that out.
    if (Test-Listening 443) {
        Write-Host "Note: 443 is listening locally. If the site still times out from OUTSIDE," -ForegroundColor DarkGray
        Write-Host "      the block is your router forward (TCP 80+443) or the Windows firewall - not Caddy." -ForegroundColor DarkGray
    }
}

Write-Host ""
Write-Host "DigiStamp stack started." -ForegroundColor Cyan
Write-Host "  Node + pool dashboards opened in their own windows." -ForegroundColor Gray
Write-Host "  Website: https://pool.digistamp.co/" -ForegroundColor Gray

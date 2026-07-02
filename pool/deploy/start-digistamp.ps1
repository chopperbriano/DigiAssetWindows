<#
.SYNOPSIS
    Start the DigiStamp stack: DigiAsset Core (node), DigiAssetPoolServer (pool),
    and the Caddy website. Waits for DigiByte Core to be ready first.

.DESCRIPTION
    You start the DigiByte Core Windows client (wallet) yourself. This script
    then:
      1. waits for DigiByte Core's RPC to respond,
      2. checks the IPFS API is up (warns if not),
      3. launches DigiAssetCore.exe and DigiAssetPoolServer.exe (each in its own
         window, from the data folder so they read config.cfg / pool.cfg), and
      4. makes sure the Caddy website task is running.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\start-digistamp.ps1
    powershell -ExecutionPolicy Bypass -File .\start-digistamp.ps1 -Root C:\DigiAssetWindows
#>
[CmdletBinding()]
param(
    [string]$Root = "C:\DigiAssetWindows",
    [int]   $WaitForCoreSeconds = 300
)
$ErrorActionPreference = "Stop"

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

Write-Host "=== Start DigiStamp stack ===" -ForegroundColor Cyan
Write-Host "Data folder: $Root"

if (-not (Test-Path $Root)) { throw "Data folder not found: $Root" }

# --- 1. Wait for DigiByte Core (you start it manually) --------------------
$cfg  = Read-Cfg (Join-Path $Root "config.cfg")
$user = $cfg["rpcuser"]
$pass = $cfg["rpcpassword"]
$port = 14022
if ($cfg["rpcport"]) { try { $port = [int]$cfg["rpcport"] } catch {} }

if (-not $user -or -not $pass) {
    Write-Warning "rpcuser/rpcpassword not found in config.cfg; cannot verify DigiByte Core. Make sure the wallet is running with server=1."
} else {
    Write-Host "Waiting for DigiByte Core RPC on 127.0.0.1:$port (up to $WaitForCoreSeconds s)..."
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
        Write-Host "$name already running."
        return
    }
    Start-Process -FilePath $exe -WorkingDirectory $Root
    Write-Host "Started $name" -ForegroundColor Green
}
Start-Exe "DigiAssetCore.exe"
Start-Exe "DigiAssetPoolServer.exe"

# --- 4. Caddy website ------------------------------------------------------
$task = Get-ScheduledTask -TaskName "DigiStampCaddy" -ErrorAction SilentlyContinue
if ($task) {
    if ($task.State -ne "Running") {
        Start-ScheduledTask -TaskName "DigiStampCaddy"
        Write-Host "Started Caddy website (DigiStampCaddy task)." -ForegroundColor Green
    } else {
        Write-Host "Caddy website already running."
    }
} else {
    Write-Warning "Caddy task 'DigiStampCaddy' not found. Run setup-caddy.ps1 once to install the website."
}

Write-Host ""
Write-Host "DigiStamp stack started." -ForegroundColor Cyan
Write-Host "  Node + pool dashboards opened in their own windows."
Write-Host "  Website: https://pool.digistamp.co/"

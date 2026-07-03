<#
.SYNOPSIS
    Stop the DigiStamp node stack (DigiAsset for Windows, IPFS, DigiByte), and optionally
    disable auto-start or fully uninstall.

.MODES
    (default)          Stop everything running now (graceful where possible).
                       The boot tasks stay, so it all restarts on next boot.
    -DisableAutostart  Also remove the boot tasks so nothing restarts on boot.
    -Uninstall         Stop, remove boot tasks + firewall rules, and delete the
                       DigiAsset/IPFS files in -Root. Leaves DigiByte Core and its
                       blockchain data alone (removing those is a separate choice).

.USAGE
    powershell -ExecutionPolicy Bypass -File .\stop-node.ps1
    powershell -ExecutionPolicy Bypass -File .\stop-node.ps1 -DisableAutostart
    powershell -ExecutionPolicy Bypass -File .\stop-node.ps1 -Uninstall
#>
[CmdletBinding()]
param(
    [string]$Root = "C:\DigiAssetWindows",
    [switch]$DisableAutostart,
    [switch]$Uninstall
)
$ErrorActionPreference = "Continue"

# Elevation (needed to stop SYSTEM tasks / remove firewall rules).
$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) {
    if ($PSCommandPath) { Start-Process powershell.exe -Verb RunAs -ArgumentList "-ExecutionPolicy Bypass -File `"$PSCommandPath`" $(if($DisableAutostart){'-DisableAutostart'}) $(if($Uninstall){'-Uninstall'}) -Root `"$Root`""; return }
    else { throw "Run this in an elevated (Administrator) PowerShell." }
}

function Read-Cfg([string]$path) {
    $h = @{}
    if (Test-Path $path) { foreach ($l in Get-Content $path) { $t=$l.Trim(); if ($t -and -not $t.StartsWith("#")) { $i=$t.IndexOf("="); if ($i -gt 0){ $h[$t.Substring(0,$i).Trim()]=$t.Substring($i+1).Trim() } } } }
    return $h
}

$tasks = "DigiStampNode", "DigiStampIPFS", "DigiStampDigiByte"
$rules = "DigiStamp IPFS swarm (TCP 4001)", "DigiStamp IPFS swarm (UDP 4001)", "DigiByte P2P (TCP 12024)"

Write-Host "=== Stopping DigiStamp node stack ===" -ForegroundColor Cyan

# 1. Graceful stops -----------------------------------------------------
# DigiByte: ask it to stop cleanly via RPC (kill risks chainstate corruption).
$cfg = Read-Cfg (Join-Path $Root "config.cfg")
if ($cfg["rpcuser"] -and $cfg["rpcpassword"]) {
    $port = 14022; if ($cfg["rpcport"]) { try { $port=[int]$cfg["rpcport"] } catch {} }
    try {
        $b64=[Convert]::ToBase64String([Text.Encoding]::ASCII.GetBytes("$($cfg['rpcuser']):$($cfg['rpcpassword'])"))
        Invoke-RestMethod -Uri "http://127.0.0.1:$port" -Method Post -ContentType "text/plain" -Headers @{Authorization="Basic $b64"} -TimeoutSec 6 -Body '{"jsonrpc":"1.0","id":"stop","method":"stop","params":[]}' | Out-Null
        Write-Host "  DigiByte Core: clean shutdown requested."
    } catch { Write-Host "  DigiByte Core: RPC stop failed (may already be down)." }
}
# IPFS: graceful shutdown via API.
try { Invoke-RestMethod -Uri "http://127.0.0.1:5001/api/v0/shutdown" -Method Post -TimeoutSec 6 | Out-Null; Write-Host "  IPFS: shutdown requested." } catch {}
Start-Sleep -Seconds 3

# 2. Stop the scheduled tasks + any stragglers --------------------------
foreach ($t in $tasks) {
    if (Get-ScheduledTask -TaskName $t -ErrorAction SilentlyContinue) { Stop-ScheduledTask -TaskName $t -ErrorAction SilentlyContinue }
}
foreach ($p in "DigiAssetWindows", "ipfs", "digibyted") {
    Get-Process $p -ErrorAction SilentlyContinue | ForEach-Object { $_ | Stop-Process -Force -ErrorAction SilentlyContinue; Write-Host "  stopped $p" }
}

if (-not $DisableAutostart -and -not $Uninstall) {
    Write-Host "`nStopped. Boot tasks are still in place - it will start again on next boot." -ForegroundColor Green
    Write-Host "To also stop it restarting on boot, run with -DisableAutostart."
    return
}

# 3. Remove boot tasks --------------------------------------------------
foreach ($t in $tasks) {
    if (Get-ScheduledTask -TaskName $t -ErrorAction SilentlyContinue) { Unregister-ScheduledTask -TaskName $t -Confirm:$false; Write-Host "  removed boot task $t" }
}

if (-not $Uninstall) {
    Write-Host "`nStopped and auto-start disabled. Files + config are left in place." -ForegroundColor Green
    return
}

# 4. Full uninstall -----------------------------------------------------
Write-Host "`n-Uninstall will DELETE $Root (DigiAsset for Windows, IPFS, config) and remove the" -ForegroundColor Yellow
Write-Host "firewall rules. DigiByte Core and its blockchain data are left installed." -ForegroundColor Yellow
$ans = Read-Host "Type DELETE to confirm"
if ($ans -ne "DELETE") { Write-Host "Cancelled - nothing removed."; return }

foreach ($r in $rules) {
    if (Get-NetFirewallRule -DisplayName $r -ErrorAction SilentlyContinue) { Remove-NetFirewallRule -DisplayName $r; Write-Host "  removed firewall rule: $r" }
}
[Environment]::SetEnvironmentVariable("IPFS_PATH", $null, "Machine")
if (Test-Path $Root) { Remove-Item -LiteralPath $Root -Recurse -Force; Write-Host "  removed $Root" }
Write-Host "`nUninstalled." -ForegroundColor Green
Write-Host "DigiByte Core is still installed (uninstall it from Windows 'Apps' if you want),"
Write-Host "and its blockchain data remains in %APPDATA%\DigiByte."

<#
.SYNOPSIS
    Bootstrap a Caddy reverse proxy + landing page in front of DigiAssetPoolServer.exe.

.DESCRIPTION
    Downloads Caddy, generates a resolved Caddyfile from the template in this
    folder, copies the static landing page, opens the firewall for HTTP/HTTPS,
    and registers a scheduled task so Caddy starts on boot. Caddy then:
      - serves the landing page at https://<domain>/
      - reverse-proxies the pool API routes to the local pool exe
      - obtains and renews a TLS certificate automatically

.PREREQUISITES
    - Run in an ELEVATED PowerShell (Administrator).
    - A DNS A record for <Domain> must already point at this server's public IP.
    - Inbound TCP 80 and 443 must be reachable from the internet.
    - DigiAssetPoolServer.exe should be running locally on <PoolPort>.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\setup-caddy.ps1
    powershell -ExecutionPolicy Bypass -File .\setup-caddy.ps1 -Domain pool.example.com -PoolPort 14028
#>
[CmdletBinding()]
param(
    [string]$Domain    = "pool.digistamp.co",
    [int]   $PoolPort  = 14028,
    [string]$InstallDir = "C:\DigiStampPool",
    [string]$PoolCfg    = "C:\DigiAssetWindows\pool.cfg"
)

$ErrorActionPreference = "Stop"
$ScriptVersion = '1.0.0'
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path

function Assert-Admin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($id)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw "This script must be run from an elevated (Administrator) PowerShell."
    }
}

Write-Host "=== DigiAssetPoolServer :: Caddy reverse-proxy setup  (v$ScriptVersion) ===" -ForegroundColor Cyan
Assert-Admin

# --- 1. Layout ------------------------------------------------------------
$caddyExe    = Join-Path $InstallDir "caddy.exe"
$siteDir     = Join-Path $InstallDir "site"
$caddyfile   = Join-Path $InstallDir "Caddyfile"
$caddyData   = Join-Path $InstallDir "caddydata"   # FIXED cert store shared by manual + SYSTEM runs
New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
New-Item -ItemType Directory -Force -Path $siteDir    | Out-Null
New-Item -ItemType Directory -Force -Path $caddyData  | Out-Null
Write-Host "Install dir: $InstallDir" -ForegroundColor Gray

# --- 2. Download Caddy (official build endpoint) --------------------------
if (Test-Path $caddyExe) {
    Write-Host "Caddy already present, skipping download." -ForegroundColor Gray
} else {
    Write-Host "Downloading Caddy for windows/amd64 ..." -ForegroundColor White
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    Invoke-WebRequest -Uri "https://caddyserver.com/api/download?os=windows&arch=amd64" `
                      -OutFile $caddyExe
    Write-Host ("Caddy downloaded ({0:N0} bytes)." -f (Get-Item $caddyExe).Length) -ForegroundColor Green
}

# --- 3. Copy the landing page (skip if this folder has no site/ - e.g. when
#        re-running from a partial download; the installed site/ is kept) ------
$srcSite = Join-Path $scriptDir "site"
if (Test-Path $srcSite) {
    Copy-Item -Path (Join-Path $srcSite "*") -Destination $siteDir -Recurse -Force
    Write-Host "Landing page copied to $siteDir" -ForegroundColor Green
} else {
    Write-Host "No site\ next to this script - keeping the existing landing page in $siteDir." -ForegroundColor Yellow
}

# --- 4. Generate a resolved Caddyfile from the template -------------------
$template = Get-Content (Join-Path $scriptDir "Caddyfile") -Raw
$resolved = $template.
    Replace('{$DOMAIN}',   $Domain).
    Replace('{$POOLPORT}', "$PoolPort").
    Replace('{$SITE_ROOT}', ($siteDir -replace '\\','/')).
    Replace('{$CADDY_DATA}', ($caddyData -replace '\\','/'))
Set-Content -Path $caddyfile -Value $resolved -Encoding UTF8
Write-Host "Caddyfile written: $caddyfile" -ForegroundColor Green

# Validate before we commit to running it.
& $caddyExe validate --config $caddyfile --adapter caddyfile
if ($LASTEXITCODE -ne 0) { throw "Caddyfile validation failed." }
Write-Host "Caddyfile validated OK." -ForegroundColor Green

# --- 5. Firewall: allow HTTP (ACME) + HTTPS -------------------------------
foreach ($p in 80,443) {
    $rule = "Caddy inbound TCP $p"
    netsh advfirewall firewall show rule name="$rule" | Out-Null
    if ($LASTEXITCODE -ne 0) {
        netsh advfirewall firewall add rule name="$rule" dir=in action=allow protocol=TCP localport=$p | Out-Null
        Write-Host "Firewall rule added: $rule" -ForegroundColor Green
    }
}

# --- 6. Register a scheduled task so Caddy starts on boot -----------------
$taskName = "DigiStampCaddy"
$action   = New-ScheduledTaskAction -Execute $caddyExe `
             -Argument "run --config `"$caddyfile`"" -WorkingDirectory $InstallDir
# Trigger at boot AND at any logon - if the network isn't up yet at boot (so the
# ACME/cert step would fail), the logon trigger gives it a second, later start.
$trigStart = New-ScheduledTaskTrigger -AtStartup
$trigLogon = New-ScheduledTaskTrigger -AtLogOn
$principal = New-ScheduledTaskPrincipal -UserId "SYSTEM" -LogonType ServiceAccount -RunLevel Highest
# ExecutionTimeLimit 0 = NO limit (the default is 3 days, which would silently
# kill this long-running web server). RestartCount high so a transient failure
# (e.g. network not ready at boot) keeps retrying instead of giving up.
$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
    -RestartCount 30 -RestartInterval (New-TimeSpan -Minutes 1) -ExecutionTimeLimit ([TimeSpan]::Zero)
Register-ScheduledTask -TaskName $taskName -Action $action -Trigger $trigStart, $trigLogon `
    -Principal $principal -Settings $settings -Force | Out-Null
Write-Host "Scheduled task '$taskName' registered (starts Caddy at boot)." -ForegroundColor Green

# --- 6b. Record this pool's own public URL so it can announce itself to the
#         discovery network (poolpublicurl). Set only if not already present.
if (Test-Path $PoolCfg) {
    $pc = @(Get-Content -Path $PoolCfg)
    if (-not ($pc | Where-Object { $_ -match '^\s*poolpublicurl\s*=' })) {
        Add-Content -Path $PoolCfg -Value "poolpublicurl=https://$Domain" -Encoding ASCII
        Write-Host "  wrote poolpublicurl=https://$Domain to $PoolCfg (so this pool is listed in the discovery directory)." -ForegroundColor Green
    }
}

# --- 7. Start it now ------------------------------------------------------
Start-ScheduledTask -TaskName $taskName
Write-Host ""
Write-Host "Done. Caddy is starting." -ForegroundColor Green
Write-Host "  Landing page : https://$Domain/" -ForegroundColor Gray
Write-Host "  Pool API     : proxied to 127.0.0.1:$PoolPort" -ForegroundColor Gray
Write-Host ""
Write-Host "Reminders:" -ForegroundColor Yellow
Write-Host "  * DNS: $Domain must resolve to THIS server's public IP." -ForegroundColor White
Write-Host "  * Ports 80 and 443 must be reachable from the internet for TLS to issue." -ForegroundColor White
Write-Host "  * Make sure DigiAssetPoolServer.exe is running on port $PoolPort." -ForegroundColor White
Write-Host "  * First HTTPS request may take ~10-30s while the certificate is obtained." -ForegroundColor White

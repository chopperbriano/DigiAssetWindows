<#
.SYNOPSIS
    Find out WHY the Caddy website (https://<domain>/) is down on the pool box.
    Runs Caddy in the foreground briefly to capture the real startup error that
    the scheduled task hides, and checks the usual culprits.

.DESCRIPTION
    Run this ON THE POOL SERVER. It reports, in order:
      1. caddy.exe + Caddyfile present, and `caddy validate` result.
      2. The DigiStampCaddy task's last run result.
      3. Anything already bound to 80/443 (a port conflict stops Caddy binding).
      4. Inbound firewall allow-rules for 80/443.
      5. This box's public IP vs the domain's DNS A-record (a mismatch breaks the
         Let's Encrypt challenge, so Caddy can't get a cert).
      6. A short foreground `caddy run` with output captured - if Caddy exits, the
         captured log shows exactly why (cert/ACME failure, bad config, bind error).

    It does not leave a stray caddy running; once you have fixed the cause, bring
    the site up the normal way:  start-digistamp.ps1 -WebsiteOnly

.PARAMETER InstallDir  Caddy folder (default C:\DigiStampPool).
.PARAMETER Domain      The site domain (default pool.digistamp.co).
.PARAMETER Seconds     How long to run Caddy while capturing output (default 10).

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\diagnose-website.ps1
#>
[CmdletBinding()]
param(
    [string]$InstallDir = 'C:\DigiStampPool',
    [string]$Domain     = 'pool.digistamp.co',
    [int]   $Seconds    = 10
)
$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

function Say($m, $c = 'Gray') { Write-Host $m -ForegroundColor $c }
function Head($m) { Write-Host "`n--- $m ---" -ForegroundColor Cyan }

# Needs admin to stop the task + bind 80/443.
$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) {
    if ($PSCommandPath) { Start-Process powershell.exe -Verb RunAs -ArgumentList "-ExecutionPolicy Bypass -File `"$PSCommandPath`" -InstallDir `"$InstallDir`" -Domain `"$Domain`" -Seconds $Seconds"; return }
    throw 'Run this in an elevated (Administrator) PowerShell.'
}

$caddy     = Join-Path $InstallDir 'caddy.exe'
$caddyfile = Join-Path $InstallDir 'Caddyfile'
Say "=== Diagnose website ($Domain) ===" 'White'
Say "Caddy dir: $InstallDir"

Head "1. Files + config"
if (-not (Test-Path $caddy))     { Say "  MISSING: $caddy - re-run setup-caddy.ps1" 'Red'; return }
if (-not (Test-Path $caddyfile)) { Say "  MISSING: $caddyfile - re-run setup-caddy.ps1" 'Red'; return }
Say "  caddy.exe + Caddyfile present." 'Green'
$val = (& $caddy validate --config $caddyfile 2>&1 | Out-String).Trim()
if ($LASTEXITCODE -eq 0) { Say "  Caddyfile VALID." 'Green' } else { Say "  Caddyfile INVALID:" 'Red'; Say "  $val" 'Yellow' }

Head "2. DigiStampCaddy task"
$task = Get-ScheduledTask -TaskName DigiStampCaddy -ErrorAction SilentlyContinue
if ($task) {
    $info = $task | Get-ScheduledTaskInfo
    Say ("  State=" + $task.State + "  LastRun=" + $info.LastRunTime + "  LastResult=" + $info.LastTaskResult + " (0 = ok)")
} else { Say "  DigiStampCaddy task NOT found - run setup-caddy.ps1 once." 'Yellow' }

Head "3. Ports 80/443 (conflicts)"
$listen = Get-NetTCPConnection -State Listen -ErrorAction SilentlyContinue | Where-Object { $_.LocalPort -in 80, 443 }
if ($listen) {
    foreach ($c in $listen) {
        $owner = (Get-Process -Id $c.OwningProcess -ErrorAction SilentlyContinue).ProcessName
        Say ("  {0} listening (pid {1} = {2})" -f $c.LocalPort, $c.OwningProcess, $owner) 'White'
    }
} else { Say "  Nothing listening on 80/443 yet (expected if Caddy is down)." 'Gray' }

Head "4. Inbound firewall for 80/443"
$fwOpen = @{}
foreach ($p in 80, 443) {
    $rules = Get-NetFirewallPortFilter -Protocol TCP -ErrorAction SilentlyContinue | Where-Object { $_.LocalPort -eq "$p" }
    $allowed = $false
    foreach ($r in $rules) {
        try { $fr = $r | Get-NetFirewallRule -ErrorAction SilentlyContinue | Where-Object { $_.Enabled -eq 'True' -and $_.Direction -eq 'Inbound' -and $_.Action -eq 'Allow' }; if ($fr) { $allowed = $true } } catch {}
    }
    $fwOpen[$p] = $allowed
    if ($allowed) { Say "  TCP $p : inbound allow rule present." 'Green' } else { Say "  TCP $p : NO inbound allow rule (Caddy's ACME challenge needs this open to the world)." 'Yellow' }
}

Head "5. Public IP vs DNS"
$pubIp = $null; try { $pubIp = (Invoke-RestMethod 'https://api.ipify.org?format=json' -TimeoutSec 10).ip } catch {}
$dnsIp = $null; try { $dnsIp = (Resolve-DnsName $Domain -Type A -ErrorAction Stop | Where-Object { $_.IPAddress } | Select-Object -First 1).IPAddress } catch {}
Say ("  This box public IP : " + $(if ($pubIp) { $pubIp } else { '(unknown)' }))
Say ("  $Domain resolves to: " + $(if ($dnsIp) { $dnsIp } else { '(unknown)' }))
if ($pubIp -and $dnsIp) {
    if ($pubIp -eq $dnsIp) { Say "  MATCH - DNS points at this box." 'Green' }
    else { Say "  MISMATCH - DNS does not point at this box. Update the A-record to $pubIp, or the cert challenge will fail." 'Red' }
}

Head "6. Foreground caddy run (captures the real error)"
try { Stop-ScheduledTask -TaskName DigiStampCaddy -ErrorAction SilentlyContinue } catch {}
Get-Process caddy -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 1
$outLog = Join-Path $env:TEMP 'caddy-diag.out.log'
$errLog = Join-Path $env:TEMP 'caddy-diag.err.log'
Remove-Item $outLog, $errLog -ErrorAction SilentlyContinue
Say "  starting caddy for ${Seconds}s..."
$p = Start-Process -FilePath $caddy -ArgumentList "run --config `"$caddyfile`"" -WorkingDirectory $InstallDir `
        -RedirectStandardOutput $outLog -RedirectStandardError $errLog -PassThru -WindowStyle Hidden
for ($i = 0; $i -lt $Seconds -and -not $p.HasExited; $i++) { Start-Sleep -Seconds 1 }
$exited = $p.HasExited
$bound443 = [bool](Get-NetTCPConnection -LocalPort 443 -State Listen -ErrorAction SilentlyContinue)
if (-not $p.HasExited) { try { $p | Stop-Process -Force -ErrorAction SilentlyContinue } catch {} }
Start-Sleep -Seconds 1
$log = ((Get-Content $errLog, $outLog -ErrorAction SilentlyContinue) -join "`n").Trim()
if ($exited) {
    Say "  Caddy EXITED immediately - it is failing to start. Output:" 'Red'
    if ($log) { Write-Host $log -ForegroundColor Yellow } else { Say "  (no output captured)" 'Yellow' }
} elseif ($bound443) {
    Say "  Caddy ran fine and bound 443. The config/cert are OK - the site was just not started." 'Green'
    Say "  Bring it up the normal way:  start-digistamp.ps1 -WebsiteOnly" 'White'
} else {
    Say "  Caddy stayed up but did NOT bind 443 within ${Seconds}s (often still getting a cert). Output:" 'Yellow'
    if ($log) { Write-Host $log -ForegroundColor Yellow }
}

Head "Diagnosis"
if (-not $fwOpen[80] -or -not $fwOpen[443]) { Say "  * 80/443 not open on the Windows firewall - AND they must be forwarded on the router. Caddy needs them reachable from the INTERNET to get its Let's Encrypt cert." 'Yellow' }
if ($pubIp -and $dnsIp -and $pubIp -ne $dnsIp) { Say "  * DNS A-record mismatch (above) - fix it so the cert challenge can validate." 'Yellow' }
Say "  If Caddy's output above mentions 'obtain certificate'/'ACME'/'challenge', the cause is reachability of 80/443 from the internet (router forward + firewall + correct DNS), not Caddy itself." 'Gray'
Say "  Once fixed, run:  start-digistamp.ps1 -WebsiteOnly" 'White'

<#
.SYNOPSIS
    Simple NODE-only binary updater. Pulls the latest DigiAssetWindows.exe (+ the
    matching cli) from the GitHub release and swaps them in. No pool, no build.

.DESCRIPTION
    For a plain node box. Downloads the two node exes from the latest release,
    verifies each is a real Windows binary, stops the node CLEANLY (so chain.db
    is not torn), swaps the files, and restarts. The auto-restart scheduled tasks
    are paused during the swap so they cannot relaunch the old exe mid-copy.

    Does nothing if already on the latest release (unless -Force).

.PARAMETER DigiAssetDir  Node folder (default C:\DigiAssetWindows).
.PARAMETER Force         Reinstall even if already on the latest release.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\update-node.ps1
.EXAMPLE
    # one-liner to fetch + run the latest copy of this script itself:
    iwr https://raw.githubusercontent.com/chopperbriano/DigiAssetWindows/master/node/update-node.ps1 -OutFile "$env:TEMP\update-node.ps1" -UseBasicParsing; powershell -ExecutionPolicy Bypass -File "$env:TEMP\update-node.ps1"
#>
[CmdletBinding()]
param(
    [string]$DigiAssetDir = 'C:\DigiAssetWindows',
    [switch]$Force
)
$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$Repo        = 'chopperbriano/DigiAssetWindows'
$Supervisors = @('DigiStampNode', 'DigiStampMaintenance')   # auto-restart tasks to pause during the swap
$Files       = @('DigiAssetWindows.exe', 'DigiAssetWindows-cli.exe')

function Say($m, $c = 'Gray') { Write-Host $m -ForegroundColor $c }

# A real Windows .exe starts with 'MZ' and is not tiny - reject a truncated
# download or an HTML/error page before it overwrites the working binary.
function Test-Exe($p) {
    if (-not (Test-Path $p) -or (Get-Item $p).Length -lt 100KB) { return $false }
    $b = [byte[]](Get-Content -Path $p -Encoding Byte -TotalCount 2)
    return ($b.Length -eq 2 -and $b[0] -eq 0x4D -and $b[1] -eq 0x5A)
}

# Elevation: stopping the process + writing into C:\DigiAssetWindows needs admin.
$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) {
    if ($PSCommandPath) {
        $a = "-ExecutionPolicy Bypass -File `"$PSCommandPath`" -DigiAssetDir `"$DigiAssetDir`""
        if ($Force) { $a += ' -Force' }
        Start-Process powershell.exe -Verb RunAs -ArgumentList $a
        return
    }
    throw 'Run this in an elevated (Administrator) PowerShell.'
}

Say "=== Update DigiAsset node binaries ===" 'Cyan'
if (-not (Test-Path $DigiAssetDir)) { throw "Node folder not found: $DigiAssetDir (pass -DigiAssetDir)." }

# What is the latest release?
$tag = ''
try { $tag = (Invoke-RestMethod "https://api.github.com/repos/$Repo/releases/latest" -Headers @{ 'User-Agent' = 'node-updater' } -TimeoutSec 20).tag_name } catch {}
Say ("Latest release: " + $(if ($tag) { $tag } else { '(could not read - will still try)' })) 'White'

# Already up to date?
$tagFile = Join-Path $DigiAssetDir '.installed-tag'
if (-not $Force -and $tag -and (Test-Path $tagFile) -and (((Get-Content $tagFile -Raw) + '').Trim() -eq $tag)) {
    Say "Already on $tag - nothing to do. (Use -Force to reinstall.)" 'Green'
    return
}

# Download + validate both exes BEFORE touching anything installed.
$tmp = Join-Path $env:TEMP 'dgb-node-update'
New-Item -ItemType Directory -Force -Path $tmp | Out-Null
$downloaded = @{}
foreach ($f in $Files) {
    $url = "https://github.com/$Repo/releases/latest/download/$f"
    $out = Join-Path $tmp $f
    Say "Downloading $f..."
    $ok = $false
    for ($i = 1; $i -le 3 -and -not $ok; $i++) {
        try { Invoke-WebRequest -Uri $url -OutFile $out -UseBasicParsing -TimeoutSec 180; $ok = (Test-Exe $out) }
        catch { Start-Sleep -Seconds (2 * $i) }
    }
    if (-not $ok) { throw "Could not download a valid $f from the latest release." }
    $downloaded[$f] = $out
}

# Pause the auto-restart tasks so they can't relaunch the old exe during the swap.
$disabled = @()
foreach ($t in $Supervisors) {
    try { if (Get-ScheduledTask -TaskName $t -ErrorAction SilentlyContinue) { Disable-ScheduledTask -TaskName $t -ErrorAction Stop | Out-Null; $disabled += $t } } catch {}
}

try {
    # Stop the node CLEANLY first (a force-kill can tear chain.db); force only if it won't exit.
    $cli = Join-Path $DigiAssetDir 'DigiAssetWindows-cli.exe'
    if (Get-Process DigiAssetWindows -ErrorAction SilentlyContinue) {
        Say "Stopping the node (clean shutdown)..."
        if (Test-Path $cli) { try { & $cli shutdown 2>$null | Out-Null } catch {} }
        for ($i = 0; $i -lt 30 -and (Get-Process DigiAssetWindows -ErrorAction SilentlyContinue); $i++) { Start-Sleep -Seconds 1 }
        Get-Process DigiAssetWindows -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
        Start-Sleep -Seconds 2
    }

    foreach ($f in $Files) {
        Copy-Item -Path $downloaded[$f] -Destination (Join-Path $DigiAssetDir $f) -Force
        Say "  updated $f" 'Green'
    }
    if ($tag) { try { Set-Content -Path $tagFile -Value $tag -Encoding ASCII } catch {} }
} finally {
    foreach ($t in $disabled) { try { Enable-ScheduledTask -TaskName $t -ErrorAction SilentlyContinue | Out-Null } catch {} }
}

# Restart the node now (the re-enabled tasks also keep it up on future boots/logons).
$node = Join-Path $DigiAssetDir 'DigiAssetWindows.exe'
if (Test-Path $node) {
    Say "Restarting the node..."
    Start-Process -FilePath $node -WorkingDirectory $DigiAssetDir -WindowStyle Normal
}
Say ("Done - node updated" + $(if ($tag) { " to $tag" } else { '' }) + ".") 'Green'

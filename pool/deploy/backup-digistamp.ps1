<#
.SYNOPSIS
    Back up DigiStamp pool + node data to a timestamped, rotated folder, and
    (optionally) the DigiByte wallet via RPC.

.DESCRIPTION
    Copies the small, precious data (pool ledger/registrations, configs, local
    assets) into <BackupRoot>\backup-<timestamp>\ and keeps the newest -Keep
    folders. The big chain.db is re-syncable so it's excluded unless you pass
    -IncludeChain.

    BEST RUN WHILE THE APPS ARE STOPPED (e.g. at boot, before start-digistamp.ps1)
    so the SQLite snapshot is clean. If the pool/node are running, all three
    SQLite parts (.db, -wal, -shm) are copied together for a consistent-enough
    snapshot, but a stopped backup is safest.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\backup-digistamp.ps1
    powershell -ExecutionPolicy Bypass -File .\backup-digistamp.ps1 -IncludeChain -Keep 14
#>
[CmdletBinding()]
param(
    # Prefer the current layout (C:\DigiAsset); fall back to the old folder if
    # that is where this box's data actually lives, so an existing pool keeps working.
    [string]$Root       = $(if (Test-Path 'C:\DigiAsset\config.cfg') { 'C:\DigiAsset' } elseif (Test-Path 'C:\DigiAssetWindows\config.cfg') { 'C:\DigiAssetWindows' } else { 'C:\DigiAsset' }),
    [string]$BackupRoot = "",
    [int]   $Keep       = 7,
    [switch]$IncludeChain,
    [switch]$SkipWallet
)
$ErrorActionPreference = "Stop"
if (-not $BackupRoot) { $BackupRoot = Join-Path $Root "backups" }

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

if (-not (Test-Path $Root)) { throw "Data folder not found: $Root" }

$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$dest  = Join-Path $BackupRoot "backup-$stamp"
New-Item -ItemType Directory -Force -Path $dest | Out-Null
Write-Host "=== Backup to $dest ===" -ForegroundColor Cyan

# --- 1. Copy data files ---------------------------------------------------
$files = @("config.cfg", "pool.cfg", "pool.db", "pool.db-wal", "pool.db-shm", "local.db")
if ($IncludeChain) { $files += @("chain.db", "chain.db-wal", "chain.db-shm") }

$copied = 0
foreach ($f in $files) {
    $src = Join-Path $Root $f
    if (Test-Path $src) {
        Copy-Item -LiteralPath $src -Destination $dest -Force
        Write-Host "  + $f"
        $copied++
    }
}
if ($copied -eq 0) { Write-Warning "No data files found in $Root." }

# --- 2. Wallet backup via RPC (safe copy of wallet.dat while Core runs) ----
if (-not $SkipWallet) {
    $cfg  = Read-Cfg (Join-Path $Root "config.cfg")
    $user = $cfg["rpcuser"]; $pass = $cfg["rpcpassword"]
    $port = 14022
    if ($cfg["rpcport"]) { try { $port = [int]$cfg["rpcport"] } catch {} }
    if ($user -and $pass) {
        try {
            # backupwallet needs a path DigiByte Core can write; JSON-escape backslashes.
            $walletOut = (Join-Path $dest "wallet-backup.dat") -replace '\\', '\\'
            $body = '{"jsonrpc":"1.0","id":"backup","method":"backupwallet","params":["' + $walletOut + '"]}'
            $b64  = [Convert]::ToBase64String([Text.Encoding]::ASCII.GetBytes("${user}:${pass}"))
            $null = Invoke-RestMethod -Uri "http://127.0.0.1:$port" -Method Post -Body $body `
                        -Headers @{ Authorization = "Basic $b64" } -ContentType "text/plain" -TimeoutSec 30
            Write-Host "  + wallet-backup.dat (via backupwallet RPC)"
        } catch {
            Write-Warning "Wallet backup skipped: DigiByte Core RPC not reachable (start the wallet, or pass -SkipWallet)."
        }
    } else {
        Write-Warning "Wallet backup skipped: rpcuser/rpcpassword not in config.cfg."
    }
}

# --- 3. Rotate: keep the newest $Keep backups -----------------------------
$all = Get-ChildItem -LiteralPath $BackupRoot -Directory -Filter "backup-*" -ErrorAction SilentlyContinue |
        Sort-Object Name -Descending
if ($all -and $all.Count -gt $Keep) {
    foreach ($old in ($all | Select-Object -Skip $Keep)) {
        Remove-Item -LiteralPath $old.FullName -Recurse -Force
        Write-Host "  - pruned $($old.Name)"
    }
}

$sizeMB = [math]::Round(((Get-ChildItem -LiteralPath $dest -Recurse | Measure-Object Length -Sum).Sum / 1MB), 1)
Write-Host ""
Write-Host "Backup complete: $dest ($sizeMB MB, keeping newest $Keep)." -ForegroundColor Green

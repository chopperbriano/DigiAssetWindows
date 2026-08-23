<#
.SYNOPSIS
    Install, configure, and fast-seed a DigiByte Core wallet on Windows - and
    nothing else. No IPFS, no DigiAsset node, no pool.

    Does the whole job in one pass:
      1. Downloads + silently installs DigiByte Core (pinned 9.26.5).
      2. Writes a complete digibyte.conf (full public service node, or -Lean).
      3. Seeds the blockchain from a pre-synced snapshot, so the wallet syncs
         only the recent delta instead of days/weeks from scratch.
      4. Opens the local firewall for P2P 12024 and tries a UPnP router forward.
      5. Starts the wallet, creates a wallet file, and offers to encrypt it.

    This is the DigiByte half of setup-digiasset.ps1, extracted to stand alone.
    If you want the FULL DigiAsset node stack (DigiByte + IPFS + the node and
    its dashboard), run setup-digiasset.ps1 instead - not this.

.DESCRIPTION
    Seeding is the point. A DigiByte chain sync from genesis takes days; the
    snapshot is a verified tarball of an already-synced blocks/ + chainstate/
    (+ indexes/), so the wallet starts near the tip. The download is resumable
    and SHA256-verified, and every failure path falls back to a normal sync
    rather than leaving you with a broken data directory.

    Re-running is safe. Anything already done is skipped: an existing
    digibyte.conf is topped up (never overwritten, so your RPC password
    survives), and an existing blocks/ folder means the snapshot is left alone.

.PARAMETER DigiByteDir     Install directory. Default C:\DigiByte.
                           Must contain NO spaces (see the NSIS note below).
.PARAMETER DataDir         Blockchain data directory. Default <DigiByteDir>\Data.
                           Pass "$env:APPDATA\DigiByte" for the stock layout.
.PARAMETER DigiByteVersion Version to install. Default 9.26.5. If that tag isn't
                           published, falls back to the current latest release.
.PARAMETER SnapshotUrl     snapshot.json manifest. Defaults to the official feed.
.PARAMETER SkipSeed        Install + configure only; sync normally from genesis.
.PARAMETER Lean            Skip the OPTIONAL service indexes (coinstatsindex,
                           block/bloom filters, digidollar stats) to save disk
                           and CPU. txindex stays on either way.
.PARAMETER Headless        Run digibyted (no window) instead of the digibyte-qt
                           GUI wallet.
.PARAMETER NoStartOnLogon  Install it, but don't auto-start DigiByte at logon.
.PARAMETER NoUpnp          Skip the automatic router port-forward attempt.
.PARAMETER NoEncryptPrompt Don't offer to encrypt the wallet.
.PARAMETER Force           Non-interactive: assume yes, never prompt.

.EXAMPLE
    # Interactive, everything default (C:\DigiByte, seeded, GUI wallet):
    powershell -ExecutionPolicy Bypass -File .\install-digibyte.ps1

.EXAMPLE
    # Stock DigiByte Core data location, unattended:
    powershell -ExecutionPolicy Bypass -File .\install-digibyte.ps1 -DataDir "$env:APPDATA\DigiByte" -Force

.EXAMPLE
    # Headless lean node, no snapshot, no auto-start:
    powershell -ExecutionPolicy Bypass -File .\install-digibyte.ps1 -Headless -Lean -SkipSeed -NoStartOnLogon -Force

.NOTES
    Needs Administrator (it installs software, writes firewall rules, and
    registers a scheduled task) - it relaunches itself elevated if needed.
    Seeding needs tar.exe (Windows 10 1803+ / Windows 11).
#>
[CmdletBinding()]
param(
    [string]$DigiByteDir     = 'C:\DigiByte',
    [string]$DataDir         = '',
    [string]$DigiByteVersion = '9.26.5',
    [string]$SnapshotUrl     = 'https://pub-bd3f441e6b464d499ba583016accfa01.r2.dev/snapshot.json',
    [switch]$SkipSeed,
    [switch]$Lean,
    [switch]$Headless,
    [switch]$NoStartOnLogon,
    [switch]$NoUpnp,
    [switch]$NoEncryptPrompt,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$SCRIPT_VERSION = '1.0.0'

# Did the caller pick a data directory, or are we defaulting? Captured BEFORE we
# elevate so the answer survives the UAC relaunch (we only forward -DataDir when
# it was actually set).
$DataDirExplicit = -not [string]::IsNullOrWhiteSpace($DataDir)
if (-not $DataDirExplicit) { $DataDir = Join-Path $DigiByteDir 'Data' }

$DgbConf   = Join-Path $DigiByteDir 'digibyte.conf'   # conf lives beside Data, not inside it
$LogDir    = Join-Path $DigiByteDir 'logs'
$LogFile   = Join-Path $LogDir      'install-digibyte.log'
$Tmp       = Join-Path $env:TEMP    'digibyte-install'
$TaskWallet = 'DigiByteWallet'      # logon auto-start task
$RpcPort   = 14022

# Node service level, read by Write-DigiByteConf.
$script:LeanNode = [bool]$Lean

# ---------------------------------------------------------------------------
#  Logging
# ---------------------------------------------------------------------------
function Ensure-Dir($p) { if ($p -and -not (Test-Path $p)) { New-Item -ItemType Directory -Force -Path $p | Out-Null } }

function Log {
    param([string]$Message, [string]$Level = 'INFO')
    try {
        Ensure-Dir $LogDir
        if ((Test-Path $LogFile) -and ((Get-Item $LogFile).Length -gt 1MB)) {
            Copy-Item $LogFile "$LogFile.1" -Force -ErrorAction SilentlyContinue
            Clear-Content $LogFile -ErrorAction SilentlyContinue
        }
        Add-Content -Path $LogFile -Encoding UTF8 `
            -Value ('{0}  [{1}] {2}' -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $Level, $Message)
    } catch {}
    $color = switch ($Level) { 'ERROR' {'Red'} 'WARN' {'Yellow'} 'OK' {'Green'} 'STEP' {'Cyan'} default {'Gray'} }
    Write-Host $Message -ForegroundColor $color
}
function Step($n, $msg) { Log ("[$n] $msg") 'STEP' }

# Ask a yes/no question. Under -Force (or a non-interactive host) it answers
# $default without blocking, so unattended runs never hang on a prompt.
function Confirm-Step([string]$prompt, [bool]$default = $true) {
    if ($Force -or -not [Environment]::UserInteractive) { return $default }
    $suffix = if ($default) { '(Y/n)' } else { '(y/N)' }
    $ans = Read-Host "$prompt $suffix"
    if ([string]::IsNullOrWhiteSpace($ans)) { return $default }
    return ($ans -match '^[Yy]')
}

# ---------------------------------------------------------------------------
#  Small helpers
# ---------------------------------------------------------------------------
function Test-Admin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    return ([Security.Principal.WindowsPrincipal]$id).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Invoke-GitHubApi($url) {
    return Invoke-RestMethod -Uri $url -TimeoutSec 30 -Headers @{
        'User-Agent' = 'digibyte-install'
        'Accept'     = 'application/vnd.github+json'
    }
}

# Download with retries. Returns $true on success.
function Get-File($url, $outFile, [int]$tries = 3) {
    Ensure-Dir (Split-Path -Parent $outFile)
    for ($i = 1; $i -le $tries; $i++) {
        try {
            Invoke-WebRequest -Uri $url -OutFile $outFile -UseBasicParsing -TimeoutSec 300
            if ((Test-Path $outFile) -and (Get-Item $outFile).Length -gt 0) { return $true }
        } catch {
            Log ("download attempt $i/$tries failed for $url : $($_.Exception.Message)") 'WARN'
        }
        Start-Sleep -Seconds (3 * $i)
    }
    return $false
}

# Lock digibyte.conf (it holds the RPC password) down to SYSTEM + Administrators,
# so a standard local user can't read the credentials out of it.
function Protect-SecretFile($path) {
    if (-not (Test-Path $path)) { return }
    try {
        # SIDs (not names) so this holds on non-English Windows:
        #   *S-1-5-18 = SYSTEM, *S-1-5-32-544 = Administrators.
        icacls $path /inheritance:r /grant:r '*S-1-5-18:(F)' '*S-1-5-32-544:(F)' 2>&1 | Out-Null
    } catch { Log "  (could not restrict permissions on $path)" 'WARN' }
}

# Parse a "key=value" config file into a hashtable (ignores comments/blanks).
function Read-Conf([string]$path) {
    $h = @{}
    if (Test-Path $path) {
        foreach ($line in Get-Content $path) {
            $t = $line.Trim()
            if ($t -eq '' -or $t.StartsWith('#')) { continue }
            $i = $t.IndexOf('=')
            if ($i -gt 0) { $h[$t.Substring(0,$i).Trim()] = $t.Substring($i+1).Trim() }
        }
    }
    return $h
}

function New-Password([int]$len = 32) {
    # Cryptographically-strong RPC password. Alphanumeric only: DigiByte forbids
    # '@' in rpcuser/rpcpassword, and symbols can trip the key=value config
    # parsing on both sides. 62-char alphabet x 32 => ~190 bits of entropy.
    $chars = foreach ($c in (48..57)+(65..90)+(97..122)) { [char]$c }   # 0-9 A-Z a-z
    $bytes = New-Object 'System.Byte[]' $len
    $rng = New-Object System.Security.Cryptography.RNGCryptoServiceProvider
    try { $rng.GetBytes($bytes) } finally { $rng.Dispose() }
    -join ($bytes | ForEach-Object { $chars[$_ % $chars.Count] })
}

function Test-ProcRunning($name) { return [bool](Get-Process $name -ErrorAction SilentlyContinue) }

# This PC's primary LAN IPv4 - the address a router port-forward points AT.
function Get-LocalIPv4 {
    try {
        $c = Get-NetIPConfiguration -ErrorAction Stop | Where-Object { $_.IPv4DefaultGateway -and $_.IPv4Address } | Select-Object -First 1
        if ($c) { return ($c.IPv4Address | Select-Object -First 1).IPAddress }
    } catch {}
    try {
        $ip = Get-NetIPAddress -AddressFamily IPv4 -ErrorAction Stop |
              Where-Object { $_.IPAddress -notlike '127.*' -and $_.IPAddress -notlike '169.254.*' } |
              Select-Object -First 1
        if ($ip) { return $ip.IPAddress }
    } catch {}
    return $null
}

# ---------------------------------------------------------------------------
#  Firewall / router
# ---------------------------------------------------------------------------
function Open-Port($name, $proto, $port) {
    if (-not (Get-NetFirewallRule -DisplayName $name -ErrorAction SilentlyContinue)) {
        New-NetFirewallRule -DisplayName $name -Direction Inbound -Action Allow -Protocol $proto -LocalPort $port | Out-Null
        Log "  + firewall: allowed inbound $proto $port"
    }
}

# Pre-authorize an app in Windows Firewall so Windows does NOT pop the "Do you
# want to allow this app?" dialog the first time DigiByte listens.
function Add-ProgramAllowRule($name, $exePath) {
    if (-not $exePath -or -not (Test-Path $exePath)) { return }
    try { $exePath = (Resolve-Path -LiteralPath $exePath).Path } catch {}
    $disp = "DigiByte allow $name"
    $existing = Get-NetFirewallRule -DisplayName $disp -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($existing) {
        # A DigiByte Core upgrade moves the exe, and a rule still pointing at the OLD
        # path authorizes nothing - Windows prompts again for the new binary while the
        # name check above says "already done". Compare the path, not just the name.
        $current = ''
        try { $current = ($existing | Get-NetFirewallApplicationFilter -ErrorAction SilentlyContinue).Program } catch {}
        if ($current -and ($current -ieq $exePath)) { return }
        try { Remove-NetFirewallRule -DisplayName $disp -ErrorAction Stop } catch {}
        Log "  firewall: $name moved - refreshing its allow rule"
    }
    try {
        New-NetFirewallRule -DisplayName $disp -Direction Inbound -Action Allow -Program $exePath -Profile Any -ErrorAction Stop | Out-Null
        Log "  + firewall: pre-authorized $name (no popup)"
    } catch { Log "  (could not pre-authorize $name in firewall: $($_.Exception.Message))" 'WARN' }
}

# Best-effort router port-forward via UPnP (IGD) for P2P 12024, so this node can
# accept INBOUND peers instead of only making outbound connections. If UPnP is
# off/unsupported it just no-ops - DigiByte's own upnp=1 also tries on its own.
function Invoke-UpnpForward {
    $ip = Get-LocalIPv4
    if (-not $ip) { Log '  UPnP: could not determine this PC''s LAN IP - skipping.' 'WARN'; return $false }
    try {
        $nat = New-Object -ComObject HNetCfg.NATUPnP
        $col = $nat.StaticPortMappingCollection
        if ($null -eq $col) { Log '  UPnP: no UPnP router found (enable UPnP on the router, or forward 12024 manually).' 'WARN'; return $false }
        try {
            $col.Add(12024, 'TCP', 12024, $ip, $true, 'DigiByte P2P') | Out-Null
            Log ("  UPnP: mapped TCP 12024 -> {0}" -f $ip) 'OK'
            return $true
        } catch {
            Log '  UPnP: router refused TCP 12024 (forward it manually).' 'WARN'
            return $false
        }
    } catch {
        Log '  UPnP: not available on this network - forward TCP 12024 manually.' 'WARN'
        return $false
    }
}

# ---------------------------------------------------------------------------
#  DigiByte install
# ---------------------------------------------------------------------------
function Resolve-DigiByteAsset($tag) {
    # DigiByte ships a win64 NSIS installer (…-win64-setup.exe), not a zip.
    # tag like "v9.26.5"; returns @{ url; name; ver }
    $rel = $null
    try { $rel = Invoke-GitHubApi "https://api.github.com/repos/DigiByte-Core/digibyte/releases/tags/$tag" } catch {}
    if ($rel) {
        $a = $rel.assets | Where-Object { $_.name -match 'win64-setup\.exe$' } | Select-Object -First 1
        if (-not $a) { $a = $rel.assets | Where-Object { $_.name -match 'win64.*\.exe$' } | Select-Object -First 1 }
        if ($a) { return @{ url = $a.browser_download_url; name = $a.name; ver = $tag.TrimStart('v') } }
    }
    $v = $tag.TrimStart('v')
    return @{ url = "https://github.com/DigiByte-Core/digibyte/releases/download/$tag/digibyte-$v-win64-setup.exe"; name = "digibyte-$v-win64-setup.exe"; ver = $v }
}

function Get-DigiByteLatestTag {
    try { return (Invoke-GitHubApi 'https://api.github.com/repos/DigiByte-Core/digibyte/releases/latest').tag_name } catch { return '' }
}

# Does this tag actually have a downloadable win64 installer? Used to fall back
# to the current latest when the pinned version isn't published (yet).
function Test-TagPublished($tag) {
    try { $rel = Invoke-GitHubApi "https://api.github.com/repos/DigiByte-Core/digibyte/releases/tags/$tag"; return [bool]$rel } catch { return $false }
}

function Get-Digibyted {
    $found = Get-ChildItem $DigiByteDir -Recurse -Filter 'digibyted.exe' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($found) { return $found.FullName }
    return (Join-Path $DigiByteDir 'daemon\digibyted.exe')
}
function Get-DigiByteQt {
    $found = Get-ChildItem $DigiByteDir -Recurse -Filter 'digibyte-qt.exe' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($found) { return $found.FullName }
    return (Join-Path $DigiByteDir 'digibyte-qt.exe')
}

# Download the DigiByte win64 setup.exe and install it silently into $DigiByteDir.
function Install-DigiByteBinaries($asset) {
    $inst = Join-Path $Tmp $asset.name
    if (-not (Get-File $asset.url $inst)) { throw "could not download DigiByte from $($asset.url)" }

    # Optional SHA256 verification. DigiByte often doesn't publish a SHA256SUMS
    # file (and a fetch can hiccup) - that's fine, we already downloaded over
    # HTTPS - so an absent/failed checksum is a quiet note, NOT a scary error.
    # A real MISMATCH still aborts.
    try {
        $sumsUrl  = ($asset.url -replace '/[^/]+$', '/SHA256SUMS')
        $sumsFile = Join-Path $Tmp 'DGB_SHA256SUMS'
        $want = $null
        try {
            Invoke-WebRequest -Uri $sumsUrl -OutFile $sumsFile -UseBasicParsing -TimeoutSec 20 -ErrorAction Stop
            if (Test-Path $sumsFile) { $want = (Get-Content $sumsFile | Where-Object { $_ -match [regex]::Escape($asset.name) } | Select-Object -First 1) }
        } catch { $want = $null }
        if ($want) {
            $wantHash = ($want -split '\s+')[0].ToLower()
            $gotHash  = (Get-FileHash $inst -Algorithm SHA256).Hash.ToLower()
            if ($wantHash -ne $gotHash) { Remove-Item $inst -Force; throw 'DigiByte checksum mismatch - aborting.' }
            Log '  DigiByte checksum verified (SHA-256).' 'OK'
        } else {
            Log '  (no published DigiByte checksum; verified via HTTPS instead)'
        }
    } catch {
        if ($_.Exception.Message -match 'mismatch') { throw }
        Log '  (DigiByte checksum step skipped; downloaded over HTTPS)'
    }

    # NSIS silent install. /D=<dir> MUST be the last arg and cannot be quoted, so
    # the install dir must have no spaces (enforced up in the main flow).
    Log "  installing DigiByte $($asset.ver) silently to $DigiByteDir (this can take a minute)..."
    $proc = Start-Process -FilePath $inst -ArgumentList @('/S', "/D=$DigiByteDir") -Wait -PassThru
    if ($proc -and $proc.ExitCode -ne 0) {
        throw "DigiByte installer exited with code $($proc.ExitCode) - the install may be incomplete (locked files?)."
    }
    Start-Sleep -Seconds 3
    $found = Get-ChildItem $DigiByteDir -Recurse -Filter 'digibyted.exe' -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $found) { throw "DigiByte installed but digibyted.exe was not found under $DigiByteDir." }
    Log "  + DigiByte $($asset.ver) -> $($found.FullName)" 'OK'
}

# ---------------------------------------------------------------------------
#  digibyte.conf
# ---------------------------------------------------------------------------
# Writes a complete conf on a fresh box; on a re-run it TOPS UP an existing conf
# with any missing setting instead of overwriting it, so a user's edits and the
# existing RPC password survive.
function Write-DigiByteConf {
    Ensure-Dir $DataDir
    Ensure-Dir $DigiByteDir
    $cfg = Read-Conf $DgbConf
    $rpcUser = $cfg['rpcuser']; $rpcPass = $cfg['rpcpassword']

    # RAM-adaptive dbcache: 25% of physical RAM, capped at 8192 MB, floor 512 -
    # so it never OOMs a small box but still syncs fast on a big one.
    $ramMB = 4096; try { $ramMB = [int]((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1MB) } catch {}
    $dbcache = [Math]::Max(512, [Math]::Min(8192, [int]($ramMB * 0.25)))

    # Base settings every node needs. upnp/natpmp try to auto-forward P2P 12024.
    $required = [ordered]@{
        server='1'; listen='1'; discover='1'; dnsseed='1'; port='12024'; deprecatedrpc='addresses'
        upnp='1'; natpmp='1'
        prune='0'; txindex='1'
        blocksonly='0'; persistmempool='1'; maxmempool='1024'; mempoolexpiry='336'; datacarrier='1'
        maxconnections='400'; maxuploadtarget='0'; dbcache="$dbcache"; par='0'; disablewallet='0'
        digidollar='1'
        rpcport="$RpcPort"; rpcbind='127.0.0.1'; rpcallowip='127.0.0.1'; rpcthreads='16'; rpcworkqueue='128'
        rest='0'; logtimestamps='1'; logips='0'; shrinkdebugfile='1'
    }
    # OPTIONAL service indexes - FULL node only. The snapshot already ships them
    # built, so on a seeded node these cost nothing extra and let this box serve
    # light clients + explorers. -Lean omits them to save disk + CPU.
    if (-not $script:LeanNode) {
        $required['coinstatsindex']       = '1'
        $required['blockfilterindex']     = 'basic'
        $required['peerblockfilters']     = '1'
        $required['peerbloomfilters']     = '1'
        $required['digidollarstatsindex'] = '1'
    }
    $addnodes = @('64.182.71.55:12024','64.182.71.56:12024')

    if (-not $rpcUser -or -not $rpcPass) {
        $rpcUser = 'digibyte'; $rpcPass = New-Password 32
        $svcBlock = ''
        if (-not $script:LeanNode) {
            $svcBlock = @"

# --- Extra service indexes (FULL node) ---------------------------------------
# Serve light clients + explorers. The snapshot ships these already built, so on
# a seeded node they cost nothing. A -Lean node omits them to save disk + CPU.
coinstatsindex=1
blockfilterindex=basic
peerblockfilters=1
peerbloomfilters=1
digidollarstatsindex=1
"@
        }
        $levelLabel = if ($script:LeanNode) { 'Lean' } else { 'Full Public Service' }
        $conf = @"
###############################################################################
# DigiByte Core $levelLabel Node Configuration  (written by install-digibyte.ps1)
#   - Helps the network: inbound peers, tx relay, full historical blocks + indexes
#   - upnp/natpmp try to auto-forward P2P 12024 on your router.
#   - Do NOT expose RPC $RpcPort to the public internet.
# Target: DigiByte Core v9.26.x mainnet
###############################################################################

# --- Node mode ---------------------------------------------------------------
server=1
listen=1
discover=1
dnsseed=1
port=12024
# Address data in RPC output - needed by explorers and DigiAsset-style clients.
deprecatedrpc=addresses
# Try to auto-open the P2P port on the router (UPnP / NAT-PMP).
upnp=1
natpmp=1

# --- Full archival node ------------------------------------------------------
# txindex MUST stay on to match the snapshot - turning it off later forces a
# full reindex, which throws away everything the seed just saved you.
prune=0
txindex=1

# --- Transaction relay / mempool ---------------------------------------------
blocksonly=0
persistmempool=1
maxmempool=1024
mempoolexpiry=336
datacarrier=1

# --- Peers / bandwidth -------------------------------------------------------
maxconnections=400
maxuploadtarget=0

# --- Performance -------------------------------------------------------------
# dbcache auto-sized to 25% of RAM (capped 8192 MB). par=0 auto-detects cores.
dbcache=$dbcache
par=0

# --- Wallet ------------------------------------------------------------------
# Wallet ENABLED. Set to 1 only if this box will never hold or receive coins.
disablewallet=0

# --- DigiDollar --------------------------------------------------------------
digidollar=1
$svcBlock
# --- RPC (LOCAL ONLY - never port-forward $RpcPort) ---------------------------
rpcport=$RpcPort
rpcbind=127.0.0.1
rpcallowip=127.0.0.1
rpcuser=$rpcUser
rpcpassword=$rpcPass
rpcthreads=16
rpcworkqueue=128

# --- REST / logging ----------------------------------------------------------
rest=0
logtimestamps=1
logips=0
shrinkdebugfile=1

# --- Seed peers (DNS discovery usually suffices) -----------------------------
addnode=64.182.71.55:12024
addnode=64.182.71.56:12024
"@
        Set-Content -Path $DgbConf -Value $conf -Encoding ASCII
        Log "  wrote digibyte.conf ($levelLabel node; dbcache=${dbcache}MB) + RPC credentials." 'OK'
    } else {
        # Existing conf: top up any required setting / addnode it is missing.
        $raw = (Get-Content $DgbConf -Raw); $added = @()
        foreach ($k in $required.Keys) {
            if (-not $cfg.ContainsKey($k)) { Add-Content -Path $DgbConf -Value ("$k=" + $required[$k]) -Encoding ASCII; $added += $k }
        }
        foreach ($ip in $addnodes) {
            if ($raw -notmatch [regex]::Escape("addnode=$ip")) { Add-Content -Path $DgbConf -Value "addnode=$ip" -Encoding ASCII; $added += "addnode=$ip" }
        }
        if ($added.Count -gt 0) { Log ("  topped up digibyte.conf (added: {0})" -f ($added -join ', ')) }
        else { Log '  digibyte.conf already complete - leaving it.' }
    }
    return @{ user = $rpcUser; pass = (Read-Conf $DgbConf)['rpcpassword'] }
}

# ---------------------------------------------------------------------------
#  Snapshot seeding
# ---------------------------------------------------------------------------
# Download with a BITS job: live %/speed/ETA, auto-resumes dropped connections
# (and resumes an in-progress job if the script is re-run). Falls back to a
# plain download if BITS is unavailable. Returns $true on success.
function Get-DownloadWithProgress($url, $dest, $label) {
    Import-Module BitsTransfer -ErrorAction SilentlyContinue
    if (Get-Command Start-BitsTransfer -ErrorAction SilentlyContinue) {
        $name = 'DigiByteSeed'
        try {
            $job = Get-BitsTransfer -Name $name -ErrorAction SilentlyContinue | Where-Object { $_.FileList.RemoteName -eq $url } | Select-Object -First 1
            if (-not $job) {
                Get-BitsTransfer -Name $name -ErrorAction SilentlyContinue | Remove-BitsTransfer -ErrorAction SilentlyContinue
                $job = Start-BitsTransfer -Source $url -Destination $dest -DisplayName $name -Asynchronous -Priority Foreground -ErrorAction Stop
            } else { Log '  resuming an in-progress download...' }
            $lastBytes = 0; $lastTime = Get-Date; $lastLogPct = -10
            while ($job.JobState -in 'Connecting','Transferring','Queued','TransientError') {
                if ($job.JobState -eq 'TransientError') { try { $job | Resume-BitsTransfer -Asynchronous -ErrorAction SilentlyContinue } catch {} }
                $bt = $job.BytesTransferred; $tot = $job.BytesTotal; $now = Get-Date
                $secs = ($now - $lastTime).TotalSeconds
                $spd = if ($secs -ge 1) { ($bt - $lastBytes) / $secs } else { $null }
                if ($spd -ne $null) { $lastBytes = $bt; $lastTime = $now }
                if ($tot -gt 0) {
                    $pct = [int](($bt / $tot) * 100)
                    $eta = if ($spd -and $spd -gt 0) { [TimeSpan]::FromSeconds([int](($tot - $bt) / $spd)).ToString() } else { '--:--:--' }
                    Write-Progress -Activity "Downloading $label snapshot" -PercentComplete $pct `
                        -Status ("{0:N1} / {1:N1} GB   {2}%   {3:N1} MB/s   ETA {4}" -f ($bt/1GB),($tot/1GB),$pct,(($(if($spd){$spd}else{0}))/1MB),$eta)
                    if ($pct -ge $lastLogPct + 10) { Log ("  ...download {0}% ({1:N1}/{2:N1} GB)" -f $pct,($bt/1GB),($tot/1GB)); $lastLogPct = $pct }
                }
                Start-Sleep -Seconds 2
            }
            Write-Progress -Activity "Downloading $label snapshot" -Completed
            if ($job.JobState -eq 'Transferred') { Complete-BitsTransfer -BitsJob $job; return $true }
            Log "  download ended in state '$($job.JobState)'." 'WARN'; $job | Remove-BitsTransfer -ErrorAction SilentlyContinue; return $false
        } catch { Log "  (BITS unavailable: $($_.Exception.Message)) - falling back." 'WARN' }
    }
    try { Invoke-WebRequest -Uri $url -OutFile $dest -UseBasicParsing -TimeoutSec 0; return (Test-Path $dest) } catch { return $false }
}

# Extract with a "still working" heartbeat, since tar shows nothing for minutes
# on a huge archive and the heavy disk I/O can look like a freeze.
function Expand-WithProgress($archive, $destDir, $label) {
    # DriveInfo.AvailableFreeSpace does a live syscall every read; Get-PSDrive's
    # .Free is cached and jitters (would show negative deltas mid-extract).
    $drive = (Split-Path $destDir -Qualifier).TrimEnd(':')
    $di = try { New-Object System.IO.DriveInfo $drive } catch { $null }
    $freeBefore = if ($di) { $di.AvailableFreeSpace } else { 0 }
    Log "  extracting $label - heavy disk activity for several minutes; this is NORMAL, not frozen." 'WARN'
    $p = Start-Process -FilePath 'tar.exe' -ArgumentList @('-xzf', "$archive", '-C', "$destDir") -PassThru -WindowStyle Hidden
    $t0 = Get-Date
    while (-not $p.HasExited) {
        Start-Sleep -Seconds 5
        $written = 0; if ($di) { try { $written = [math]::Max(0, $freeBefore - $di.AvailableFreeSpace) } catch {} }
        Write-Progress -Activity "Extracting $label snapshot" -Status ("~{0:N1} GB written   elapsed {1}   (working, please wait...)" -f ($written/1GB), (((Get-Date)-$t0).ToString('hh\:mm\:ss')))
    }
    Write-Progress -Activity "Extracting $label snapshot" -Completed
    return ($p.ExitCode -eq 0)
}

function Get-Snapshot($url, $sha, $destDir, $label) {
    $tmp = Join-Path $Tmp (Split-Path $url -Leaf)
    if (Test-Path $tmp) { Remove-Item $tmp -Force -ErrorAction SilentlyContinue }
    Log "  downloading $label snapshot (large; resumable - safe to leave running)..." 'STEP'
    if (-not (Get-DownloadWithProgress $url $tmp $label)) { Log "  $label download failed - will sync normally." 'WARN'; return $false }
    Log "  verifying $label checksum (reads the whole file, ~a minute)..."
    $h = (Get-FileHash $tmp -Algorithm SHA256).Hash.ToLower()
    if ($h -ne ("$sha").ToLower()) { Remove-Item $tmp -Force -ErrorAction SilentlyContinue; Log "  $label checksum MISMATCH - discarding (will sync normally)." 'WARN'; return $false }
    Log '  checksum OK.' 'OK'
    Ensure-Dir $destDir
    $ok = Expand-WithProgress $tmp $destDir $label
    Remove-Item $tmp -Force -ErrorAction SilentlyContinue
    if (-not $ok) { Log "  $label snapshot extract failed - will sync normally." 'WARN'; return $false }
    return $true
}

# Restore the blockchain from the snapshot manifest so the wallet skips the
# multi-day sync. Only runs when there's no blocks\ already; any failure
# (unreachable / checksum / extract) falls back to a normal sync. Returns the
# snapshot height on success, or $null.
function Restore-Snapshot {
    if (-not $SnapshotUrl) { return $null }
    if (Test-Path (Join-Path $DataDir 'blocks')) {
        Log '  a blockchain is already present in the data directory - not seeding.' 'WARN'
        return $null
    }
    if (-not (Get-Command tar.exe -ErrorAction SilentlyContinue)) {
        Log '  seeding needs tar.exe (Windows 10 1803+); syncing normally.' 'WARN'; return $null
    }
    Log 'Fetching snapshot manifest...' 'STEP'
    # Parse defensively: R2/other hosts may serve .json as octet-stream, in which
    # case Invoke-RestMethod would hand back raw text instead of an object. Fetch
    # the text, strip any UTF-8 BOM, and ConvertFrom-Json ourselves.
    $m = $null
    try {
        $resp = Invoke-WebRequest -Uri $SnapshotUrl -UseBasicParsing -TimeoutSec 30
        $txt = $resp.Content
        if ($txt -is [byte[]]) { $txt = [System.Text.Encoding]::UTF8.GetString($txt) }
        $txt = $txt.TrimStart([char]0xFEFF, [char]0xEF, [char]0xBB, [char]0xBF)
        $m = $txt | ConvertFrom-Json
    } catch { Log '  snapshot manifest unreachable/invalid - syncing normally.' 'WARN'; return $null }
    if (-not $m -or -not $m.baseUrl) { Log '  snapshot manifest has no baseUrl - syncing normally.' 'WARN'; return $null }
    # Defensive: an older/corrupt manifest could carry digibyte as a STRING (a
    # stringified JSON blob, sometimes BOM-prefixed) instead of a nested object.
    if ($m.digibyte -is [string]) {
        try { $m.digibyte = (($m.digibyte).TrimStart([char]0xFEFF, [char]0xEF, [char]0xBB, [char]0xBF) | ConvertFrom-Json) } catch {}
    }
    if (-not $m.digibyte) { Log '  manifest has no DigiByte snapshot - syncing normally.' 'WARN'; return $null }

    $base  = ("$($m.baseUrl)").TrimEnd('/')
    $url   = "$base/$($m.digibyte.file)"
    $szGB  = if ($m.digibyte.sizeBytes) { [double]$m.digibyte.sizeBytes / 1GB } else { 0 }
    Log ("  snapshot: {0}   height {1:N0}   DigiByte {2}{3}" -f `
            $m.digibyte.file, [int]$m.digibyte.height, $m.digibyte.version, `
            $(if ($szGB) { "   ~{0:N1} GB download" -f $szGB } else { '' }))

    # Free-space sanity check. The archive is downloaded AND extracted on the same
    # drive, so peak usage is the compressed file plus everything it unpacks to -
    # block data barely compresses, so budget ~2.5x the download and leave room for
    # the chain to keep growing. Warn (and let the user bail) rather than filling
    # the disk mid-extract, which would leave a half-written data directory.
    if ($szGB -gt 0) {
        $need = $szGB * 2.5
        try {
            $drv  = (Split-Path $DataDir -Qualifier).TrimEnd(':')
            $free = (New-Object System.IO.DriveInfo $drv).AvailableFreeSpace / 1GB
            Log ("  disk: {0:N1} GB free on {1}:  (need roughly {2:N1} GB for download + extract)" -f $free, $drv, $need)
            if ($free -lt $need) {
                Log ("  LOW DISK SPACE - {0:N1} GB free, ~{1:N1} GB recommended." -f $free, $need) 'WARN'
                if (-not (Confirm-Step '  Continue seeding anyway?' $false)) {
                    Log '  seeding skipped by user - will sync normally.' 'WARN'; return $null
                }
            }
        } catch { Log '  (could not check free disk space)' 'WARN' }
    }

    Ensure-Dir $DataDir
    if (Get-Snapshot $url $m.digibyte.sha256 $DataDir 'DigiByte blockchain') {
        Log "  + blockchain seeded (height $([int]$m.digibyte.height))." 'OK'
        return [int]$m.digibyte.height
    }
    return $null
}

# ---------------------------------------------------------------------------
#  Run / RPC / wallet
# ---------------------------------------------------------------------------
function Start-DigiByteWallet {
    if ($Headless) {
        $dgb = Get-Digibyted
        if (-not (Test-Path $dgb)) { return $false }
        Add-ProgramAllowRule 'DigiByte (digibyted)' $dgb
        if (-not (Test-ProcRunning 'digibyted')) {
            Start-Process $dgb -ArgumentList "-datadir=`"$DataDir`" -conf=`"$DgbConf`"" -WindowStyle Hidden
        }
        return $true
    }
    $qt = Get-DigiByteQt
    if (-not (Test-Path $qt)) { return $false }
    Add-ProgramAllowRule 'DigiByte wallet (digibyte-qt)' $qt
    if (-not (Test-ProcRunning 'digibyte-qt')) {
        # Only one process may hold the datadir, so stop the daemon first if a
        # previous headless run left it running.
        if (Test-ProcRunning 'digibyted') {
            Get-Process digibyted -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
            for ($w = 0; $w -lt 15 -and (Test-ProcRunning 'digibyted'); $w++) { Start-Sleep -Seconds 1 }
        }
        Start-Process $qt -ArgumentList "-datadir=$DataDir -conf=$DgbConf"   # paths validated space-free
    }
    return $true
}

# Register the logon auto-start. The guard means a manual launch isn't doubled.
function Register-LogonTask {
    $exe      = if ($Headless) { Get-Digibyted } else { Get-DigiByteQt }
    $procName = if ($Headless) { 'digibyted' } else { 'digibyte-qt' }
    $arguments = "-datadir=$DataDir -conf=$DgbConf"   # arg must be space-free (no quotes)
    $guard = "if (-not (Get-Process '$procName' -ErrorAction SilentlyContinue)) { Start-Process -FilePath '$exe' -WorkingDirectory '$DigiByteDir' -ArgumentList '$arguments' }"
    $a = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument "-WindowStyle Hidden -ExecutionPolicy Bypass -Command `"$guard`""
    $t = New-ScheduledTaskTrigger -AtLogOn
    $u = [Security.Principal.WindowsIdentity]::GetCurrent().Name
    $p = New-ScheduledTaskPrincipal -UserId $u -LogonType Interactive -RunLevel Highest
    $s = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit ([TimeSpan]::Zero)
    Register-ScheduledTask -TaskName $TaskWallet -Action $a -Trigger $t -Principal $p -Settings $s -Force | Out-Null
}

# Generic DigiByte RPC call - returns the parsed .result (throws on failure).
function Invoke-DgbRpc([string]$method, [string]$paramsJson = '[]') {
    $cfg = Read-Conf $DgbConf
    $port = $RpcPort; if ($cfg['rpcport']) { try { $port = [int]$cfg['rpcport'] } catch {} }
    $b64 = [Convert]::ToBase64String([Text.Encoding]::ASCII.GetBytes("$($cfg['rpcuser']):$($cfg['rpcpassword'])"))
    $body = '{"jsonrpc":"1.0","id":"setup","method":"' + $method + '","params":' + $paramsJson + '}'
    $r = Invoke-RestMethod -Uri "http://127.0.0.1:$port" -Method Post -ContentType 'text/plain' `
            -Headers @{ Authorization = "Basic $b64" } -TimeoutSec 20 -Body $body
    return $r.result
}

# Sync progress via RPC (0..1). $null when RPC isn't reachable OR answered without
# a real result - DigiByte can reply 200 with {"result":null,"error":...} while it
# is still starting, and [double]$null silently becomes 0, which would otherwise
# read as "RPC is up, 0% synced" and let the caller march on too early.
function Get-DigiByteProgress {
    try {
        $info = Invoke-DgbRpc 'getblockchaininfo'
        if ($null -eq $info -or $null -eq $info.verificationprogress) { return $null }
        return [double]$info.verificationprogress
    } catch { return $null }
}

# DigiByte RPC just needs to RESPOND - we do NOT wait for a full sync.
function Wait-ForDigiByteRpc([int]$timeoutSec = 300) {
    $tries = [Math]::Max(1, [int]($timeoutSec / 3))
    for ($i = 0; $i -lt $tries; $i++) {
        if ($null -ne (Get-DigiByteProgress)) { return $true }
        Start-Sleep -Seconds 3
    }
    return ($null -ne (Get-DigiByteProgress))
}

# Modern DigiByte Core (Bitcoin Core base) does NOT auto-create a wallet, so a
# fresh node has nothing to receive into. Idempotent.
function Ensure-DigiByteWallet {
    try { $loaded = @(Invoke-DgbRpc 'listwallets'); if ($loaded.Count -gt 0) { return } }
    catch { return }   # RPC not ready yet
    try { Invoke-DgbRpc 'loadwallet' '["wallet"]' | Out-Null; Log '  loaded existing DigiByte wallet.' 'OK'; return } catch {}
    try {
        Invoke-DgbRpc 'createwallet' '["wallet"]' | Out-Null
        Log '  created a DigiByte wallet. ENCRYPT it + back up wallet.dat (see the notes at the end).' 'OK'
    } catch { Log "  could not create a wallet yet: $($_.Exception.Message)" 'WARN' }
}

# Offer to encrypt the wallet. Encrypting requires a passphrase to SPEND;
# receiving still works without one. The passphrase is never stored or logged.
# encryptwallet stops DigiByte, so we restart it afterward.
function Protect-Wallet {
    if ($NoEncryptPrompt -or $Force -or -not [Environment]::UserInteractive) { return }
    Ensure-DigiByteWallet
    $wi = $null
    try { $wi = Invoke-DgbRpc 'getwalletinfo' } catch { return }   # no wallet/RPC yet
    if ($null -ne $wi -and $null -ne $wi.unlocked_until) { Log '  wallet is already encrypted - good.' 'OK'; return }

    Write-Host "`n--- Protect your wallet (recommended) ---" -ForegroundColor Cyan
    Write-Host 'Encrypting means a passphrase is needed to SPEND your coins, so someone with' -ForegroundColor White
    Write-Host 'access to this PC cannot drain it. Receiving still works normally.' -ForegroundColor White
    Write-Host 'WRITE THE PASSPHRASE DOWN and keep it safe - if you lose it, the coins are GONE.' -ForegroundColor Yellow
    Write-Host 'There is no reset or recovery.' -ForegroundColor Yellow
    if ((Read-Host 'Encrypt the wallet now? (Y/n)') -match '^[Nn]') {
        Log '  skipped wallet encryption. You can do it later in DigiByte-Qt: Settings > Encrypt Wallet.' 'WARN'
        return
    }
    for ($i = 0; $i -lt 3; $i++) {
        $sec1 = Read-Host 'Enter a wallet passphrase' -AsSecureString
        $sec2 = Read-Host 'Re-enter to confirm'       -AsSecureString
        $p1 = [Runtime.InteropServices.Marshal]::PtrToStringBSTR([Runtime.InteropServices.Marshal]::SecureStringToBSTR($sec1))
        $p2 = [Runtime.InteropServices.Marshal]::PtrToStringBSTR([Runtime.InteropServices.Marshal]::SecureStringToBSTR($sec2))
        if ($p1 -ne $p2)      { Write-Host '  Passphrases do not match - try again.' -ForegroundColor Yellow; continue }
        if ($p1.Length -lt 8) { Write-Host '  Please use at least 8 characters.'    -ForegroundColor Yellow; continue }
        try {
            # JSON-escape backslash first, then double-quote, so odd passphrases survive.
            $esc = ($p1 -replace '\\', '\\\\') -replace '"', '\"'
            Invoke-DgbRpc 'encryptwallet' ('["' + $esc + '"]') | Out-Null
            Log '  wallet ENCRYPTED. DigiByte is restarting to apply the change.' 'OK'
            $p1 = $null; $p2 = $null; $esc = $null
            Start-Sleep -Seconds 5
            $proc = if ($Headless) { 'digibyted' } else { 'digibyte-qt' }
            for ($w = 0; $w -lt 30 -and (Test-ProcRunning $proc); $w++) { Start-Sleep -Seconds 1 }
            Start-DigiByteWallet | Out-Null
            Wait-ForDigiByteRpc 180 | Out-Null
        } catch {
            Log "  encryptwallet failed: $($_.Exception.Message). Encrypt later in DigiByte-Qt (Settings > Encrypt Wallet)." 'WARN'
        }
        break
    }
}

# ---------------------------------------------------------------------------
#  Main
# ---------------------------------------------------------------------------

# Elevate first - we install software, write firewall rules, and register a task.
if (-not (Test-Admin)) {
    if ($PSCommandPath) {
        $fwd = "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" -DigiByteDir `"$DigiByteDir`" -DigiByteVersion `"$DigiByteVersion`" -SnapshotUrl `"$SnapshotUrl`""
        # Only forward -DataDir when it was actually set, so the elevated instance
        # still derives it from -DigiByteDir when the user didn't pick one.
        if ($DataDirExplicit) { $fwd += " -DataDir `"$DataDir`"" }
        foreach ($s in 'SkipSeed','Lean','Headless','NoStartOnLogon','NoUpnp','NoEncryptPrompt','Force') {
            if ((Get-Variable $s -ValueOnly)) { $fwd += " -$s" }
        }
        Write-Host 'Requesting Administrator privileges...' -ForegroundColor Cyan
        Start-Process powershell.exe -Verb RunAs -ArgumentList $fwd
        return
    }
    throw 'Run this in an elevated (Administrator) PowerShell.'
}

Ensure-Dir $Tmp
Ensure-Dir $LogDir

Log "===== DigiByte Core - install + configure + seed  (script v$SCRIPT_VERSION) =====" 'OK'

# The NSIS installer's /D= switch cannot be quoted, and DigiByte is launched with
# unquoted -datadir=/-conf= arguments. A path with a space silently truncates
# both, so refuse up front with a clear message instead of half-installing.
foreach ($pair in @(@{n='-DigiByteDir'; v=$DigiByteDir}, @{n='-DataDir'; v=$DataDir})) {
    if ($pair.v -match '\s') {
        throw "$($pair.n) '$($pair.v)' contains a space. DigiByte's silent installer and command line cannot handle that - use a path with no spaces (e.g. C:\DigiByte)."
    }
}

Log "  install dir : $DigiByteDir"
Log "  data dir    : $DataDir"
Log "  config      : $DgbConf"
Log "  log         : $LogFile"
Log "  mode        : $(if ($Headless) { 'headless daemon (digibyted)' } else { 'GUI wallet (digibyte-qt)' }), $(if ($Lean) { 'lean' } else { 'full service' }) node"

# --- 1. Install DigiByte Core ----------------------------------------------
$tag = "v$DigiByteVersion"
if (-not (Test-TagPublished $tag)) {
    $latest = Get-DigiByteLatestTag
    if ($latest) {
        Log "  DigiByte $tag isn't published - falling back to the current latest ($latest)." 'WARN'
        $tag = $latest
    } else {
        Log "  could not reach the GitHub release API - trying $tag directly." 'WARN'
    }
}
Step 1 "Installing DigiByte Core $($tag.TrimStart('v'))..."
Install-DigiByteBinaries (Resolve-DigiByteAsset $tag)

# --- 2. Configure -----------------------------------------------------------
Step 2 'Writing digibyte.conf...'
$rpc = Write-DigiByteConf
Protect-SecretFile $DgbConf

# --- 3. Seed ----------------------------------------------------------------
# MUST happen before the first launch: extracting blocks\ + chainstate\ under a
# running DigiByte would corrupt the database it has open.
$seedHeight = $null
if ($SkipSeed) {
    Step 3 'Skipping the snapshot (-SkipSeed) - DigiByte will sync from genesis.'
} else {
    Step 3 'Seeding the blockchain from a snapshot (skips days of syncing)...'
    $seedHeight = Restore-Snapshot
}

# --- 4. Network -------------------------------------------------------------
Step 4 'Opening the local firewall for DigiByte P2P...'
Open-Port 'DigiByte P2P (TCP 12024)' TCP 12024
$upnpOk = $false
if ($NoUpnp) { Log '  UPnP skipped (-NoUpnp).' }
else         { $upnpOk = Invoke-UpnpForward }

# --- 5. Start ---------------------------------------------------------------
Step 5 'Starting DigiByte...'
if (-not $NoStartOnLogon) {
    Register-LogonTask
    Log "  + registered '$TaskWallet' - DigiByte starts automatically at logon." 'OK'
} else {
    Log '  auto-start at logon NOT registered (-NoStartOnLogon).' 'WARN'
}
if (Start-DigiByteWallet) {
    Log '  DigiByte launched; waiting for its RPC to answer (up to 5 minutes)...'
    if (Wait-ForDigiByteRpc 300) {
        Log '  RPC is up.' 'OK'
        Ensure-DigiByteWallet
        Protect-Wallet
    } else {
        Log '  RPC did not answer yet. On a seeded node the first start verifies the' 'WARN'
        Log '  snapshot, which can take a while - this is normal. It will come up.' 'WARN'
    }
} else {
    Log '  could not find the DigiByte executable to start - launch it manually.' 'ERROR'
}

# --- 6. Summary -------------------------------------------------------------
$prog = Get-DigiByteProgress
$blocks = $null
try { $blocks = (Invoke-DgbRpc 'getblockchaininfo').blocks } catch {}

Write-Host ''
Write-Host '=============================================================' -ForegroundColor Green
Write-Host ' DigiByte Core is installed, configured, and running' -ForegroundColor Green
Write-Host '=============================================================' -ForegroundColor Green
Write-Host ''
Write-Host "  Program      : $DigiByteDir"
Write-Host "  Blockchain   : $DataDir"
Write-Host "  Config       : $DgbConf"
Write-Host "  RPC          : 127.0.0.1:$RpcPort  (user '$($rpc.user)' - local only, never port-forward this)"
if ($seedHeight)      { Write-Host "  Seeded to    : block $('{0:N0}' -f $seedHeight)" -ForegroundColor Cyan }
if ($null -ne $blocks){ Write-Host "  Current block: $('{0:N0}' -f [int]$blocks)" }
if ($null -ne $prog)  { Write-Host ("  Sync         : {0:P2} complete" -f $prog) }
Write-Host "  P2P 12024    : local firewall open$(if ($upnpOk) { '; router forwarded via UPnP' } else { '; forward TCP 12024 on your router to accept inbound peers' })"
Write-Host ''
Write-Host '  What happens now:' -ForegroundColor Cyan
if ($seedHeight) {
    Write-Host '   * DigiByte is verifying the snapshot, then syncing only the recent blocks.'
    Write-Host '     Expect minutes-to-hours, not the days a sync from genesis takes.'
} else {
    Write-Host '   * DigiByte is syncing the chain from scratch. This takes days - leave it running.'
}
if (-not $NoStartOnLogon) { Write-Host "   * It restarts automatically at every logon (task '$TaskWallet')." }
Write-Host ''
Write-Host '  IMPORTANT:' -ForegroundColor Yellow
Write-Host '   * Back up your wallet: File > Backup Wallet in DigiByte-Qt, and keep a copy'
Write-Host '     of the backup OFF this PC. A dead disk with no backup means lost coins.'
Write-Host '   * Encrypt the wallet if you skipped it: Settings > Encrypt Wallet.'
Write-Host "   * Do NOT set txindex=0 or prune=1 in $DgbConf - either one throws away the"
Write-Host '     seeded data and forces a full resync.'
Write-Host ''
Write-Host "  Log: $LogFile"
Write-Host ''

Log "Done (script v$SCRIPT_VERSION)." 'OK'

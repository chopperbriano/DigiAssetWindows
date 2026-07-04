<#
.SYNOPSIS
    One script that installs AND maintains a full DigiAsset for Windows node on a
    fresh PC, then keeps it updated and healthy on every restart.

    Stack it sets up (all automatic, all start on boot):
      * DigiByte Core wallet (digibyted)  -> C:\DigiByte   (blockchain + RPC)
      * IPFS / kubo daemon                -> C:\DigiAsset  (file storage)
      * DigiAsset for Windows node        -> C:\DigiAsset  (the node + dashboard)

.DESCRIPTION
    TWO MODES in one file:

    -Mode Install  (default, run it yourself the first time)
        Interactive, newbie-friendly walkthrough. Downloads and installs the
        pinned baseline versions (DigiByte 9.26.4, kubo 0.49.0, latest
        DigiAsset for Windows), writes every config file for you, opens the
        local firewall, registers all the boot tasks, tests internet
        reachability, and installs itself as a maintenance task.

    -Mode Service  (runs itself automatically at every boot, as SYSTEM)
        Non-interactive. Checks GitHub / IPFS for newer versions of ALL THREE
        components and updates them (verified downloads, graceful restarts),
        then health-checks the whole stack and AGGRESSIVELY self-heals
        (restart tasks, re-download corrupt files, re-open firewall). Only
        pops a Windows alert if healing fails. Everything is logged to
        C:\DigiAsset\logs.

.USAGE
    Right-click > Run with PowerShell (it will ask for Administrator), or:
      powershell -ExecutionPolicy Bypass -File .\setup-digiasset.ps1

    One-liner (paste into an Administrator PowerShell):
      iwr https://raw.githubusercontent.com/chopperbriano/DigiAssetWindows/master/setup-digiasset.ps1 -OutFile "$env:TEMP\setup-digiasset.ps1" -UseBasicParsing; powershell -ExecutionPolicy Bypass -File "$env:TEMP\setup-digiasset.ps1"

.NOTES
    DigiByte's first blockchain sync takes hours and runs in the background.
    Re-running Install is safe; it skips anything already done.
#>
[CmdletBinding()]
param(
    [ValidateSet('Install','Service','LaunchNode')]
    [string]$Mode           = 'Install',
    [string]$DigiByteDir    = 'C:\DigiByte',
    [string]$DigiAssetDir   = 'C:\DigiAsset',
    [string]$PayoutAddress  = '',
    [string]$PoolServer     = 'https://pool.digistamp.co',
    # Pinned baseline versions used for a FIRST install. Service mode then
    # tracks the latest releases and updates past these. If a pinned version
    # isn't published yet, the installer falls back to the current latest.
    [string]$DigiByteVersion = '9.26.4',
    # Fast-sync snapshot manifest (snapshot.json) URL. If set, a FRESH install
    # downloads + verifies + extracts a pre-synced DigiByte blockchain + chain.db
    # so it skips the ~week-long sync. Overrides $DefaultSnapshotUrl below.
    [string]$SnapshotUrl = '',
    # By default the GUI apps (DigiByte wallet, IPFS Desktop, node dashboard)
    # auto-start when you log in. Pass -NoStartOnLogon to install them but NOT
    # register the logon auto-start (you'd start them by hand).
    [switch]$NoStartOnLogon
)

$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

# ---------------------------------------------------------------------------
#  Constants
# ---------------------------------------------------------------------------
$SCRIPT_VERSION = '2.9.0'
$Repo           = 'chopperbriano/DigiAssetWindows'
$RawScriptUrl   = "https://raw.githubusercontent.com/$Repo/master/setup-digiasset.ps1"
# Fast-sync snapshot manifest (snapshot.json on your Cloudflare R2). Set this to
# your public URL to turn fast-sync ON for everyone; leave '' to sync normally.
# The -SnapshotUrl parameter overrides it. Until the snapshot files are uploaded,
# a fetch just fails and the installer syncs normally (safe).
$DefaultSnapshotUrl = 'https://pub-bd3f441e6b464d499ba583016accfa01.r2.dev/snapshot.json'

$DgbData        = Join-Path $DigiByteDir  'data'          # blockchain + digibyte.conf
# DigiByte ships a win64 NSIS installer (not a zip). After a silent install the
# daemon lands at <DigiByteDir>\daemon\digibyted.exe; we discover the real path
# at install time and remember it here (see Get-Digibyted).
$DgbExeMarker   = Join-Path $DigiAssetDir 'state\digibyted-path.txt'
$DgbExeDefault  = Join-Path $DigiByteDir  'daemon\digibyted.exe'
$NodeExe        = Join-Path $DigiAssetDir 'DigiAssetWindows.exe'
$CliExe         = Join-Path $DigiAssetDir 'DigiAssetWindows-cli.exe'
$PoolExe        = Join-Path $DigiAssetDir 'DigiAssetPoolServer.exe'  # present only on a pool box
$IpfsExe        = Join-Path $DigiAssetDir 'ipfs.exe'
$IpfsRepo       = Join-Path $DigiAssetDir 'ipfs-repo'
$NodeConfig     = Join-Path $DigiAssetDir 'config.cfg'
$DgbConf        = Join-Path $DgbData      'digibyte.conf'
$LogDir         = Join-Path $DigiAssetDir 'logs'
$LogFile        = Join-Path $LogDir       'setup.log'
$StateFile      = Join-Path $DigiAssetDir 'state\versions.json'
$InstalledScript= Join-Path $DigiAssetDir 'setup-digiasset.ps1'
$Tmp            = Join-Path $env:TEMP     'digiasset-setup'

$TaskDigiByte   = 'DigiStampDigiByte'   # legacy headless task (removed on upgrade)
$TaskIpfs       = 'DigiStampIPFS'       # legacy headless task (removed on upgrade)
$TaskWallet     = 'DigiStampWallet'     # DigiByte GUI wallet, visible at logon
$TaskNode       = 'DigiStampNode'       # DigiAsset node dashboard, visible at logon
$TaskMaint      = 'DigiStampMaintenance'

# GUI apps. IPFS Desktop (Electron) installs per-user and registers its own
# login auto-start; it exposes the same :5001 API the node uses.
$IpfsDesktopRepo = 'ipfs/ipfs-desktop'
$IpfsDesktopExe  = Join-Path $env:LOCALAPPDATA 'Programs\IPFS Desktop\IPFS Desktop.exe'

# Ports we open on the LOCAL firewall (the router forward is 4001 only).
$RpcPort        = 14022

# ---------------------------------------------------------------------------
#  Logging + alerting
# ---------------------------------------------------------------------------
function Ensure-Dir($p) { if ($p -and -not (Test-Path $p)) { New-Item -ItemType Directory -Force -Path $p | Out-Null } }

function Log {
    param([string]$Message, [string]$Level = 'INFO')
    try {
        Ensure-Dir $LogDir
        # Rotate at ~1 MB, keep one previous.
        if ((Test-Path $LogFile) -and ((Get-Item $LogFile).Length -gt 1MB)) {
            Copy-Item $LogFile "$LogFile.1" -Force -ErrorAction SilentlyContinue
            Clear-Content $LogFile -ErrorAction SilentlyContinue
        }
        $line = ('{0}  [{1}] {2}' -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $Level, $Message)
        Add-Content -Path $LogFile -Value $line -Encoding UTF8
    } catch {}
    $color = switch ($Level) { 'ERROR' {'Red'} 'WARN' {'Yellow'} 'OK' {'Green'} 'STEP' {'Cyan'} default {'Gray'} }
    Write-Host $Message -ForegroundColor $color
}

function Step($n, $msg) { Log ("[$n] $msg") 'STEP' }

# Pop a visible Windows alert (all sessions) AND log it. Used only when
# aggressive auto-heal has already failed.
function Alert([string]$msg) {
    Log ("ALERT: $msg") 'ERROR'
    try {
        # msg.exe takes a single-line message as its last arg, so keep it short
        # and put the detail in the log + the desktop breadcrumb below.
        $oneLine = ('DigiAsset node needs attention - see {0}' -f $LogFile)
        Start-Process -FilePath 'msg.exe' -ArgumentList @('*', '/TIME:120', $oneLine) -WindowStyle Hidden -ErrorAction SilentlyContinue
    } catch {}
    # Also drop a breadcrumb on the public desktop so it can't be missed.
    try {
        $pub = Join-Path ([Environment]::GetFolderPath('CommonDesktopDirectory')) 'DigiAsset - ACTION NEEDED.txt'
        Set-Content -Path $pub -Value "$([DateTime]::Now)`r`n$msg`r`n`r`nLog: $LogFile" -Encoding UTF8 -ErrorAction SilentlyContinue
    } catch {}
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
        'User-Agent' = 'digiasset-setup'
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

function Get-Sha512Hex($file) { (Get-FileHash -Path $file -Algorithm SHA512).Hash.ToLower() }

# Is $latest strictly newer than $current? Numeric compare when both parse as
# versions; otherwise "different means update" (safe: after updating we store
# the new tag, so it can't loop).
function Test-Newer($latest, $current) {
    if ([string]::IsNullOrWhiteSpace($latest))  { return $false }
    if ([string]::IsNullOrWhiteSpace($current)) { return $true }
    $l = $latest.TrimStart('v'); $c = $current.TrimStart('v')
    try { return ([version]$l -gt [version]$c) } catch { return ($l -ne $c) }
}

function Read-State {
    # Always return an object with all four fields present, so Service mode can
    # assign to them even if an older/partial state file is on disk (assigning
    # to a missing property of a PSCustomObject throws in PowerShell 5.1).
    $d = @{ digibyte = ''; kubo = ''; digiasset = ''; script = '' }
    if (Test-Path $StateFile) {
        try {
            $j = Get-Content $StateFile -Raw | ConvertFrom-Json
            foreach ($k in @('digibyte','kubo','digiasset','script')) {
                if (($j.PSObject.Properties.Name -contains $k) -and $j.$k) { $d[$k] = [string]$j.$k }
            }
        } catch {}
    }
    return [pscustomobject]$d
}
function Write-State($state) {
    Ensure-Dir (Split-Path -Parent $StateFile)
    ($state | ConvertTo-Json) | Set-Content -Path $StateFile -Encoding UTF8
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

function Test-PortOpen4001 {
    try { return ((Invoke-RestMethod 'https://ifconfig.co/port/4001' -TimeoutSec 15).reachable -eq $true) }
    catch { return $null }   # $null = couldn't run the test (don't treat as closed)
}

# The pool publishes its treasury (donation) address + balance at
# <pool>/pool/stats.json. Returns the parsed object, or $null if unreachable.
function Get-TreasuryInfo {
    try { return (Invoke-RestMethod -Uri "$($PoolServer.TrimEnd('/'))/pool/stats.json" -TimeoutSec 15) }
    catch { return $null }
}

# This PC's primary LAN IPv4 - the address the user points their router's port
# forward / NAT rules AT. Returns $null if it can't be determined.
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
    try {
        $v4 = [System.Net.Dns]::GetHostAddresses([System.Net.Dns]::GetHostName()) |
              Where-Object { $_.AddressFamily -eq 'InterNetwork' -and $_.ToString() -notlike '127.*' -and $_.ToString() -notlike '169.254.*' } |
              Select-Object -First 1
        if ($v4) { return $v4.ToString() }
    } catch {}
    return $null
}

# ---------------------------------------------------------------------------
#  Scheduled tasks (idempotent)
# ---------------------------------------------------------------------------
function Register-DaemonTask($name, $exe, $arguments, $workdir) {
    $a = New-ScheduledTaskAction -Execute $exe -Argument $arguments -WorkingDirectory $workdir
    $t = New-ScheduledTaskTrigger -AtStartup
    $p = New-ScheduledTaskPrincipal -UserId 'SYSTEM' -LogonType ServiceAccount -RunLevel Highest
    $s = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
            -RestartCount 5 -RestartInterval (New-TimeSpan -Minutes 2) -ExecutionTimeLimit ([TimeSpan]::Zero)
    Register-ScheduledTask -TaskName $name -Action $a -Trigger $t -Principal $p -Settings $s -Force | Out-Null
}

# Visible logon task that only starts the node if it isn't already running
# (so a manual start + the logon task can't produce two windows).
function Register-GuardedLogonTask($name, $exe, $workdir, $procName, $arguments = '') {
    $argPart = ''
    if ($arguments) { $argPart = " -ArgumentList '$arguments'" }   # arg must be space-free (no quotes)
    $guard = "if (-not (Get-Process '$procName' -ErrorAction SilentlyContinue)) { Start-Process -FilePath '$exe' -WorkingDirectory '$workdir'$argPart }"
    $a = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument "-WindowStyle Hidden -ExecutionPolicy Bypass -Command `"$guard`""
    $t = New-ScheduledTaskTrigger -AtLogOn
    $u = [Security.Principal.WindowsIdentity]::GetCurrent().Name
    $p = New-ScheduledTaskPrincipal -UserId $u -LogonType Interactive -RunLevel Highest
    $s = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit ([TimeSpan]::Zero)
    Register-ScheduledTask -TaskName $name -Action $a -Trigger $t -Principal $p -Settings $s -Force | Out-Null
}

# The node's logon task runs THIS script in -Mode LaunchNode, which waits for
# IPFS + DigiByte to be ready and then (re)starts the node - so the node never
# races its dependencies at login.
function Register-NodeLaunchTask {
    $arg = "-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File `"$InstalledScript`" -Mode LaunchNode -DigiByteDir `"$DigiByteDir`" -DigiAssetDir `"$DigiAssetDir`""
    $a = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument $arg
    $t = New-ScheduledTaskTrigger -AtLogOn
    $u = [Security.Principal.WindowsIdentity]::GetCurrent().Name
    $p = New-ScheduledTaskPrincipal -UserId $u -LogonType Interactive -RunLevel Highest
    $s = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit ([TimeSpan]::Zero)
    Register-ScheduledTask -TaskName $TaskNode -Action $a -Trigger $t -Principal $p -Settings $s -Force | Out-Null
}

# Maintenance task: runs THIS script in -Mode Service at boot, and again every
# 6 hours so issues are caught even without a reboot.
function Register-MaintenanceTask {
    $arg = "-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File `"$InstalledScript`" -Mode Service -DigiByteDir `"$DigiByteDir`" -DigiAssetDir `"$DigiAssetDir`" -PoolServer `"$PoolServer`""
    $a = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument $arg
    $tStart = New-ScheduledTaskTrigger -AtStartup
    try { $tStart.Delay = 'PT2M' } catch {}   # let the daemons come up first (best-effort)
    $tEvery = New-ScheduledTaskTrigger -Once -At ((Get-Date).AddMinutes(10)) `
                -RepetitionInterval (New-TimeSpan -Hours 6) -RepetitionDuration (New-TimeSpan -Days 3650)
    $p = New-ScheduledTaskPrincipal -UserId 'SYSTEM' -LogonType ServiceAccount -RunLevel Highest
    $s = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -StartWhenAvailable `
            -ExecutionTimeLimit (New-TimeSpan -Hours 2)
    Register-ScheduledTask -TaskName $TaskMaint -Action $a -Trigger $tStart,$tEvery -Principal $p -Settings $s -Force | Out-Null
}

function Open-Port($name, $proto, $port) {
    if (-not (Get-NetFirewallRule -DisplayName $name -ErrorAction SilentlyContinue)) {
        New-NetFirewallRule -DisplayName $name -Direction Inbound -Action Allow -Protocol $proto -LocalPort $port | Out-Null
        Log "  + firewall: allowed inbound $proto $port"
    }
}
function Ensure-Firewall {
    Open-Port 'DigiStamp IPFS swarm (TCP 4001)' TCP 4001
    Open-Port 'DigiStamp IPFS swarm (UDP 4001)' UDP 4001
    Open-Port 'DigiByte P2P (TCP 12024)'        TCP 12024
}

# Pre-authorize an app in Windows Firewall so Windows does NOT pop the "Do you
# want to allow this app?" dialog the first time the app listens. Adding an
# allow rule for the program before it starts suppresses that prompt.
function Add-ProgramAllowRule($name, $exePath) {
    if (-not $exePath -or -not (Test-Path $exePath)) { return }
    $disp = "DigiStamp allow $name"
    if (-not (Get-NetFirewallRule -DisplayName $disp -ErrorAction SilentlyContinue)) {
        try {
            New-NetFirewallRule -DisplayName $disp -Direction Inbound -Action Allow -Program $exePath -Profile Any -ErrorAction Stop | Out-Null
            Log "  + firewall: pre-authorized $name (no popup)"
        } catch { Log "  (could not pre-authorize $name in firewall: $($_.Exception.Message))" 'WARN' }
    }
}

# The node/pool exes are MSVC-built and need the Visual C++ x64 runtime
# (MSVCP140.dll, VCRUNTIME140*.dll). Fresh Windows often lacks it, which makes
# DigiAssetWindows.exe fail to launch - so install it if missing.
function Ensure-VCRuntime {
    $sys = Join-Path $env:SystemRoot 'System32'
    if ((Test-Path (Join-Path $sys 'msvcp140.dll')) -and (Test-Path (Join-Path $sys 'vcruntime140.dll'))) {
        Log '  Visual C++ x64 runtime already present.' 'OK'; return
    }
    Log '  Visual C++ x64 runtime missing - installing (the node needs MSVCP140.dll)...'
    $vc = Join-Path $Tmp 'vc_redist.x64.exe'
    if (-not (Get-File 'https://aka.ms/vs/17/release/vc_redist.x64.exe' $vc)) {
        throw 'could not download the Visual C++ x64 runtime (vc_redist.x64.exe).'
    }
    $proc = Start-Process -FilePath $vc -ArgumentList @('/install','/quiet','/norestart') -Wait -PassThru
    $rc = $proc.ExitCode
    if ($rc -eq 0 -or $rc -eq 3010 -or $rc -eq 1638) {
        Log ("  + Visual C++ x64 runtime installed (exit {0})." -f $rc) 'OK'
        if ($rc -eq 3010) { Log '  (a reboot will finalize the VC++ runtime; the node still runs now.)' 'WARN' }
    } else {
        throw "Visual C++ runtime install failed (exit code $rc)."
    }
}

# ---------------------------------------------------------------------------
#  Service health probes
# ---------------------------------------------------------------------------
# DigiByte sync progress via RPC. Returns $null if it can't be reached.
function Get-DigiByteProgress {
    $cfg = Read-Conf $DgbConf
    if (-not $cfg['rpcuser']) { return $null }
    $port = $RpcPort; if ($cfg['rpcport']) { try { $port = [int]$cfg['rpcport'] } catch {} }
    try {
        $b64 = [Convert]::ToBase64String([Text.Encoding]::ASCII.GetBytes("$($cfg['rpcuser']):$($cfg['rpcpassword'])"))
        $r = Invoke-RestMethod -Uri "http://127.0.0.1:$port" -Method Post -ContentType 'text/plain' `
                -Headers @{ Authorization = "Basic $b64" } -TimeoutSec 8 `
                -Body '{"jsonrpc":"1.0","id":"m","method":"getblockchaininfo","params":[]}'
        return [double]$r.result.verificationprogress
    } catch { return $null }
}
function Test-IpfsUp {
    $api = 'http://127.0.0.1:5001/api/v0/'
    $cfg = Read-Conf $NodeConfig
    if ($cfg['ipfspath']) { $api = $cfg['ipfspath']; if (-not $api.EndsWith('/')) { $api += '/' } }
    try { Invoke-RestMethod -Uri ($api + 'id') -Method Post -TimeoutSec 8 | Out-Null; return $true } catch { return $false }
}
function Test-ProcRunning($name) { return [bool](Get-Process $name -ErrorAction SilentlyContinue) }

# --- Dependency waits -------------------------------------------------------
# The DigiAsset node depends on BOTH IPFS (it bootstraps its DB over IPFS and
# pins content) and DigiByte Core RPC (it reads the chain). Starting the node
# before those are ready is what causes "IPFS Exception: Timeout". These poll
# until the dependency is ready (or a timeout), so we never launch the node
# into a dependency that isn't up yet.
function Wait-ForIpfs([int]$timeoutSec = 300) {
    $tries = [Math]::Max(1, [int]($timeoutSec / 3))
    for ($i = 0; $i -lt $tries; $i++) {
        if (Test-IpfsUp) { return $true }
        Start-Sleep -Seconds 3
    }
    return (Test-IpfsUp)
}
# DigiByte RPC just needs to RESPOND - the node then follows the sync as it
# progresses, so we do NOT wait for a full sync (that takes hours/days).
function Wait-ForDigiByteRpc([int]$timeoutSec = 300) {
    $tries = [Math]::Max(1, [int]($timeoutSec / 3))
    for ($i = 0; $i -lt $tries; $i++) {
        if ($null -ne (Get-DigiByteProgress)) { return $true }
        Start-Sleep -Seconds 3
    }
    return ($null -ne (Get-DigiByteProgress))
}

# ---------------------------------------------------------------------------
#  Component installers / updaters
# ---------------------------------------------------------------------------
# Where is digibyted.exe? Remembered at install time, else discovered under the
# install dir, else the standard NSIS location.
function Get-Digibyted {
    if (Test-Path $DgbExeMarker) {
        $p = (Get-Content $DgbExeMarker -Raw).Trim()
        if ($p -and (Test-Path $p)) { return $p }
    }
    $found = Get-ChildItem $DigiByteDir -Recurse -Filter 'digibyted.exe' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($found) { return $found.FullName }
    return $DgbExeDefault
}

function Resolve-DigiByteAsset($tag) {
    # DigiByte ships a win64 NSIS installer (…-win64-setup.exe), not a zip.
    # tag like "v9.26.4"; returns @{ url; name; ver }
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

# Download the DigiByte win64 setup.exe and install it silently into $DigiByteDir,
# then discover and remember the digibyted.exe path.
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
    # the install dir must have no spaces (default C:\DigiByte is fine).
    Log "  installing DigiByte $($asset.ver) silently to $DigiByteDir (this can take a minute)..."
    Start-Process -FilePath $inst -ArgumentList @('/S', "/D=$DigiByteDir") -Wait
    Start-Sleep -Seconds 3
    $found = Get-ChildItem $DigiByteDir -Recurse -Filter 'digibyted.exe' -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $found) { throw "DigiByte installed but digibyted.exe was not found under $DigiByteDir." }
    Ensure-Dir (Split-Path -Parent $DgbExeMarker)
    Set-Content -Path $DgbExeMarker -Value $found.FullName -Encoding UTF8
    Log "  + DigiByte $($asset.ver) -> $($found.FullName)" 'OK'
}

function Stop-DigiByteGracefully {
    $cfg = Read-Conf $DgbConf
    if ($cfg['rpcuser']) {
        $port = $RpcPort; if ($cfg['rpcport']) { try { $port = [int]$cfg['rpcport'] } catch {} }
        try {
            $b64 = [Convert]::ToBase64String([Text.Encoding]::ASCII.GetBytes("$($cfg['rpcuser']):$($cfg['rpcpassword'])"))
            Invoke-RestMethod -Uri "http://127.0.0.1:$port" -Method Post -ContentType 'text/plain' `
                -Headers @{ Authorization = "Basic $b64" } -TimeoutSec 8 `
                -Body '{"jsonrpc":"1.0","id":"stop","method":"stop","params":[]}' | Out-Null
        } catch {}
    }
    for ($i = 0; $i -lt 30 -and (Test-ProcRunning 'digibyted'); $i++) { Start-Sleep -Seconds 2 }
    Get-Process digibyted -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
}

function Start-DigiByte {
    $dgb = Get-Digibyted
    if (-not (Test-Path $dgb)) { return $false }
    Add-ProgramAllowRule 'DigiByte (digibyted)' $dgb
    if (-not (Test-ProcRunning 'digibyted')) {
        Start-Process $dgb -ArgumentList "-datadir=`"$DgbData`"" -WindowStyle Hidden
    }
    return $true
}

function Register-DigiByteTask {
    $dgb = Get-Digibyted
    Register-DaemonTask $TaskDigiByte $dgb "-datadir=`"$DgbData`"" (Split-Path -Parent $dgb)
}

# The GUI wallet (visible, taskbar). Served RPC comes from digibyte-qt with
# server=1 in digibyte.conf. The full DigiByte install puts it at <dir>\digibyte-qt.exe.
function Get-DigiByteQt {
    $found = Get-ChildItem $DigiByteDir -Recurse -Filter 'digibyte-qt.exe' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($found) { return $found.FullName }
    return (Join-Path $DigiByteDir 'digibyte-qt.exe')
}
function Start-DigiByteWallet {
    $qt = Get-DigiByteQt
    if (-not (Test-Path $qt)) { return $false }
    Add-ProgramAllowRule 'DigiByte wallet (digibyte-qt)' $qt
    if (-not (Test-ProcRunning 'digibyte-qt')) {
        Start-Process $qt -ArgumentList "-datadir=$DgbData"   # $DgbData has no spaces
    }
    return $true
}

function Get-KuboLatestVersion {
    try {
        $versions = (Invoke-WebRequest 'https://dist.ipfs.tech/kubo/versions' -UseBasicParsing -TimeoutSec 20).Content -split "`n"
        return (($versions | Where-Object { $_ -and ($_ -notmatch 'rc') } | Select-Object -Last 1).Trim().TrimStart('v'))
    } catch { return '' }
}

# Return a kubo version that actually exists for download: the requested one if
# its Windows zip is published, otherwise the current latest. (The pinned
# baseline may be ahead of what has shipped.)
function Resolve-KuboVersion($requested) {
    $v = "v$requested"
    try {
        Invoke-WebRequest "https://dist.ipfs.tech/kubo/$v/kubo_${v}_windows-amd64.zip" -Method Head -UseBasicParsing -TimeoutSec 20 | Out-Null
        return $requested
    } catch {}
    $latest = Get-KuboLatestVersion
    if ($latest) {
        if ($latest -ne $requested) { Log "  kubo $requested is not published yet; using current latest $latest." 'WARN' }
        return $latest
    }
    throw "no downloadable kubo version found (requested $requested)."
}

function Install-Ipfs($ver) {
    $v   = "v$ver"
    $url = "https://dist.ipfs.tech/kubo/$v/kubo_${v}_windows-amd64.zip"
    $zip = Join-Path $Tmp "kubo_$ver.zip"
    if (-not (Get-File $url $zip)) { throw "could not download kubo from $url" }

    # SHA-512 verify against the published sidecar; abort on a real mismatch.
    try {
        $expected = ((Invoke-WebRequest "$url.sha512" -UseBasicParsing -TimeoutSec 20).Content -split '\s+')[0].Trim().ToLower()
        if ($expected) {
            if ((Get-Sha512Hex $zip) -ne $expected) { Remove-Item $zip -Force; throw 'kubo checksum mismatch - aborting.' }
            Log '  IPFS checksum verified (SHA-512).' 'OK'
        }
    } catch { Log "  (IPFS checksum step skipped: $($_.Exception.Message))" 'WARN' }

    $ex = Join-Path $Tmp 'kubo_extract'
    if (Test-Path $ex) { Remove-Item $ex -Recurse -Force }
    Expand-Archive -Path $zip -DestinationPath $ex -Force
    $wasRunning = Test-ProcRunning 'ipfs'
    if ($wasRunning) { try { Invoke-RestMethod 'http://127.0.0.1:5001/api/v0/shutdown' -Method Post -TimeoutSec 6 | Out-Null } catch {}; Start-Sleep 2; Get-Process ipfs -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue }
    Copy-Item (Join-Path $ex 'kubo\ipfs.exe') -Destination $IpfsExe -Force
    Log "  + IPFS/kubo $ver -> $IpfsExe" 'OK'
}

function Initialize-Ipfs {
    [Environment]::SetEnvironmentVariable('IPFS_PATH', $IpfsRepo, 'Machine')
    $env:IPFS_PATH = $IpfsRepo
    if (-not (Test-Path (Join-Path $IpfsRepo 'config'))) { & $IpfsExe init | Out-Null; Log '  IPFS repo initialised.' }
    try {
        $pubip = (Invoke-RestMethod 'https://api.ipify.org' -TimeoutSec 10).Trim()
        if ($pubip) { & $IpfsExe config --json Addresses.Announce "[`"/ip4/$pubip/tcp/4001`"]" | Out-Null; Log "  IPFS announce set to $pubip:4001" }
    } catch {}
}
function Start-Ipfs {
    if (-not (Test-Path $IpfsExe)) { return $false }
    $env:IPFS_PATH = $IpfsRepo
    Add-ProgramAllowRule 'IPFS (kubo)' $IpfsExe
    if (-not (Test-ProcRunning 'ipfs')) { Start-Process -FilePath $IpfsExe -ArgumentList 'daemon --enable-gc' -WindowStyle Hidden }
    return $true
}

# --- IPFS Desktop (GUI, tray icon) - the run model used by the installer -----
function Get-IpfsDesktopAsset {
    try {
        $rel = Invoke-GitHubApi "https://api.github.com/repos/$IpfsDesktopRepo/releases/latest"
        $a = $rel.assets | Where-Object { $_.name -match 'setup.*win-x64\.exe$' } | Select-Object -First 1
        if (-not $a) { $a = $rel.assets | Where-Object { $_.name -match 'win-x64\.exe$' } | Select-Object -First 1 }
        if ($a) { return @{ url = $a.browser_download_url; name = $a.name; ver = $rel.tag_name.TrimStart('v') } }
    } catch {}
    return $null
}
function Install-IpfsDesktop {
    if (Test-Path $IpfsDesktopExe) { Log '  IPFS Desktop already installed.' 'OK'; return 'installed' }
    $asset = Get-IpfsDesktopAsset
    if (-not $asset) { throw 'could not find the IPFS Desktop installer on GitHub.' }
    $inst = Join-Path $Tmp $asset.name
    if (-not (Get-File $asset.url $inst)) { throw "could not download IPFS Desktop from $($asset.url)" }
    Log "  installing IPFS Desktop $($asset.ver) silently (this can take a minute)..."
    # electron-builder NSIS: /S = silent. It installs per-user, registers its own
    # login auto-start, launches the app, and runs kubo internally on :5001.
    # Run it via `start` in a SEPARATE console so the app it auto-launches doesn't
    # dump its Electron logs into OUR installer window.
    Start-Process -FilePath 'cmd.exe' -ArgumentList "/c start `"`" /wait `"$inst`" /S" -WindowStyle Hidden -Wait
    Start-Sleep -Seconds 3
    if (Test-Path $IpfsDesktopExe) { Log "  + IPFS Desktop $($asset.ver) -> $IpfsDesktopExe" 'OK' }
    else { Log "  + IPFS Desktop $($asset.ver) installed (tray icon appears at login)." 'OK' }
    return $asset.ver
}
function Start-IpfsDesktop {
    if (-not (Test-Path $IpfsDesktopExe)) { return $false }
    # Launch detached (separate console) so its logs don't spam our window.
    if (-not (Test-ProcRunning 'IPFS Desktop')) { Start-Process -FilePath 'cmd.exe' -ArgumentList "/c start `"`" `"$IpfsDesktopExe`"" -WindowStyle Hidden }
    return $true
}

function Get-DigiAssetLatestTag {
    try { return (Invoke-GitHubApi "https://api.github.com/repos/$Repo/releases/latest").tag_name } catch { return '' }
}
function Install-DigiAsset {
    foreach ($f in 'DigiAssetWindows.exe','DigiAssetWindows-cli.exe') {
        $out = Join-Path $DigiAssetDir $f
        $wasRunning = ($f -eq 'DigiAssetWindows.exe') -and (Test-ProcRunning 'DigiAssetWindows')
        if ($wasRunning) { Get-Process DigiAssetWindows -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue; Start-Sleep 2 }
        if (-not (Get-File "https://github.com/$Repo/releases/latest/download/$f" $out)) {
            throw "could not download $f"
        }
        Log "  + $f" 'OK'
    }
}
# Keep the pool server binary in sync on a POOL box. Only touches things if
# DigiAssetPoolServer.exe is already deployed (i.e. this is a pool host). The
# pool + node ship in the SAME release, so this is called when a new release is
# detected. Stops the running pool, swaps the exe, and restarts it (headless is
# fine for a server) so the pool doesn't fall behind the node.
function Update-PoolServer {
    if (-not (Test-Path $PoolExe)) { return }
    Log '  updating DigiAssetPoolServer.exe...' 'STEP'
    $wasRunning = Test-ProcRunning 'DigiAssetPoolServer'
    if ($wasRunning) {
        Get-Process DigiAssetPoolServer -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
        for ($w = 0; $w -lt 20 -and (Test-ProcRunning 'DigiAssetPoolServer'); $w++) { Start-Sleep -Milliseconds 500 }
    }
    Add-ProgramAllowRule 'DigiAsset pool server' $PoolExe
    if (Get-File "https://github.com/$Repo/releases/latest/download/DigiAssetPoolServer.exe" $PoolExe) {
        Log '  + DigiAssetPoolServer.exe updated.' 'OK'
        if ($wasRunning) { Start-Process -FilePath $PoolExe -WorkingDirectory $DigiAssetDir; Log '  pool server restarted.' 'OK' }
    } else { Log '  pool server update download failed.' 'WARN' }
}

function Start-Node {
    if (-not (Test-Path $NodeExe)) { return $false }
    Add-ProgramAllowRule 'DigiAsset node' $NodeExe
    if (-not (Test-ProcRunning 'DigiAssetWindows')) { Start-Process -FilePath $NodeExe -WorkingDirectory $DigiAssetDir }
    return $true
}

# ---------------------------------------------------------------------------
#  Config writers
# ---------------------------------------------------------------------------
function Write-DigiByteConf {
    Ensure-Dir $DgbData
    # Feature settings the node needs (DigiDollar + block/bloom filter indexes),
    # and seed peers. Applied to a fresh conf; missing ones appended to an
    # existing conf on re-run.
    $features = @('digidollar=1','blockfilterindex=1','peerblockfilters=1','peerbloomfilters=1')
    $addnodes = @(
        '191.81.59.115','175.45.182.173','45.76.235.153','24.74.186.115','24.101.88.154',
        '8.214.25.169','47.75.38.245','64.182.71.30','64.182.71.55','64.182.71.56'
    )
    $cfg = Read-Conf $DgbConf
    $rpcUser = $cfg['rpcuser']; $rpcPass = $cfg['rpcpassword']
    if (-not $rpcUser -or -not $rpcPass) {
        $rpcUser = 'digiasset'; $rpcPass = New-Password 32
        $lines = @(
            "rpcuser=$rpcUser","rpcpassword=$rpcPass","rpcbind=127.0.0.1","rpcport=$RpcPort",
            "rpcallowip=127.0.0.1","whitelist=127.0.0.1","listen=1","server=1","txindex=1","deprecatedrpc=addresses"
        )
        $lines += $features
        $lines += ($addnodes | ForEach-Object { "addnode=$_" })
        Set-Content -Path $DgbConf -Value $lines -Encoding ASCII
        Log '  wrote digibyte.conf with fresh RPC credentials + node settings.'
    } else {
        # Existing conf: append any required feature settings + addnodes it is missing.
        $raw = ''; if (Test-Path $DgbConf) { $raw = (Get-Content $DgbConf -Raw) }
        $added = @()
        foreach ($f in $features) {
            $key = ($f -split '=')[0]
            if (-not $cfg.ContainsKey($key)) { Add-Content -Path $DgbConf -Value $f -Encoding ASCII; $added += $key }
        }
        foreach ($ip in $addnodes) {
            if ($raw -notmatch [regex]::Escape("addnode=$ip")) { Add-Content -Path $DgbConf -Value "addnode=$ip" -Encoding ASCII; $added += "addnode=$ip" }
        }
        if ($added.Count -gt 0) { Log ("  added missing digibyte.conf settings: {0}" -f ($added -join ', ')) }
        else { Log '  digibyte.conf already has all settings - leaving it.' }
    }
    return @{ user = $rpcUser; pass = (Read-Conf $DgbConf)['rpcpassword'] }
}

function Write-NodeConfig($rpc) {
    Ensure-Dir $DigiAssetDir
    if (Test-Path $NodeConfig) { Log '  config.cfg already exists - leaving it untouched.'; return }
    $lines = @(
        "rpcbind=127.0.0.1","rpcport=$RpcPort","rpcuser=$($rpc.user)","rpcpassword=$($rpc.pass)",
        "ipfspath=http://localhost:5001/api/v0/",
        "psp1server=$PoolServer","psp1subscribe=1","psp1payout=$PayoutAddress",
        "pruneage=5760","bootstrapchainstate=1"
    )
    Set-Content -Path $NodeConfig -Value $lines -Encoding ASCII
    Log "  + config.cfg (pool=$PoolServer, payout=$PayoutAddress)" 'OK'
}

# Keep the local copy of THIS script current so the maintenance task always
# runs the newest logic. Returns $true if it changed.
function Update-SelfScript {
    try {
        $tmp = Join-Path $Tmp 'setup-latest.ps1'
        if (Get-File $RawScriptUrl $tmp 1) {
            # Only adopt the new copy if it actually PARSES - never overwrite a
            # working maintenance script with a truncated or corrupt download.
            $perr = $null
            $null = [System.Management.Automation.Language.Parser]::ParseFile($tmp, [ref]$null, [ref]$perr)
            if ($perr -and $perr.Count -gt 0) { Log '  (self-update skipped: downloaded script did not parse)' 'WARN'; return $false }
            $new = Get-Sha512Hex $tmp
            $cur = ''
            if (Test-Path $InstalledScript) { $cur = Get-Sha512Hex $InstalledScript }
            if ($new -ne $cur) { Copy-Item $tmp $InstalledScript -Force; Log '  maintenance script self-updated.' 'OK'; return $true }
        }
    } catch { Log "  (self-update skipped: $($_.Exception.Message))" 'WARN' }
    return $false
}

# --- Fast-sync snapshots ----------------------------------------------------
# Download a .tar.gz snapshot, verify its SHA256, and extract into $destDir.
# Resumable (BITS) with an Invoke-WebRequest fallback. Returns $true on success.
# Download with a BITS job: live %/speed/ETA, auto-resumes dropped connections
# (and resumes an in-progress job if the installer is re-run). Falls back to a
# plain download if BITS is unavailable. Returns $true on success.
function Get-DownloadWithProgress($url, $dest, $label) {
    Import-Module BitsTransfer -ErrorAction SilentlyContinue
    if (Get-Command Start-BitsTransfer -ErrorAction SilentlyContinue) {
        $name = 'DigiAssetSnapshot'
        try {
            $job = Get-BitsTransfer -Name $name -ErrorAction SilentlyContinue | Where-Object { $_.FileList.RemoteName -eq $url } | Select-Object -First 1
            if (-not $job) {
                Get-BitsTransfer -Name $name -ErrorAction SilentlyContinue | Remove-BitsTransfer -ErrorAction SilentlyContinue
                $job = Start-BitsTransfer -Source $url -Destination $dest -DisplayName $name -Asynchronous -Priority Foreground -ErrorAction Stop
            } else { Log "  resuming an in-progress download..." }
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
    Log "  checksum OK." 'OK'
    Ensure-Dir $destDir
    $ok = Expand-WithProgress $tmp $destDir $label
    Remove-Item $tmp -Force -ErrorAction SilentlyContinue
    if (-not $ok) { Log "  $label snapshot extract failed - will sync normally." 'WARN'; return $false }
    return $true
}

# On a FRESH install, restore the DigiByte blockchain + chain.db from the snapshot
# manifest so the node skips the multi-day sync. Only touches things not already
# present; any failure (unreachable/checksum/extract) falls back to a normal sync.
function Restore-Snapshot {
    $url = if ($SnapshotUrl) { $SnapshotUrl } else { $DefaultSnapshotUrl }
    if (-not $url) { return }
    if (-not (Get-Command tar.exe -ErrorAction SilentlyContinue)) { Log '  fast-sync needs tar (Win10 1803+); syncing normally.' 'WARN'; return }
    Log 'Fast-sync: fetching snapshot manifest...' 'STEP'
    # Parse defensively: R2/other hosts may serve .json as octet-stream, in which
    # case Invoke-RestMethod would hand back raw text instead of an object. Fetch
    # the text, strip any UTF-8 BOM, and ConvertFrom-Json ourselves.
    $m = $null
    try {
        $resp = Invoke-WebRequest -Uri $url -UseBasicParsing -TimeoutSec 30
        $txt = $resp.Content
        if ($txt -is [byte[]]) { $txt = [System.Text.Encoding]::UTF8.GetString($txt) }
        $m = ($txt.TrimStart([char]0xFEFF)) | ConvertFrom-Json
    } catch { Log '  snapshot manifest unreachable/invalid - syncing normally.' 'WARN'; return }
    if (-not $m -or -not $m.baseUrl) { Log '  snapshot manifest has no baseUrl - syncing normally.' 'WARN'; return }
    $base = ("$($m.baseUrl)").TrimEnd('/')
    if ($m.digibyte -and -not (Test-Path (Join-Path $DgbData 'blocks'))) {
        Ensure-Dir $DgbData
        if (Get-Snapshot "$base/$($m.digibyte.file)" $m.digibyte.sha256 $DgbData 'DigiByte blockchain') {
            Log "  + DigiByte blockchain restored (height $($m.digibyte.height))." 'OK'
        }
    } elseif ($m.digibyte) { Log '  DigiByte data already present - not restoring.' }
    if ($m.chaindb -and -not (Test-Path (Join-Path $DigiAssetDir 'chain.db'))) {
        Ensure-Dir $DigiAssetDir
        if (Get-Snapshot "$base/$($m.chaindb.file)" $m.chaindb.sha256 $DigiAssetDir 'DigiAsset chain.db') {
            Log '  + chain.db restored.' 'OK'
        }
    }
}

# ---------------------------------------------------------------------------
#  INSTALL MODE
# ---------------------------------------------------------------------------
function Invoke-Install {
    # Safety net: the entry point already guarantees elevation before we get
    # here (it relaunches under UAC). This just refuses to run privileged steps
    # if the function is somehow called without admin.
    if (-not (Test-Admin)) { throw 'Administrator rights are required to install.' }

    Ensure-Dir $DigiAssetDir; Ensure-Dir $DigiByteDir; Ensure-Dir $Tmp; Ensure-Dir $LogDir
    Log "===== DigiAsset for Windows - installer (script v$SCRIPT_VERSION) =====" 'OK'

    $localIp = Get-LocalIPv4

    Write-Host ""
    Write-Host "This sets your PC up to HOST DigiAsset content and EARN DGB from the DigiStamp" -ForegroundColor White
    Write-Host "pool. It installs and auto-starts everything, and keeps it updated for you:" -ForegroundColor White
    Write-Host ""
    Write-Host "  * DigiByte Core wallet   (GUI window) -> $DigiByteDir" -ForegroundColor Gray
    Write-Host "  * IPFS Desktop           (tray icon)  -> installed for your user" -ForegroundColor Gray
    Write-Host "  * DigiAsset for Windows  (dashboard)  -> $DigiAssetDir" -ForegroundColor Gray
    Write-Host "  These open as real apps (taskbar + tray) and start when you log in." -ForegroundColor Gray
    Write-Host ""
    Write-Host "BEFORE YOU BEGIN - set up your home router so the internet can reach this node." -ForegroundColor Yellow
    Write-Host "Your PC's Windows firewall is opened automatically, but your ROUTER is NOT -" -ForegroundColor Yellow
    Write-Host "you must add a Port Forward / NAT rule for each port below." -ForegroundColor Yellow
    Write-Host ""
    if ($localIp) {
        Write-Host "  This PC's local IP is  " -ForegroundColor Gray -NoNewline
        Write-Host $localIp -ForegroundColor Green -NoNewline
        Write-Host "   <-- forward the ports below TO this address" -ForegroundColor Gray
    } else {
        Write-Host "  Find this PC's local IP by running:  ipconfig   (use the 'IPv4 Address')" -ForegroundColor Gray
    }
    Write-Host ""
    Write-Host "  On your router, forward these to that local IP:" -ForegroundColor Gray
    Write-Host ""
    Write-Host "     PORT    PROTOCOL   WHAT IT HOSTS" -ForegroundColor Cyan
    Write-Host "     4001    TCP        DigiAsset / IPFS   " -ForegroundColor White -NoNewline
    Write-Host "(REQUIRED - how the pool verifies + pays you)" -ForegroundColor Yellow
    Write-Host "     4001    UDP        DigiAsset / IPFS   (recommended - QUIC, faster peers)" -ForegroundColor White
    Write-Host "     12024   TCP        DigiByte peers     (recommended - helps host DigiByte)" -ForegroundColor White
    Write-Host ""
    Write-Host "  Do NOT forward 5001, 14022, or 8090 - those must stay PRIVATE (local only)." -ForegroundColor Red
    Write-Host ""
    Write-Host "Windows will show a few security popups during install - please APPROVE them all:" -ForegroundColor Yellow
    Write-Host "  * 'Do you want to allow this app to make changes to your device?' (UAC)  -> YES" -ForegroundColor Yellow
    Write-Host "  * 'Allow this app through the firewall?' (DigiByte / IPFS / node)  -> ALLOW (both networks)" -ForegroundColor Yellow
    Write-Host "  They're expected and safe - the install can't finish without them." -ForegroundColor Yellow
    Write-Host ""
    Write-Host "Heads up: DigiByte's first sync can take many HOURS - sometimes a DAY or two." -ForegroundColor Yellow
    Write-Host "It's a big blockchain :)  Just leave the PC on and logged in while it catches up." -ForegroundColor Yellow
    Write-Host ""
    Write-Host "Nothing here spends your coins." -ForegroundColor Green -NoNewline
    Write-Host "  (These router steps are shown again at the end.)" -ForegroundColor DarkGray
    Write-Host ""
    $go = Read-Host 'Press Enter to continue, or type N then Enter to cancel'
    if ($go -match '^[Nn]') { Write-Host 'Cancelled - nothing was changed.' -ForegroundColor Yellow; return }

    # 0. Payout address ------------------------------------------------------
    $treasury = Get-TreasuryInfo   # pool's published treasury address + balance (may be $null if unreachable)
    if (-not $PayoutAddress) {
        Write-Host "`n--- Your payout address ---" -ForegroundColor Cyan
        Write-Host 'The DigiByte address where the pool sends your hosting earnings.' -ForegroundColor White
        Write-Host 'Use an address from a wallet YOU control (starts with D, S, or dgb1).' -ForegroundColor White
        Write-Host ''
        Write-Host 'Please be realistic about earnings:' -ForegroundColor Yellow
        Write-Host '  * Payments are TINY - this is a tip jar for hosting, not a salary.' -ForegroundColor White
        Write-Host '  * You are ONLY paid when the pool TREASURY has funds. The treasury is' -ForegroundColor White
        Write-Host '    shared out among all verified nodes; when it is empty, nobody is paid' -ForegroundColor White
        Write-Host '    that period. The pool never pays money it does not have.' -ForegroundColor White
        Write-Host "  * See the live treasury balance + every payout at $PoolServer" -ForegroundColor White
        if ($treasury -and $treasury.donationAddress) {
            Write-Host "  * Pool treasury (donation) address: $($treasury.donationAddress)" -ForegroundColor Gray
        }
        Write-Host ''
        $script:PayoutAddress = Read-Host '  Paste your DGB payout address'
        $PayoutAddress = $script:PayoutAddress
    }
    if ($PayoutAddress -notmatch '^(D|S|dgb1)[0-9A-Za-z]{6,90}$') {
        throw 'That does not look like a DigiByte address. Re-run and paste a valid D..., S..., or dgb1... address.'
    }

    # --- Prerequisites ------------------------------------------------------
    Log 'Checking prerequisites (Visual C++ x64 runtime the node needs)...' 'STEP'
    Ensure-VCRuntime

    # 1. DigiByte (GUI wallet) -----------------------------------------------
    Step 1 "Installing DigiByte Core $DigiByteVersion (wallet GUI)..."
    Install-DigiByteBinaries (Resolve-DigiByteAsset "v$DigiByteVersion")
    $rpc = Write-DigiByteConf
    Restore-Snapshot   # fast-sync: extract pre-synced blockchain + chain.db before first launch (fresh install only)
    if (Get-ScheduledTask -TaskName $TaskDigiByte -ErrorAction SilentlyContinue) { Unregister-ScheduledTask -TaskName $TaskDigiByte -Confirm:$false }  # drop legacy headless task
    if (-not $NoStartOnLogon) { Register-GuardedLogonTask $TaskWallet (Get-DigiByteQt) $DigiByteDir 'digibyte-qt' "-datadir=$DgbData" }
    Start-DigiByteWallet | Out-Null
    Log '  DigiByte wallet (GUI) running + opens at every logon. Blockchain syncs in the background.' 'OK'

    # 2. IPFS Desktop (GUI) --------------------------------------------------
    Step 2 'Installing IPFS Desktop (GUI, tray icon)...'
    if (Get-ScheduledTask -TaskName $TaskIpfs -ErrorAction SilentlyContinue) { Unregister-ScheduledTask -TaskName $TaskIpfs -Confirm:$false }  # drop legacy headless kubo task
    $ipfsVer = Install-IpfsDesktop
    Start-IpfsDesktop | Out-Null
    Log '  IPFS Desktop running (tray icon) + auto-starts at logon.' 'OK'

    # 3. DigiAsset node ------------------------------------------------------
    Step 3 'Installing DigiAsset for Windows (latest release)...'
    Install-DigiAsset
    Write-NodeConfig $rpc
    # The node depends on IPFS + DigiByte RPC - wait for them before launching so
    # it doesn't FATAL on an IPFS timeout. (The logon task, registered in step 5,
    # does the same wait every login.) Registered/launched after step 5 copies
    # the script the launch task runs.
    Log '  waiting for IPFS + DigiByte to be ready before the node starts...'
    Wait-ForIpfs 300 | Out-Null
    Wait-ForDigiByteRpc 300 | Out-Null
    Start-Node | Out-Null
    Log '  DigiAsset node dashboard started.' 'OK'

    # 4. Firewall ------------------------------------------------------------
    Step 4 'Opening the local Windows firewall for hosting (inbound)...'
    Ensure-Firewall
    Log '  local firewall now allows inbound 4001/TCP + 4001/UDP (DigiAsset/IPFS) and 12024/TCP (DigiByte).' 'OK'
    Log '  You must ALSO forward these on your home router - see the summary below.' 'WARN'

    # 5. Maintenance task ----------------------------------------------------
    Step 5 'Installing the auto-update + self-heal maintenance task...'
    # Stage THIS script at $InstalledScript - the node launch task and the
    # maintenance task both run it. Copy the running file; if that isn't possible
    # (e.g. it was launched from memory), download it. Then confirm + log.
    $staged = $false
    if ($PSCommandPath -and (Test-Path $PSCommandPath)) {
        try {
            if ((Resolve-Path -LiteralPath $PSCommandPath).Path -ne $InstalledScript) {
                Copy-Item -LiteralPath $PSCommandPath -Destination $InstalledScript -Force
            }
            $staged = (Test-Path $InstalledScript)
        } catch {}
    }
    if (-not $staged) {
        try { Get-File $RawScriptUrl $InstalledScript 3 | Out-Null } catch {}
        $staged = (Test-Path $InstalledScript)
    }
    if ($staged) { Log "  installer staged at $InstalledScript" 'OK' }
    else { Log "  WARNING: could not stage $InstalledScript (node will start directly instead)." 'WARN' }

    # Drop the companion tools next to the node so they are always handy.
    foreach ($tool in 'monitor-node.ps1','stop-node.ps1') {
        try { Get-File "https://raw.githubusercontent.com/$Repo/master/$tool" (Join-Path $DigiAssetDir $tool) 2 | Out-Null } catch {}
    }
    # Node logon task. If the script is staged, use the dependency-aware launcher
    # (waits for IPFS + DigiByte). If not, fall back to launching the node exe
    # directly so it still starts at logon (never point the task at a missing file).
    if (-not $NoStartOnLogon) {
        if ($staged) {
            Register-NodeLaunchTask
            Log '  node set to wait for its dependencies + start at every logon.' 'OK'
        } else {
            Register-GuardedLogonTask $TaskNode $NodeExe $DigiAssetDir 'DigiAssetWindows'
            Log '  node set to start at every logon (direct launch).' 'WARN'
        }
    }
    # Record what we installed so Service mode knows the baseline.
    $state = Read-State
    $state.digibyte  = $DigiByteVersion
    $state.kubo      = "$ipfsVer"   # IPFS Desktop version (field name kept for compatibility)
    $state.digiasset = (Get-DigiAssetLatestTag)
    $state.script    = $SCRIPT_VERSION
    Write-State $state
    Register-MaintenanceTask
    Log "  maintenance task '$TaskMaint' registered (boot + every 6h)." 'OK'

    # 6. Reachability --------------------------------------------------------
    Step 6 'Testing whether port 4001 is reachable from the internet...'
    Start-Sleep 2
    $reach = Test-PortOpen4001
    if ($reach -eq $true) { Log '  SUCCESS: port 4001 is OPEN. You are set to be verified + paid.' 'OK' }
    else { Log '  Port 4001 is NOT reachable yet - forward it on your router (below), then re-test.' 'WARN' }

    # Save a treasury/earnings note the user can always find.
    $treasuryNote = Join-Path $DigiAssetDir 'TREASURY.txt'
    try {
        $note = @(
            'DigiStamp pool - earnings & treasury',
            '',
            "Your payout address : $PayoutAddress",
            "Pool                : $PoolServer"
        )
        if ($treasury -and $treasury.donationAddress) {
            $note += "Treasury address    : $($treasury.donationAddress)"
            if ($null -ne $treasury.treasuryBalance) { $note += ("Treasury at install : {0} DGB" -f $treasury.treasuryBalance) }
        }
        $note += @(
            '',
            'Payments are TINY and are only shared when the treasury has funds,',
            'split among all verified nodes. When the treasury is empty, nobody is',
            'paid that period - the pool never pays money it does not have.',
            '',
            "See the live treasury balance + every payout any time at:  $PoolServer"
        )
        Set-Content -Path $treasuryNote -Value $note -Encoding UTF8
    } catch {}

    # Summary ----------------------------------------------------------------
    Write-Host "`n===== Done =====" -ForegroundColor Green
    Write-Host 'Everything is installed, auto-starting on boot, and self-updating.' -ForegroundColor White
    Write-Host ''
    Write-Host 'HOSTING PORTS - your local Windows firewall is ALREADY open for these.' -ForegroundColor Cyan
    Write-Host 'To accept incoming connections you must ALSO forward them on your home router:' -ForegroundColor White
    Write-Host '   TCP 4001    DigiAsset / IPFS hosting  (REQUIRED - the pool verifies + pays you)' -ForegroundColor White
    Write-Host '   UDP 4001    DigiAsset / IPFS (QUIC)   (recommended - faster peer connections)' -ForegroundColor White
    Write-Host '   TCP 12024   DigiByte hosting          (recommended - serve DigiByte peers)' -ForegroundColor White
    Write-Host '   Keep 5001 / 14022 / 8090 PRIVATE - never forward them (they are local-only).' -ForegroundColor Red
    Write-Host ''
    Write-Host 'ABOUT EARNINGS:' -ForegroundColor Cyan
    Write-Host '  * Payments are TINY and only shared when the pool treasury has funds.' -ForegroundColor White
    if ($treasury -and $treasury.donationAddress) {
        Write-Host "  * Pool treasury address: $($treasury.donationAddress)" -ForegroundColor Gray
    }
    Write-Host "  * See what is in the treasury any time at $PoolServer" -ForegroundColor White
    Write-Host "  * Saved for you: $treasuryNote" -ForegroundColor Gray
    Write-Host ''
    Write-Host 'YOUR APPS (taskbar + tray):' -ForegroundColor Cyan
    Write-Host '  * DigiByte wallet + DigiAsset dashboard open as windows; IPFS Desktop sits in the tray.' -ForegroundColor White
    if ($NoStartOnLogon) {
        Write-Host '  * You chose NOT to auto-start on logon - launch them yourself when you want them.' -ForegroundColor Yellow
    } else {
        Write-Host '  * They start automatically every time you LOG IN.' -ForegroundColor White
    }
    Write-Host ''
    Write-Host 'RUN IT UNATTENDED (recommended for an always-on node):' -ForegroundColor Cyan
    Write-Host '  These are desktop apps, so they run while you are LOGGED IN. To have an always-on' -ForegroundColor White
    Write-Host '  node come back up after a reboot with nobody at the keyboard, set the PC to auto-' -ForegroundColor White
    Write-Host '  login using Microsoft Sysinternals Autologon (free, official):' -ForegroundColor White
    Write-Host '     https://learn.microsoft.com/sysinternals/downloads/autologon' -ForegroundColor Green
    Write-Host '  Run it once, enter your Windows username + password, and every boot auto-logs-in and' -ForegroundColor White
    Write-Host '  launches the apps for you.' -ForegroundColor White
    Write-Host ''
    Write-Host 'WHAT HAPPENS NOW:' -ForegroundColor Cyan
    Write-Host '  * DigiByte is syncing the blockchain (hours the first time) - watch it in the wallet.' -ForegroundColor White
    Write-Host '  * The node waits for IPFS + DigiByte to be ready before it starts (a short delay is normal).' -ForegroundColor White
    Write-Host '  * Once synced, the node registers with the pool automatically.' -ForegroundColor White
    Write-Host "  * Update + health checks run on every boot and every 6 hours." -ForegroundColor White
    Write-Host ''
    Write-Host 'HANDY COMMANDS (Administrator PowerShell):' -ForegroundColor Cyan
    Write-Host "  * Check status : powershell -ExecutionPolicy Bypass -File $DigiAssetDir\monitor-node.ps1" -ForegroundColor Gray
    Write-Host "  * Stop / remove: powershell -ExecutionPolicy Bypass -File $DigiAssetDir\stop-node.ps1" -ForegroundColor Gray
    Write-Host "  * Logs         : $LogFile" -ForegroundColor Gray

    # Live status so you can see, at a glance, what's actually up right now.
    Write-Host ''
    Write-Host 'STATUS RIGHT NOW:' -ForegroundColor Cyan
    Start-Sleep -Seconds 3
    if (Test-ProcRunning 'digibyte-qt') { Write-Host '  [OK]   DigiByte wallet    - running (a window is open; it will sync for hours/days)' -ForegroundColor Green }
    else { Write-Host '  [WARN] DigiByte wallet    - not detected (look for its window / re-open it)' -ForegroundColor Yellow }
    if (Test-IpfsUp) { Write-Host '  [OK]   IPFS Desktop       - API responding on :5001' -ForegroundColor Green }
    elseif (Test-ProcRunning 'IPFS Desktop') { Write-Host '  [WARN] IPFS Desktop       - running, API still starting (give it a minute)' -ForegroundColor Yellow }
    else { Write-Host '  [FAIL] IPFS Desktop       - not running (check the tray)' -ForegroundColor Red }
    if (Test-ProcRunning 'DigiAssetWindows') { Write-Host '  [OK]   DigiAsset node      - running' -ForegroundColor Green }
    else { Write-Host '  [WARN] DigiAsset node      - not up yet. It waits for IPFS + DigiByte and retries at' -ForegroundColor Yellow
           Write-Host '                              logon. If its window shows an error, re-run monitor-node.ps1.' -ForegroundColor Yellow }
    Write-Host ''
    Write-Host 'Re-check any time:  powershell -ExecutionPolicy Bypass -File ' -ForegroundColor Gray -NoNewline
    Write-Host "$DigiAssetDir\monitor-node.ps1" -ForegroundColor Green

    # Last thing on screen (most visible) - securing the wallet is a MUST.
    Write-Host ''
    Write-Host '============================================================' -ForegroundColor Yellow
    Write-Host ' ONE MORE THING - CREATE AND ENCRYPT YOUR WALLET (IMPORTANT)' -ForegroundColor Yellow
    Write-Host '============================================================' -ForegroundColor Yellow
    Write-Host '  Your DigiByte wallet holds your private keys. Secure it BEFORE it holds coins:' -ForegroundColor White
    Write-Host '   1. Open the DigiByte wallet (it is starting now / on your taskbar).' -ForegroundColor White
    Write-Host '   2. Let it create a wallet (accept the default), or File > Create Wallet.' -ForegroundColor White
    Write-Host '   3. Settings > Encrypt Wallet -> set a STRONG passphrase and WRITE IT DOWN.' -ForegroundColor White
    Write-Host '      If you lose that passphrase, your coins are GONE - nobody can recover them.' -ForegroundColor Red
    Write-Host '   4. File > Backup Wallet... -> save wallet.dat somewhere safe/offline.' -ForegroundColor White
    Write-Host '   Encrypting does NOT affect RECEIVING payouts; you only unlock when YOU send.' -ForegroundColor Gray
    Write-Host '============================================================' -ForegroundColor Yellow

    # Durable proof of completion (the window may close; this file + log line stay).
    Log "Install completed successfully (script v$SCRIPT_VERSION)." 'OK'
    try { Set-Content -Path (Join-Path $LogDir 'INSTALL-COMPLETE.txt') -Value ("DigiAsset for Windows install completed - script v$SCRIPT_VERSION - " + (Get-Date).ToString('s')) -Encoding ascii } catch {}
}

# ---------------------------------------------------------------------------
#  LAUNCH-NODE MODE  (run by the node's logon task: wait for deps, supervise)
# ---------------------------------------------------------------------------
function Invoke-LaunchNode {
    Ensure-Dir $LogDir
    Log "----- launch-node (script v$SCRIPT_VERSION) -----"
    if (Test-ProcRunning 'DigiAssetWindows') { Log 'launch-node: node already running.' 'OK'; return }

    # The node depends on IPFS (DB bootstrap + pinning) and DigiByte Core RPC.
    # Wait for both so it doesn't FATAL on "IPFS Exception: Timeout". We wait for
    # DigiByte's RPC to RESPOND, not for a full sync - the node follows the sync
    # as it progresses, which can take hours or days.
    Log 'launch-node: waiting for IPFS API (:5001) - IPFS Desktop can take a bit to boot...'
    if (-not (Wait-ForIpfs 300)) { Log 'launch-node: IPFS API still not up after 5 min; starting anyway (node will retry).' 'WARN' }
    Log 'launch-node: waiting for DigiByte RPC to respond (NOT full sync)...'
    if (-not (Wait-ForDigiByteRpc 300)) { Log 'launch-node: DigiByte RPC still not up after 5 min; starting anyway.' 'WARN' }

    # Supervise the launch: if the node dies quickly (a transient IPFS/DigiByte
    # hiccup during the long early sync), re-check the deps and retry, bounded,
    # so it doesn't just give up while its dependencies settle.
    for ($attempt = 1; $attempt -le 8; $attempt++) {
        if (Test-ProcRunning 'DigiAssetWindows') { Log 'launch-node: node is running.' 'OK'; return }
        Start-Node | Out-Null
        Start-Sleep -Seconds 30
        if (Test-ProcRunning 'DigiAssetWindows') { Log "launch-node: node running (attempt $attempt)." 'OK'; return }
        Log "launch-node: node exited quickly (attempt $attempt) - re-checking IPFS/DigiByte and retrying..." 'WARN'
        Wait-ForIpfs 120 | Out-Null
        Wait-ForDigiByteRpc 120 | Out-Null
    }
    Log 'launch-node: node would not stay up after retries - check IPFS Desktop + the DigiByte wallet.' 'WARN'
}

# ---------------------------------------------------------------------------
#  SERVICE MODE  (update + health + aggressive self-heal, non-interactive)
# ---------------------------------------------------------------------------
function Invoke-Service {
    Ensure-Dir $Tmp; Ensure-Dir $LogDir
    Log "----- maintenance run (script v$SCRIPT_VERSION) -----"
    if (-not (Test-Admin)) { Log 'not elevated - some update/heal actions may fail (this normally runs as SYSTEM).' 'WARN' }
    $state = Read-State
    $problems = @()

    # --- 1. Keep this script current for next time -------------------------
    Update-SelfScript | Out-Null

    # --- 2. Prerequisites (independent of the user session) ----------------
    try { Ensure-VCRuntime } catch { $problems += "VC++ runtime: $($_.Exception.Message)"; Log $problems[-1] 'WARN' }
    Ensure-Firewall

    # --- 3. Binary updates. Applied now; the GUI apps pick them up at the
    #        next login/reboot (with Autologon that's automatic). ------------
    # DigiByte Core (wallet) binaries.
    try {
        $latest = Get-DigiByteLatestTag
        if ($latest -and (Test-Newer $latest $state.digibyte)) {
            Log "DigiByte update: $($state.digibyte) -> $latest" 'STEP'
            Stop-DigiByteGracefully
            Install-DigiByteBinaries (Resolve-DigiByteAsset $latest)
            $state.digibyte = $latest.TrimStart('v'); Write-State $state
        }
    } catch { $problems += "DigiByte update failed: $($_.Exception.Message)"; Log $problems[-1] 'ERROR' }

    # DigiAsset node exe (+ pool server exe on a pool box - same release).
    try {
        $latest = Get-DigiAssetLatestTag
        if ($latest -and (Test-Newer $latest $state.digiasset)) {
            Log "DigiAsset update: $($state.digiasset) -> $latest" 'STEP'
            Install-DigiAsset
            Update-PoolServer   # no-op unless DigiAssetPoolServer.exe is deployed
            $state.digiasset = $latest; Write-State $state
        }
    } catch { $problems += "DigiAsset update failed: $($_.Exception.Message)"; Log $problems[-1] 'ERROR' }

    # IPFS Desktop self-updates (Electron autoupdater) - just ensure it's installed.
    if (-not (Test-Path $IpfsDesktopExe)) {
        Log 'IPFS Desktop missing - reinstalling.' 'WARN'
        try { Install-IpfsDesktop | Out-Null } catch { $problems += "IPFS Desktop reinstall failed: $($_.Exception.Message)" }
    }

    # --- 4. Reinstall missing binaries (the GUI apps relaunch at next login) -
    if (-not (Test-Path (Get-Digibyted))) {
        Log 'DigiByte binaries missing - reinstalling.' 'WARN'
        try { Install-DigiByteBinaries (Resolve-DigiByteAsset "v$($state.digibyte)") } catch { $problems += "DigiByte reinstall failed: $($_.Exception.Message)" }
    }
    if (-not (Test-Path $NodeExe)) {
        Log 'DigiAssetWindows.exe missing - reinstalling.' 'WARN'
        try { Install-DigiAsset } catch { $problems += "DigiAsset reinstall failed: $($_.Exception.Message)" }
    }

    # --- 5. Health check (report only). These are GUI apps in the USER
    #        session, so they auto-start at logon - a SYSTEM task can't launch
    #        them into your desktop. A maintenance run before login will show
    #        them "not running", which is normal; Autologon makes boot -> login
    #        automatic. Real problems (missing binaries / failed updates) alert. -
    $prog = Get-DigiByteProgress
    if ($null -ne $prog) { Log ('DigiByte wallet: running, sync {0:P1}' -f $prog) 'OK' }
    elseif (Test-ProcRunning 'digibyte-qt') { Log 'DigiByte wallet running (RPC not up yet - still starting/syncing).' 'WARN' }
    else { Log 'DigiByte wallet not running - starts when you log in (see Autologon).' 'WARN' }

    if (Test-IpfsUp) { Log 'IPFS Desktop: API responding on 5001.' 'OK' }
    elseif (Test-ProcRunning 'IPFS Desktop') { Log 'IPFS Desktop running (API not up yet).' 'WARN' }
    else { Log 'IPFS Desktop not running - starts when you log in.' 'WARN' }

    if (Test-ProcRunning 'DigiAssetWindows') { Log 'DigiAsset node: running.' 'OK' }
    else { Log 'DigiAsset node not running - starts when you log in.' 'WARN' }

    # Pool server (only on a pool box).
    if (Test-Path $PoolExe) {
        if (Test-ProcRunning 'DigiAssetPoolServer') { Log 'Pool server: running.' 'OK' }
        else { Log 'Pool server installed but not running - start it (start-digistamp.ps1).' 'WARN' }
    }

    $reach = Test-PortOpen4001
    if ($reach -eq $false) { Log 'Port 4001 not reachable from the internet - forward TCP 4001 on your router.' 'WARN' }

    # --- 4. Escalate only if something is still broken ---------------------
    if ($problems.Count -gt 0) {
        Alert (($problems | Select-Object -Unique) -join "`n")
    } else {
        Log 'all components healthy.' 'OK'
    }
}

# ---------------------------------------------------------------------------
#  Entry point
# ---------------------------------------------------------------------------
# FIRST THING: make sure we are elevated. Install writes to C:\, installs
# services, and registers scheduled tasks + firewall rules - all need admin.
# Service mode runs as SYSTEM from the scheduled task, so it is already elevated
# (this guard only ever triggers for a manual, non-admin Install).
if ($Mode -eq 'Install' -and -not (Test-Admin)) {
    if ($PSCommandPath) {
        Write-Host 'This installer needs Administrator rights - approve the UAC prompt that appears...' -ForegroundColor Yellow
        # Relaunch elevated, forwarding every argument the user actually passed.
        $fwd = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', ('"{0}"' -f $PSCommandPath))
        foreach ($k in $PSBoundParameters.Keys) {
            $val = $PSBoundParameters[$k]
            if ($val -is [System.Management.Automation.SwitchParameter]) { if ($val.IsPresent) { $fwd += "-$k" } }
            else { $fwd += "-$k"; $fwd += ('"{0}"' -f $val) }
        }
        try { Start-Process powershell.exe -Verb RunAs -ArgumentList $fwd; exit 0 }
        catch { Write-Host 'Could not elevate. Right-click PowerShell, choose "Run as administrator", then run the one-line installer again.' -ForegroundColor Red; exit 1 }
    }
    Write-Host 'Administrator rights are required. Open PowerShell as administrator (right-click > Run as administrator) and run the one-line installer again.' -ForegroundColor Red
    exit 1
}

try {
    switch ($Mode) {
        'Service'    { Invoke-Service }
        'LaunchNode' { Invoke-LaunchNode }
        default      { Invoke-Install }
    }
    # The self-elevated install runs in its own window that would otherwise close
    # the instant the script ends - leaving no proof it finished. Hold it open for
    # a manual, interactive install (never for the headless SYSTEM service task).
    if ($Mode -eq 'Install' -and [Environment]::UserInteractive) {
        try { Read-Host "`nAll done - press Enter to close this window" | Out-Null } catch {}
    }
} catch {
    Log ("FATAL: $($_.Exception.Message)") 'ERROR'
    if ($Mode -eq 'Service') { Alert "maintenance run failed: $($_.Exception.Message)" }
    elseif ($Mode -eq 'Install') {
        Write-Host "`nSomething went wrong: $($_.Exception.Message)" -ForegroundColor Red
        Write-Host "See $LogFile" -ForegroundColor Gray
        if ([Environment]::UserInteractive) { try { Read-Host 'Press Enter to close' | Out-Null } catch {} }
    }
    exit 1
}

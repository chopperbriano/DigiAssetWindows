<#
.SYNOPSIS
    One script that installs AND maintains a full DigiAsset for Windows node on a
    fresh PC, then keeps it updated and healthy on every restart.

    Stack it sets up (all automatic, all start on boot):
      * DigiByte Core wallet (digibyted)  -> C:\DigiByte   (blockchain + RPC)
      * IPFS / kubo daemon                -> C:\DigiAssetWindows  (file storage)
      * DigiAsset for Windows node        -> C:\DigiAssetWindows  (the node + dashboard)

.DESCRIPTION
    TWO MODES in one file:

    -Mode Install  (default, run it yourself the first time)
        ALL QUESTIONS UP FRONT, then walk away. Two things are asked before any
        download starts, because only you can answer them:
          * payout address    - press ENTER to have one created in the DigiByte
                                wallet this sets up on your PC, or paste one you
                                already control. Pre-answer with -PayoutAddress.
          * wallet encryption - offered with the passphrase collected now and
                                applied near the end, once a wallet exists.
                                -EncryptWallet pre-answers yes, -NoEncryptPrompt
                                skips the offer.

        Nothing after that point interrupts you. Everything else is worked out
        rather than asked, because the answer is measurable or the default is
        right for anyone who is not running their own pool:
          * full vs lean    - chosen from free disk space. Override with -Lean.
          * pool URL        - the default is right unless you run your own pool.
          * firewall        - every listening program is pre-authorized BEFORE
                              it first starts, so Windows never raises its
                              "allow this app?" alert.

        Downloads and installs DigiByte Core (pinned to 9.26.5 by
        -DigiByteVersion), plus the CURRENT latest IPFS Desktop and DigiAsset
        for Windows - neither of those is pinned, both track their newest
        GitHub release. IPFS Desktop bundles its own kubo, so kubo is never
        downloaded separately. Then writes every config file, opens the local
        firewall, registers all the boot tasks, tests internet reachability,
        and installs itself as a maintenance task.

    -Mode Service  (runs itself automatically at every boot, as SYSTEM)
        Non-interactive. Checks GitHub / IPFS for newer versions of ALL THREE
        components and updates them (verified downloads, graceful restarts),
        then health-checks the whole stack and AGGRESSIVELY self-heals
        (restart tasks, re-download corrupt files, re-open firewall). Only
        pops a Windows alert if healing fails. Everything is logged to
        C:\DigiAssetWindows\logs.

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
    [string]$DigiAssetDir   = 'C:\DigiAssetWindows',
    [string]$PayoutAddress  = '',
    [string]$PoolServer     = 'https://pool.digistamp.co',
    # Pinned baseline versions used for a FIRST install. Service mode then
    # tracks the latest releases and updates past these. If a pinned version
    # isn't published yet, the installer falls back to the current latest.
    [string]$DigiByteVersion = '9.26.5',
    # Fast-sync snapshot manifest (snapshot.json) URL. If set, a FRESH install
    # downloads + verifies + extracts a pre-synced DigiByte blockchain + chain.db
    # so it skips the ~week-long sync. Overrides $DefaultSnapshotUrl below.
    [string]$SnapshotUrl = '',
    # By default the GUI apps (DigiByte wallet, IPFS Desktop, node dashboard)
    # auto-start when you log in. Pass -NoStartOnLogon to install them but NOT
    # register the logon auto-start (you'd start them by hand).
    [switch]$NoStartOnLogon,
    # -Lean: build a leaner DigiByte node that skips the OPTIONAL service indexes
    # (coinstatsindex, block/bloom filters, digidollar stats) to save disk + CPU.
    # Default (omit) = full public service node. Interactive install also offers this.
    [switch]$Lean,
    # -NoUpnp: skip the automatic router port-forward (UPnP) attempt.
    [switch]$NoUpnp,
    # -NoEncryptPrompt: do not offer wallet encryption at all during setup.
    [switch]$NoEncryptPrompt,
    # -EncryptWallet: skip the yes/no and go straight to asking for a passphrase.
    # An interactive install already offers encryption up front (see
    # Get-InstallAnswers), so this only pre-answers that question.
    [switch]$EncryptWallet
)

$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

# ---------------------------------------------------------------------------
#  Constants
# ---------------------------------------------------------------------------
$SCRIPT_VERSION = '2.25.0'
$Repo           = 'chopperbriano/DigiAssetWindows'
$RawScriptUrl   = "https://raw.githubusercontent.com/$Repo/master/setup-digiasset.ps1"
# Fast-sync snapshot manifest (snapshot.json on your Cloudflare R2). Set this to
# your public URL to turn fast-sync ON for everyone; leave '' to sync normally.
# The -SnapshotUrl parameter overrides it. Until the snapshot files are uploaded,
# a fetch just fails and the installer syncs normally (safe).
$DefaultSnapshotUrl = 'https://pub-bd3f441e6b464d499ba583016accfa01.r2.dev/snapshot.json'

$DgbData        = Join-Path $DigiByteDir  'Data'          # blockchain (blocks/chainstate/wallets)
# DigiByte ships a win64 NSIS installer (not a zip). After a silent install the
# daemon lands at <DigiByteDir>\daemon\digibyted.exe; we discover the real path
# at install time and remember it here (see Get-Digibyted).
$DgbExeMarker   = Join-Path $DigiAssetDir 'state\digibyted-path.txt'
$DgbExeDefault  = Join-Path $DigiByteDir  'daemon\digibyted.exe'
$NodeExe        = Join-Path $DigiAssetDir 'DigiAssetWindows.exe'
$PoolExe        = Join-Path $DigiAssetDir 'DigiAssetPoolServer.exe'  # present only on a pool box
# Legacy headless-kubo paths. Nothing installs or runs these any more (IPFS
# Desktop bundles its own kubo + repo), but the names are kept so the upgrade
# cleanup below can find what an older install left on disk.
$LegacyIpfsRepo = Join-Path $DigiAssetDir 'ipfs-repo'
$NodeConfig     = Join-Path $DigiAssetDir 'config.cfg'
$DgbConf        = Join-Path $DigiByteDir  'digibyte.conf'  # conf lives in C:\DigiByte (NOT in Data)
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

# Node service level. $true = lean (skip optional service indexes). Set from the
# -Lean switch and/or the interactive prompt; read by Write-DigiByteConf.
$script:LeanNode = [bool]$Lean
# Wallet passphrase collected UP FRONT by Get-InstallAnswers and applied in step 3,
# once DigiByte Core exists and has a wallet to encrypt. Held as a SecureString and
# converted to plaintext only at the moment it is handed to encryptwallet - the
# install runs for 20-40 minutes between the question and its use. $null means the
# operator declined, or this is a non-interactive run.
$script:WalletPassphrase = $null

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

# Lock a secret-bearing config (RPC password, wallet passphrase, peer token) down
# to SYSTEM + Administrators only, so a standard local user can't read the
# credentials out of C:\DigiByte or C:\DigiAssetWindows. (B-INST7)
function Protect-SecretFile($path) {
    if (-not (Test-Path $path)) { return }
    try {
        # SIDs (not names) so this holds on non-English Windows:
        #   *S-1-5-18 = SYSTEM, *S-1-5-32-544 = Administrators.
        icacls $path /inheritance:r /grant:r '*S-1-5-18:(F)' '*S-1-5-32-544:(F)' 2>&1 | Out-Null
    } catch { Log "  (could not restrict permissions on $path)" 'WARN' }
}

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
# NOTE: Register-DaemonTask + Register-DigiByteTask were removed here. They created
# the SYSTEM-level headless DigiByte task ($TaskDigiByte), which this script now
# UNREGISTERS on upgrade in favour of the GUI wallet started by a logon task - so
# they were not merely uncalled, they built the thing the install tears down. The
# node/pool daemons use Register-GuardedLogonTask + the maintenance task instead.

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
    # Backstop: the programs are pre-authorized before they are first launched, but
    # re-assert it here so a repair run fixes an install whose rules were removed or
    # whose exe moved. Port rules alone never suppress the per-program popup.
    Add-IpfsFirewallRules
}

# Best-effort automatic router port-forward via UPnP (IGD). Opens 4001 TCP/UDP
# (IPFS/DigiAsset hosting - what the pool verifies) and 12024 TCP (DigiByte P2P).
# Many home routers support this; if UPnP is off/unsupported it just no-ops and
# the user forwards manually. DigiByte's own upnp=1 also maps 12024 on its own.
function Invoke-UpnpForward {
    $ip = Get-LocalIPv4
    if (-not $ip) { Log '  UPnP: could not determine this PC''s LAN IP - skipping.' 'WARN'; return $false }
    $maps = @(
        @{ port = 4001;  proto = 'TCP'; desc = 'DigiAsset-IPFS' },
        @{ port = 4001;  proto = 'UDP'; desc = 'DigiAsset-IPFS-QUIC' },
        @{ port = 12024; proto = 'TCP'; desc = 'DigiByte-P2P' }
    )
    $any = $false
    try {
        $nat = New-Object -ComObject HNetCfg.NATUPnP
        $col = $nat.StaticPortMappingCollection
        if ($null -eq $col) { Log '  UPnP: no UPnP router found (enable UPnP on the router, or forward manually).' 'WARN'; return $false }
        foreach ($m in $maps) {
            try {
                $col.Add($m.port, $m.proto, $m.port, $ip, $true, ('DigiStamp ' + $m.desc)) | Out-Null
                Log ("  UPnP: mapped {0} {1} -> {2}" -f $m.proto, $m.port, $ip) 'OK'
                $any = $true
            } catch {
                Log ("  UPnP: router refused {0} {1} (forward it manually)" -f $m.proto, $m.port) 'WARN'
            }
        }
    } catch {
        Log '  UPnP: not available on this network - forward ports manually (see summary).' 'WARN'
        return $false
    }
    return $any
}

# Pre-authorize an app in Windows Firewall so Windows does NOT pop the "Do you
# want to allow this app?" dialog the first time the app listens. Adding an
# allow rule for the program before it starts suppresses that prompt.
function Add-ProgramAllowRule($name, $exePath) {
    if (-not $exePath -or -not (Test-Path $exePath)) { return }
    try { $exePath = (Resolve-Path -LiteralPath $exePath).Path } catch {}
    $disp = "DigiStamp allow $name"
    $existing = Get-NetFirewallRule -DisplayName $disp -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($existing) {
        # An app upgrade moves the exe, and a rule still pointing at the OLD path
        # authorizes nothing - Windows prompts again for the new binary while the
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

# Pre-authorize IPFS so port 4001 never raises a "Windows Security Alert".
#
# The Open-Port 4001 rules do NOT prevent that popup: Windows prompts PER PROGRAM,
# not per port, so a listening binary with no program rule is prompted for even when
# the port is already open. It is the kubo daemon bundled INSIDE IPFS Desktop - not
# the Electron app - that binds 4001, and its path moves with every IPFS Desktop
# version, so find whatever ipfs.exe is actually shipped rather than hard-coding it.
#
# MUST run after the install (the exe has to exist) but BEFORE the first launch -
# once the daemon binds the port with no rule, the prompt has already appeared.
function Add-IpfsFirewallRules {
    $exe = Get-IpfsDesktopExe
    if (-not $exe) { Log '  (IPFS Desktop not found - firewall pre-authorization skipped)' 'WARN'; return }
    Add-ProgramAllowRule 'IPFS Desktop' $exe

    $root = Split-Path $exe -Parent
    $kubo = @()
    try { $kubo = @(Get-ChildItem -LiteralPath $root -Filter 'ipfs.exe' -Recurse -Force -ErrorAction SilentlyContinue | Select-Object -First 4) } catch {}
    if ($kubo.Count -eq 0) {
        Log '  (bundled kubo ipfs.exe not found under IPFS Desktop - port 4001 may prompt once)' 'WARN'
        return
    }
    $n = 0
    foreach ($k in $kubo) { $n++; Add-ProgramAllowRule "IPFS kubo daemon $n" $k.FullName }
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

# Same as Invoke-DgbRpc but aimed at a NAMED wallet. Wallet calls like getnewaddress
# are rejected on the base endpoint when more than one wallet is loaded, so anything
# wallet-specific has to name the wallet in the URL.
function Invoke-DgbWalletRpc([string]$wallet, [string]$method, [string]$paramsJson = '[]') {
    $cfg = Read-Conf $DgbConf
    $port = $RpcPort; if ($cfg['rpcport']) { try { $port = [int]$cfg['rpcport'] } catch {} }
    $b64 = [Convert]::ToBase64String([Text.Encoding]::ASCII.GetBytes("$($cfg['rpcuser']):$($cfg['rpcpassword'])"))
    $body = '{"jsonrpc":"1.0","id":"setup","method":"' + $method + '","params":' + $paramsJson + '}'
    $uri = "http://127.0.0.1:$port/wallet/" + [uri]::EscapeDataString($wallet)
    $r = Invoke-RestMethod -Uri $uri -Method Post -ContentType 'text/plain' `
            -Headers @{ Authorization = "Basic $b64" } -TimeoutSec 20 -Body $body
    return $r.result
}

# Settle on the DigiByte address the pool pays hosting earnings to.
#
# If the caller passed -PayoutAddress we use it. Otherwise we CREATE one in the
# DigiByte wallet this installer just set up on this PC. That wallet belongs to the
# user, runs on their machine, and only they hold its keys - so it is a correct
# payout destination, and it removes the one question a non-technical user could not
# answer during setup ("paste a DigiByte address"). Nobody has to already own a
# wallet, find an address, or risk mistyping one; the earnings simply arrive in the
# DigiByte wallet on their desktop.
#
# Needs DigiByte RPC up and a wallet loaded, so call it after Wait-ForDigiByteRpc +
# Ensure-DigiByteWallet and before Write-NodeConfig (which writes it to config.cfg).
function Resolve-PayoutAddress {
    if ($script:PayoutAddress -match '^(D|S|dgb1)[0-9A-Za-z]{6,90}$') {
        Log "  payout address (supplied): $script:PayoutAddress" 'OK'
        return
    }
    if ($script:PayoutAddress) {
        Log "  ignoring -PayoutAddress '$script:PayoutAddress' - not a valid DigiByte address." 'WARN'
        $script:PayoutAddress = ''
    }

    $wallet = ''
    try { $w = @(Invoke-DgbRpc 'listwallets'); if ($w.Count -gt 0) { $wallet = "$($w[0])" } } catch {}

    # The wallet can still be finishing its first load right after createwallet, so
    # give it a few tries rather than dropping the user into an unpaid install.
    for ($try = 1; $try -le 5; $try++) {
        try {
            $addr = if ($wallet) { Invoke-DgbWalletRpc $wallet 'getnewaddress' '["DigiAsset hosting payout"]' }
                    else         { Invoke-DgbRpc 'getnewaddress' '["DigiAsset hosting payout"]' }
            $addr = ("$addr").Trim()
            if ($addr -match '^(D|S|dgb1)[0-9A-Za-z]{6,90}$') {
                $script:PayoutAddress = $addr
                Log "  payout address created in your own DigiByte wallet: $addr" 'OK'
                Log '  earnings arrive in the DigiByte wallet on this PC - no address to paste, nothing to set up.'
                return
            }
        } catch {
            if ($try -eq 5) { Log "  could not create a payout address: $($_.Exception.Message)" 'WARN' }
        }
        Start-Sleep 3
    }

    # Non-fatal on purpose: hosting still works and still helps the network, and a
    # half-finished install is worse for the user than an unpaid one. The summary
    # calls this out, and re-running the installer fixes it.
    Log '  NO payout address could be created - this node will host but will NOT be paid.' 'WARN'
    Log '  the maintenance run retries this automatically; no action needed from you.' 'WARN'
}

# Maintenance self-heal for the case above: config.cfg has no payout address because
# DigiByte RPC was not answering when the installer needed it. Left alone the node
# would host forever without ever being paid, and nothing would tell the user why -
# so every maintenance run retries it once DigiByte is up. No-op when already set.
function Repair-PayoutAddress {
    if (-not (Test-Path $NodeConfig)) { return }
    $raw = ''
    try { $raw = Get-Content -LiteralPath $NodeConfig -Raw } catch { return }
    if ($raw -match '(?m)^\s*psp2payout\s*=\s*\S') { return }   # already has a real value

    Ensure-DigiByteWallet
    $script:PayoutAddress = ''
    Resolve-PayoutAddress
    if ($script:PayoutAddress -notmatch '^(D|S|dgb1)[0-9A-Za-z]{6,90}$') { return }

    # Replace the commented placeholder in place so the surrounding documentation
    # comments survive; append only if the key is absent entirely.
    $out = @(); $set2 = $false; $set0 = $false
    foreach ($ln in (Get-Content -LiteralPath $NodeConfig)) {
        if ($ln -match '^\s*#?\s*psp2payout\s*=') {
            if (-not $set2) { $out += "psp2payout=$script:PayoutAddress"; $set2 = $true }
            continue
        }
        if ($ln -match '^\s*#?\s*psp0payout\s*=') {
            if (-not $set0) { $out += "psp0payout=$script:PayoutAddress"; $set0 = $true }
            continue
        }
        $out += $ln
    }
    if (-not $set2) { $out += "psp2payout=$script:PayoutAddress" }
    if (-not $set0) { $out += "psp0payout=$script:PayoutAddress" }
    try {
        Set-Content -LiteralPath $NodeConfig -Value $out -Encoding ASCII
        Log "  self-heal: payout address set to $script:PayoutAddress (node registers with the pool on its next restart)." 'OK'
    } catch { Log "  could not write the payout address into config.cfg: $($_.Exception.Message)" 'WARN' }
}

# Ensure a DigiByte wallet exists + is loaded. Modern DigiByte Core (Bitcoin Core
# base) does NOT auto-create one, so a fresh node has no wallet - the node can't
# get a payout address and the user has nothing to receive into. Idempotent: if a
# wallet is already loaded it returns; otherwise it loads the default 'digiasset'
# wallet if present, else creates it. Needs DigiByte RPC to be responding.
function Ensure-DigiByteWallet {
    try { $loaded = @(Invoke-DgbRpc 'listwallets'); if ($loaded.Count -gt 0) { return } }
    catch { return }   # RPC not ready yet - a later cycle will handle it
    try { Invoke-DgbRpc 'loadwallet' '["digiasset"]' | Out-Null; Log '  loaded DigiByte wallet "digiasset".' 'OK'; return } catch {}
    try {
        Invoke-DgbRpc 'createwallet' '["digiasset"]' | Out-Null
        Log '  created a DigiByte wallet ("digiasset"). ENCRYPT it + back up wallet.dat (see the notes at the end).' 'OK'
    } catch { Log "  could not create a DigiByte wallet yet (will retry): $($_.Exception.Message)" 'WARN' }
}

# Ask everything the install needs BEFORE any of it starts.
#
# The install runs 20-40 minutes and is meant to be walk-away. Prompts scattered
# through it were worse than either extreme: the operator had to babysit the whole
# run in case a question arrived fifteen minutes in. So every decision that is
# genuinely the user's is collected here, in one block, and nothing after this point
# stops to ask anything.
#
# Only two things qualify. The payout address, because it is their money and they may
# already have a wallet - though ENTER accepts one created on this PC, which is right
# for most people. And wallet encryption, because a passphrase they forget destroys
# the earnings with no recovery, so it cannot be defaulted either way.
#
# Everything else stays automatic: the pool URL default is right for anyone not
# running their own pool, and full-vs-lean is decided by measuring free disk, which
# is a better answer than asking someone who has no way to judge.
function Get-InstallAnswers {
    if (-not [Environment]::UserInteractive) { return }   # scheduled + service runs never prompt

    Write-Host ''
    Write-Host '=== A few questions before we start ===' -ForegroundColor Cyan
    Write-Host 'After this the install runs on its own - nothing else will interrupt you.' -ForegroundColor Gray
    Write-Host ''

    # --- 1. Payout address (skipped when -PayoutAddress was passed) ----------
    if (-not $script:PayoutAddress) {
        Write-Host '1) Where should your hosting earnings be paid?' -ForegroundColor White
        Write-Host '   Press ENTER to create an address in the DigiByte wallet on this PC (recommended -' -ForegroundColor Gray
        Write-Host '   only you hold its keys). Or paste an address you already control (D..., S..., dgb1...).' -ForegroundColor Gray
        for ($t = 0; $t -lt 5; $t++) {
            $a = ("$(Read-Host '   Payout address (or press Enter)')").Trim()
            if (-not $a) {
                Write-Host '   OK - one will be created in your wallet on this PC.' -ForegroundColor Green
                break
            }
            if ($a -match '^(D|S|dgb1)[0-9A-Za-z]{6,90}$') {
                # Read it back: the format check cannot catch a truncated or transposed
                # paste, and a wrong address means the earnings go to a stranger.
                Write-Host "   Earnings will go to: $a" -ForegroundColor Cyan
                if ((Read-Host '   Is that exactly right? (Y/n)') -notmatch '^[Nn]') { $script:PayoutAddress = $a; break }
                Write-Host '   OK - paste it again.' -ForegroundColor Yellow
                continue
            }
            Write-Host '   That does not look like a DigiByte address. Try again, or press Enter to have one made.' -ForegroundColor Yellow
        }
    } else {
        Log "  payout address supplied on the command line: $script:PayoutAddress"
    }

    # --- 2. Wallet encryption -----------------------------------------------
    if ($NoEncryptPrompt) { return }
    Write-Host ''
    Write-Host '2) Encrypt the DigiByte wallet on this PC?' -ForegroundColor White
    Write-Host '   A passphrase is then needed to SPEND earnings, so someone with access to this PC' -ForegroundColor Gray
    Write-Host '   cannot drain it. RECEIVING payouts still works normally either way.' -ForegroundColor Gray
    Write-Host '   WRITE THE PASSPHRASE DOWN. If you lose it the coins are GONE - there is no reset,' -ForegroundColor Yellow
    Write-Host '   no recovery, and nobody can help you.' -ForegroundColor Yellow
    if (-not $EncryptWallet) {
        if ((Read-Host '   Encrypt the wallet? (y/N)') -notmatch '^[Yy]') {
            Write-Host '   Skipping. You can do it any time in DigiByte-Qt: Settings > Encrypt Wallet.' -ForegroundColor Gray
            return
        }
    }
    for ($i = 0; $i -lt 3; $i++) {
        $sec1 = Read-Host '   Enter a wallet passphrase' -AsSecureString
        $sec2 = Read-Host '   Re-enter to confirm'       -AsSecureString
        $b1 = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($sec1)
        $b2 = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($sec2)
        try {
            $p1 = [Runtime.InteropServices.Marshal]::PtrToStringBSTR($b1)
            $p2 = [Runtime.InteropServices.Marshal]::PtrToStringBSTR($b2)
            if ($p1 -ne $p2)      { Write-Host '   Passphrases do not match - try again.' -ForegroundColor Yellow; continue }
            if ($p1.Length -lt 8) { Write-Host '   Please use at least 8 characters.'     -ForegroundColor Yellow; continue }
            $script:WalletPassphrase = $sec1
            Write-Host '   Got it - the wallet is encrypted near the end of the install.' -ForegroundColor Green
            return
        } finally {
            [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($b1)
            [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($b2)
        }
    }
    Write-Host '   Could not set a passphrase - continuing WITHOUT encryption.' -ForegroundColor Yellow
    Write-Host '   Do it later in DigiByte-Qt: Settings > Encrypt Wallet.' -ForegroundColor Yellow
}

# Offer to encrypt the payout wallet during an interactive install. Encrypting
# protects the earnings on this box (a passphrase is required to SPEND; receiving
# still works with no passphrase). Skips silently when non-interactive, already
# encrypted, or the wallet/RPC isn't ready. The passphrase is never stored or
# logged. encryptwallet stops DigiByte, so we restart the wallet afterward.
function Protect-Wallet {
    # Never prompts. The passphrase was collected by Get-InstallAnswers before any
    # install work began; $null here means the operator declined or this run is
    # non-interactive. Applied at this point because encryptwallet needs a wallet,
    # which does not exist until DigiByte Core is installed and answering RPC.
    if (-not $script:WalletPassphrase) { return }
    Ensure-DigiByteWallet
    $wi = $null
    try { $wi = Invoke-DgbRpc 'getwalletinfo' } catch { return }   # no wallet/RPC yet
    if ($null -ne $wi -and $null -ne $wi.unlocked_until) { Log '  wallet is already encrypted - good.' 'OK'; return }

    $bstr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($script:WalletPassphrase)
    try {
        $p1 = [Runtime.InteropServices.Marshal]::PtrToStringBSTR($bstr)
        # JSON-escape backslash first, then double-quote, so odd passphrases survive.
        $esc = ($p1 -replace '\\', '\\\\') -replace '"', '\"'
        Invoke-DgbRpc 'encryptwallet' ('["' + $esc + '"]') | Out-Null
        Log '  wallet ENCRYPTED. DigiByte is restarting to apply the change.' 'OK'
        $p1 = $null; $esc = $null
        Start-Sleep -Seconds 5
        for ($w = 0; $w -lt 30 -and (Test-ProcRunning 'digibyte-qt'); $w++) { Start-Sleep -Seconds 1 }
        Start-DigiByteWallet | Out-Null
        Wait-ForDigiByteRpc 180 | Out-Null
    } catch {
        Log "  encryptwallet failed: $($_.Exception.Message). Encrypt later in DigiByte-Qt (Settings > Encrypt Wallet)." 'WARN'
    } finally {
        # Drop the plaintext copy and the SecureString the moment we are done with it,
        # rather than leaving either alive for the rest of the run.
        [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($bstr)
        $script:WalletPassphrase = $null
    }
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
    $proc = Start-Process -FilePath $inst -ArgumentList @('/S', "/D=$DigiByteDir") -Wait -PassThru
    if ($proc -and $proc.ExitCode -ne 0) {
        throw "DigiByte installer exited with code $($proc.ExitCode) - the install may be incomplete (locked files?)."
    }
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
    # Wait for BOTH the daemon and the GUI wallet to exit (either can be the one
    # running), then force-kill whichever is left - otherwise the binary swap below
    # hits a locked digibyte-qt.exe and silently produces a half-updated install.
    for ($i = 0; $i -lt 30 -and ((Test-ProcRunning 'digibyted') -or (Test-ProcRunning 'digibyte-qt')); $i++) { Start-Sleep -Seconds 2 }
    Get-Process digibyted, digibyte-qt -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
}

function Start-DigiByte {
    $dgb = Get-Digibyted
    if (-not (Test-Path $dgb)) { return $false }
    Add-ProgramAllowRule 'DigiByte (digibyted)' $dgb
    if (-not (Test-ProcRunning 'digibyted')) {
        Start-Process $dgb -ArgumentList "-datadir=`"$DgbData`" -conf=`"$DgbConf`"" -WindowStyle Hidden
    }
    return $true
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
        # If the Service-mode updater left the headless daemon running, stop it
        # first so the GUI can take the datadir without a lock conflict (they share
        # one datadir; only one may hold it).
        if (Test-ProcRunning 'digibyted') {
            Get-Process digibyted -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
            for ($w = 0; $w -lt 15 -and (Test-ProcRunning 'digibyted'); $w++) { Start-Sleep -Seconds 1 }
        }
        Start-Process $qt -ArgumentList "-datadir=$DgbData -conf=$DgbConf"   # neither path has spaces
    }
    return $true
}

# NOTE: the headless raw-kubo install path (Get-KuboLatestVersion,
# Resolve-KuboVersion, Install-Ipfs, Initialize-Ipfs, Start-Ipfs) was removed - it
# had no callers left after the switch to IPFS Desktop, which bundles kubo and
# runs it internally on :5001. It survived long enough to make the header docs
# claim a "pinned kubo version" that was never installed. The legacy cleanup it
# left behind IS still needed and lives in Invoke-Install: unregistering the old
# DigiStampIPFS task and clearing a stale machine-level IPFS_PATH.

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
# IPFS Desktop is a PER-USER install. The SYSTEM maintenance task's $env:LOCALAPPDATA
# points at SYSTEM's own profile, so $IpfsDesktopExe (built from it) can't see the
# user's copy. Search every user profile, and treat a running process as installed.
function Get-IpfsDesktopExe {
    if (Test-Path $IpfsDesktopExe) { return $IpfsDesktopExe }
    try {
        $hit = Get-ChildItem 'C:\Users\*\AppData\Local\Programs\IPFS Desktop\IPFS Desktop.exe' -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($hit) { return $hit.FullName }
    } catch {}
    return $null
}
function Test-IpfsDesktopInstalled {
    if (Test-ProcRunning 'IPFS Desktop') { return $true }
    return [bool](Get-IpfsDesktopExe)
}

function Get-DigiAssetLatestTag {
    try { return (Invoke-GitHubApi "https://api.github.com/repos/$Repo/releases/latest").tag_name } catch { return '' }
}
# Cheap sanity check that a downloaded file is a real Windows PE (starts with
# 'MZ') and a plausible size - not an HTML error page, an S3 404 body, or a
# truncated blob. (B-INST2)
function Test-PeImage($path) {
    try {
        if (-not (Test-Path $path)) { return $false }
        if ((Get-Item $path).Length -lt 65536) { return $false }
        $fs = [System.IO.File]::OpenRead($path)
        try { $b = New-Object byte[] 2; [void]$fs.Read($b, 0, 2) } finally { $fs.Dispose() }
        return ($b[0] -eq 0x4D -and $b[1] -eq 0x5A)   # 'M','Z'
    } catch { return $false }
}
# Fetch the release's SHA256SUMS and parse it into @{ filename = hash }.
# Returns $null when the release does not publish one (anything before win.133).
#
# Honest scope: the sums live on the same GitHub release as the binaries, so this
# does NOT defend against a compromised release - someone who can swap the exe can
# swap the sums too. What it does catch is the realistic failure: a truncated or
# corrupted download, a CDN serving a stale object, or the wrong file being attached
# to a release. Real protection against a hostile release needs Authenticode signing.
function Get-ReleaseChecksums {
    $url = "https://github.com/$Repo/releases/latest/download/SHA256SUMS"
    $tmp = Join-Path $Tmp 'SHA256SUMS'
    if (-not (Get-File $url $tmp)) { return $null }
    $map = @{}
    foreach ($line in (Get-Content -LiteralPath $tmp -ErrorAction SilentlyContinue)) {
        # "<64 hex>  <filename>" - the sha256sum format, with one or two spaces.
        if ($line -match '^\s*([0-9a-fA-F]{64})\s+\*?(\S.*?)\s*$') {
            $map[$Matches[2]] = $Matches[1].ToLower()
        }
    }
    if ($map.Count -eq 0) { return $null }
    return $map
}

# Verify one downloaded file against the checksum map.
# MISMATCH is fatal - that is the signal worth acting on. A MISSING entry only warns,
# so a rollback to a release published before SHA256SUMS existed still installs.
function Test-ReleaseChecksum($sums, $name, $path) {
    if (-not $sums) { return $true }
    if (-not $sums.ContainsKey($name)) {
        Log "  ($name is not listed in SHA256SUMS - integrity not verified)" 'WARN'
        return $true
    }
    $got = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLower()
    if ($got -ne $sums[$name]) {
        Log "  CHECKSUM MISMATCH on $name" 'ERROR'
        Log "    expected $($sums[$name])" 'ERROR'
        Log "    got      $got" 'ERROR'
        return $false
    }
    Log "  $name checksum OK" 'OK'
    return $true
}

function Install-DigiAsset {
    $sums = Get-ReleaseChecksums
    if (-not $sums) { Log '  (this release publishes no SHA256SUMS - falling back to a format check only)' 'WARN' }
    foreach ($f in 'DigiAssetWindows.exe','DigiAssetWindows-cli.exe') {
        $out = Join-Path $DigiAssetDir $f
        $tmp = "$out.new"
        $wasRunning = ($f -eq 'DigiAssetWindows.exe') -and (Test-ProcRunning 'DigiAssetWindows')
        # Download to a SIDE file first and validate it BEFORE touching the live
        # exe, so a truncated/failed download can never leave a corrupt binary in
        # place (the old exe keeps running). Only then stop + atomically swap. (B-INST2)
        if (-not (Get-File "https://github.com/$Repo/releases/latest/download/$f" $tmp)) {
            Remove-Item $tmp -Force -ErrorAction SilentlyContinue
            throw "could not download $f"
        }
        if (-not (Test-PeImage $tmp)) {
            Remove-Item $tmp -Force -ErrorAction SilentlyContinue
            throw "downloaded $f is not a valid Windows executable (corrupt or partial download) - keeping the existing binary"
        }
        # Checked BEFORE the live exe is touched, so a bad download can never replace
        # a working binary - the running one keeps going and the install stops here.
        if (-not (Test-ReleaseChecksum $sums $f $tmp)) {
            Remove-Item $tmp -Force -ErrorAction SilentlyContinue
            throw "$f failed its SHA256 check - refusing to install it. The existing binary is untouched. Re-run to download again; if it keeps failing, report it."
        }
        if ($wasRunning) { Get-Process DigiAssetWindows -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue; Start-Sleep 2 }
        if (Test-Path $out) { Copy-Item $out "$out.bak" -Force -ErrorAction SilentlyContinue }  # roll-back copy
        try { Move-Item $tmp $out -Force }
        catch { Start-Sleep 2; Move-Item $tmp $out -Force }   # exe may still be releasing its lock
        Log "  + $f" 'OK'
    }
    Install-WebAssets
}
# The node serves a local web console on http://localhost:8090 (live node
# dashboard + searchable RPC reference). Its static files live in a web/ folder
# next to the exe; they ship as web.zip in the release. Non-fatal: an older
# release without web.zip simply leaves the previous web/ (or none) in place.
function Install-WebAssets {
    $zip = Join-Path $env:TEMP 'digiasset-web.zip'
    if (-not (Get-File "https://github.com/$Repo/releases/latest/download/web.zip" $zip)) {
        Log '  (web console assets not in this release - skipping)' 'WARN'; return
    }
    try {
        $dest = Join-Path $DigiAssetDir 'web'
        if (Test-Path $dest) { Remove-Item $dest -Recurse -Force -ErrorAction SilentlyContinue }
        Expand-Archive -Path $zip -DestinationPath $DigiAssetDir -Force   # zip has a top-level web\ folder
        Log '  + web console (dashboard + RPC reference)' 'OK'
    } catch { Log "  web console extract failed: $($_.Exception.Message)" 'WARN' }
    finally { Remove-Item $zip -Force -ErrorAction SilentlyContinue }
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
    # -WindowStyle Normal forces a VISIBLE console. Without it the node inherits
    # the hidden show-state of the logon task's hidden PowerShell, so its live
    # dashboard runs but never appears on the desktop.
    if (-not (Test-ProcRunning 'DigiAssetWindows')) { Start-Process -FilePath $NodeExe -WorkingDirectory $DigiAssetDir -WindowStyle Normal }
    return $true
}

# Opens the local web console (live node status dashboard) in the default browser
# once it is actually serving. The node was started earlier in the install, but
# its web thread needs a few seconds to bind the socket, so we poll the port for
# readiness first and don't open a dead tab. Port comes from config.cfg's webport
# (default 8090). Never fatal - a failure here just means the user opens it by hand.
function Open-WebConsole {
    $port = 8090
    try {
        if (Test-Path $NodeConfig) {
            $m = Select-String -Path $NodeConfig -Pattern '^\s*webport\s*=\s*(\d+)' -ErrorAction SilentlyContinue | Select-Object -First 1
            if ($m) { $port = [int]$m.Matches[0].Groups[1].Value }
        }
    } catch {}
    $url = "http://localhost:$port"

    # Poll for the web server (up to ~20s) so the browser lands on a live page.
    $ready = $false
    for ($i = 0; $i -lt 40; $i++) {
        try {
            $c = New-Object Net.Sockets.TcpClient
            $iar = $c.BeginConnect('127.0.0.1', $port, $null, $null)
            if ($iar.AsyncWaitHandle.WaitOne(500) -and $c.Connected) { $ready = $true; $c.Close(); break }
            $c.Close()
        } catch {}
        Start-Sleep -Milliseconds 500
    }

    try {
        Start-Process $url
        if ($ready) { Log "  opened the node status page in your browser: $url" 'OK' }
        else        { Log "  opened $url - the web server is still starting, refresh if the page is not up yet." 'WARN' }
    } catch { Log "  could not auto-open the browser - open $url manually to see node status." 'WARN' }
}

# ---------------------------------------------------------------------------
#  Config writers
# ---------------------------------------------------------------------------
function Write-DigiByteConf {
    Ensure-Dir $DgbData
    Ensure-Dir $DigiByteDir   # digibyte.conf lives in C:\DigiByte (parent of Data)
    $cfg = Read-Conf $DgbConf
    $rpcUser = $cfg['rpcuser']; $rpcPass = $cfg['rpcpassword']

    # RAM-adaptive dbcache: 25% of physical RAM, capped at 8192 MB (the service-node
    # target), floor 512 - so it never OOMs a typical wallet + IPFS + node box.
    $ramMB = 4096; try { $ramMB = [int]((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1MB) } catch {}
    $dbcache = [Math]::Max(512, [Math]::Min(8192, [int]($ramMB * 0.25)))

    # Every setting this node needs to be a good public service node. Used to build
    # a fresh conf and to top up a pre-existing one (append-missing on re-run).
    # Base settings every node needs (required for DigiAsset + a good peer).
    # upnp/natpmp try to auto-forward the P2P port on the router.
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
    # OPTIONAL service indexes - only on a FULL (non-lean) node. Built during the
    # initial sync at no extra cost; they let this box serve light clients +
    # explorers. A -Lean node omits them to save disk + CPU.
    if (-not $script:LeanNode) {
        $required['coinstatsindex']       = '1'
        $required['blockfilterindex']     = 'basic'
        $required['peerblockfilters']     = '1'
        $required['peerbloomfilters']     = '1'
        $required['digidollarstatsindex'] = '1'
    }
    $addnodes = @('64.182.71.55:12024','64.182.71.56:12024')

    if (-not $rpcUser -or -not $rpcPass) {
        $rpcUser = 'digiasset'; $rpcPass = New-Password 32
        # Optional service-index block - full node only.
        $svcBlock = ''
        if (-not $script:LeanNode) {
            $svcBlock = @"

# --- Extra service indexes (FULL node) ---------------------------------------
# Serve light clients + explorers. Built during initial sync at no extra cost.
# A -Lean node omits these to save disk + CPU.
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
# DigiByte Core $levelLabel Node Configuration  (written by DigiAsset for Windows)
#   - Helps the network: inbound peers, tx relay, full historical blocks + indexes
#   - upnp/natpmp try to auto-forward P2P 12024 on your router.
#   - Do NOT expose RPC 14022 to the public internet.
# Target: DigiByte Core v9.26.x mainnet
###############################################################################

# --- Node mode ---------------------------------------------------------------
server=1
listen=1
discover=1
dnsseed=1
port=12024
deprecatedrpc=addresses
# Try to auto-open the P2P port on the router (UPnP / NAT-PMP).
upnp=1
natpmp=1

# --- Full archival node ------------------------------------------------------
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
# Wallet ENABLED (disablewallet=0): the DigiAsset node needs a payout address and
# the installer offers to encrypt a wallet. Set to 1 only if you will NOT
# receive payouts on this box.
disablewallet=0

# --- DigiDollar --------------------------------------------------------------
digidollar=1
$svcBlock
# --- RPC (LOCAL ONLY - never port-forward 14022) -----------------------------
# The DigiAsset node + pool authenticate with these credentials (also copied into
# config.cfg / pool.cfg). Keep them in sync across all three files.
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

function Write-NodeConfig($rpc) {
    Ensure-Dir $DigiAssetDir
    if (Test-Path $NodeConfig) {
        # Repair in place - preserve the file (and its comments), but actually apply
        # a changed payout address. Re-running the installer with a new address MUST
        # update psp1payout (what the pool pays) + psp0payout; the old code only
        # appended missing keys, so a re-run silently kept paying the old address.
        $existing = Get-Content $NodeConfig
        $changed = $false
        if ($PayoutAddress) {
            # foreach STATEMENT (same scope) so $changed actually updates - a
            # ForEach-Object block would set it in a child scope only.
            $rebuilt = @()
            foreach ($ln in $existing) {
                if ($ln -match '^\s*psp2payout\s*=(.*)$') {
                    if ($Matches[1].Trim() -ne $PayoutAddress) { $ln = "psp2payout=$PayoutAddress"; $changed = $true }
                } elseif ($ln -match '^\s*psp1payout\s*=(.*)$') {
                    if ($Matches[1].Trim() -ne $PayoutAddress) { $ln = "psp1payout=$PayoutAddress"; $changed = $true }
                } elseif ($ln -match '^\s*psp0payout\s*=(.*)$') {
                    if ($Matches[1].Trim() -ne $PayoutAddress) { $ln = "psp0payout=$PayoutAddress"; $changed = $true }
                }
                $rebuilt += $ln
            }
            $existing = $rebuilt
        }
        # Top up any missing keys an older/partial config lacks.
        $add = @()
        if ($PayoutAddress -and -not ($existing -match '^\s*psp0payout\s*=')) { $add += @('psp0subscribe=1',"psp0payout=$PayoutAddress") }
        # Only top up the pool payout for a config that has NEITHER psp1 nor psp2
        # payout (a broken/partial config) - and add psp2 (DigiStamp), not the
        # deprecated psp1. Crucially do NOT re-add psp1subscribe=1: a fresh install
        # writes psp1subscribe=0 with no psp1payout, and a bare `-not psp1payout`
        # top-up would re-enable psp1 on every re-run -> double-registration.
        if ($PayoutAddress -and -not ($existing -match '^\s*psp2payout\s*=') -and -not ($existing -match '^\s*psp1payout\s*=')) { $add += @('psp2subscribe=1',"psp2payout=$PayoutAddress",'psp1subscribe=0') }
        if (-not ($existing -match '^\s*verifydatabasewrite\s*=')) { $add += 'verifydatabasewrite=0' }   # fast path
        # DigiDollar indexing (win.124+). Only added when the key is ABSENT, so an
        # operator who deliberately set trackdigidollar=0 is never overridden on a
        # re-run. The node itself also persists this key on first start; writing it
        # here makes the intent explicit and documented in the file.
        if (-not ($existing -match '^\s*trackdigidollar\s*=')) { $add += 'trackdigidollar=1' }
        if ($changed -or $add.Count -gt 0) {
            $out = @($existing)
            if ($add.Count -gt 0) {
                # Newline guard: never concatenate onto a hand-edited last line.
                if ($out.Count -gt 0 -and "$($out[-1])".Trim() -ne '') { $out += '' }
                $out += $add
            }
            Set-Content -Path $NodeConfig -Value $out -Encoding ASCII
            if ($changed)        { Log "  config.cfg: payout address updated to $PayoutAddress (node will re-register with the pool)." 'OK' }
            if ($add.Count -gt 0) { Log '  config.cfg: added missing psp payout / fast-path settings.' 'OK' }
        } else {
            Log '  config.cfg already up to date - leaving it untouched.'
        }
        return
    }
    # Resolve-PayoutAddress is best-effort, so the address CAN still be empty here.
    # Write a commented placeholder rather than a bare "psp2payout=" - an empty value
    # is a malformed setting the node would have to interpret, while a comment leaves
    # config.cfg valid and the maintenance run fills it in later.
    $payout2Line = if ($PayoutAddress) { "psp2payout=$PayoutAddress" }
                   else { '#psp2payout=      # NOT SET yet - this node hosts but is NOT paid until it is' }
    $payout0Line = if ($PayoutAddress) { "psp0payout=$PayoutAddress" } else { '#psp0payout=' }

    # NOTE: the comment lines below are written INTO config.cfg (the node's config
    # parser keeps lines starting with # and preserves them on write-back). Keep
    # them ASCII and apostrophe-free to stay valid in this single-quoted array.
    $lines = @(
        '# =============================================================================',
        '# DigiAsset for Windows - node configuration (config.cfg)',
        '#   Written by setup-digiasset.ps1. Safe to edit by hand; lines starting with',
        '#   # are comments. Restart the node (DigiAssetWindows.exe) after any change.',
        '# =============================================================================',
        '',
        '# --- DigiByte Core RPC (how this node reads the blockchain) -------------------',
        '#   The node talks to DigiByte Core on THIS machine with these credentials.',
        '#   They MUST match rpcuser / rpcpassword / rpcport in C:\DigiByte\digibyte.conf.',
        '#   Local only - never port-forward 14022 to the internet.',
        "rpcbind=127.0.0.1",
        "rpcport=$RpcPort",
        "rpcuser=$($rpc.user)",
        "rpcpassword=$($rpc.pass)",
        '',
        '# --- IPFS (where DigiAsset files are stored) ----------------------------------',
        '#   Local IPFS/Kubo HTTP API the node uses to pin and serve asset content.',
        'ipfspath=http://localhost:5001/api/v0/',
        '',
        '# --- The pool this node joins (psp2 = DigiStamp Pool) ------------------------',
        '#   psp2server = the pool your node registers with and hosts files for.',
        '#   IMPORTANT: on a REMOTE node this must be the pool PUBLIC https address',
        '#   (e.g. https://pool.digistamp.co). Do NOT use http://127.0.0.1:14028 - that',
        '#   only works ON the pool server itself; on a node it shows "Pool unreachable".',
        "psp2server=$PoolServer",
        'psp2subscribe=1',
        '#   psp2payout = the DGB address the pool pays your hosting earnings to.',
        $payout2Line,
        '#   psp2secret is auto-generated on first run (this node identity). Never copy',
        '#   it from another node - each node must have its own unique secret.',
        '#   Pool 1 (legacy MCTrivia PSP) is deprecated; kept OFF so this node does not',
        '#   double-register on the same server (psp1 and psp2 share pool.digistamp.co).',
        'psp1subscribe=0',
        '',
        '# --- Local pool (psp0) - your own private pin list ---------------------------',
        '#   Pool 0 is your own local pin list; pool 2 (above) is the pool you join.',
        'psp0subscribe=1',
        $payout0Line,
        '',
        '# --- Sync / storage tuning ---------------------------------------------------',
        '#   verifydatabasewrite=0 -> fast initial sync (relaxed durability only during',
        '#     catch-up; safe because chain.db can be rebuilt; durable once synced).',
        '#   storenonassetutxo=0   -> smaller, faster database (recommended).',
        '#   pruneage=5760         -> keep ~1 day of prunable history.',
        '#   bootstrapchainstate=1 -> fast-start chain.db over IPFS if it is missing.',
        '#   pipelinesync=0        -> experimental sync speedup, off by default.',
        'verifydatabasewrite=0',
        'storenonassetutxo=0',
        'pruneage=5760',
        'bootstrapchainstate=1',
        'pipelinesync=0',
        '',
        '# --- DigiDollar ---------------------------------------------------------------',
        '#   trackdigidollar=1 indexes DigiDollar balances, collateral vaults and the',
        '#     oracle DGB/USD price, and powers getdigidollarinfo / digidollarstats plus',
        '#     the digidollar fields on getaddressholdings and getwalletbalances.',
        '#   DigiDollar activated at block 23,869,440. The FIRST start after enabling it',
        '#     on an already-synced node rewinds to that height and re-scans forward,',
        '#     which takes a while - that is expected, not a fault.',
        '#   Set to 0 to skip all of it. Turning it off and back on forces a fresh',
        '#     rewind: a gap in DigiDollar history cannot be filled in by later blocks.',
        'trackdigidollar=1'
    )
    Set-Content -Path $NodeConfig -Value $lines -Encoding ASCII
    Log "  + config.cfg (documented; pool=$PoolServer, payout=$PayoutAddress)" 'OK'
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
        # Strip a UTF-8 BOM whether it decoded as U+FEFF or as mojibake bytes (ï»¿).
        $txt = $txt.TrimStart([char]0xFEFF, [char]0xEF, [char]0xBB, [char]0xBF)
        $m = $txt | ConvertFrom-Json
    } catch { Log '  snapshot manifest unreachable/invalid - syncing normally.' 'WARN'; return }
    if (-not $m -or -not $m.baseUrl) { Log '  snapshot manifest has no baseUrl - syncing normally.' 'WARN'; return }
    # Defensive: an older/corrupt manifest could carry digibyte/chaindb as a STRING
    # (a stringified JSON blob, sometimes BOM-prefixed) instead of a nested object -
    # re-parse so a bad manifest doesn't break fast-sync.
    foreach ($k in 'digibyte','chaindb') {
        if ($m.$k -is [string]) {
            try { $m.$k = (($m.$k).TrimStart([char]0xFEFF, [char]0xEF, [char]0xBB, [char]0xBF) | ConvertFrom-Json) } catch {}
        }
    }
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
    # From here the install runs start to finish with NO questions. Everything that
    # used to be a prompt is either derived automatically (payout address, full vs
    # lean) or has a default that is right for essentially everyone (pool URL).
    # Advanced users override with -PayoutAddress / -PoolServer / -Lean.

    # 0. Payout address ------------------------------------------------------
    # NOT resolved here: the address is created in the user's own DigiByte wallet,
    # which does not exist until step 1 installs and starts DigiByte Core. See
    # Resolve-PayoutAddress, called in step 3 just before config.cfg is written.
    $treasury = Get-TreasuryInfo   # pool's published treasury address + balance (may be $null if unreachable)
    Write-Host "`n--- Your hosting earnings ---" -ForegroundColor Cyan
    Write-Host 'Earnings are paid into the DigiByte wallet this installer sets up on THIS PC.' -ForegroundColor White
    Write-Host 'You do not need an address, an exchange account, or an existing wallet.' -ForegroundColor White
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

    # Ask everything now, before a single byte is downloaded, so the rest of the
    # install is genuinely walk-away.
    Get-InstallAnswers

    # 0b. Which pool to join --------------------------------------------------
    # No prompt: the default is correct for everyone except someone running their
    # own pool, and that person passes -PoolServer. Asking a non-technical user to
    # judge a URL gains nothing and is one more thing to get wrong.
    if ($PoolServer -match '127\.0\.0\.1|localhost') {
        Log '  pool URL is a localhost address - only correct ON the pool server itself.' 'WARN'
    }
    Log "  this node will join: $PoolServer"

    # --- Prerequisites ------------------------------------------------------
    # Internet: everything below downloads from GitHub / IPFS. Fail fast + clearly
    # instead of a cryptic "could not download" halfway through.
    Log 'Checking internet connection...' 'STEP'
    try { Invoke-WebRequest 'https://github.com' -UseBasicParsing -Method Head -TimeoutSec 15 | Out-Null; Log '  internet OK.' 'OK' }
    catch { throw "Can't reach the internet (couldn't contact github.com). Connect to the internet and re-run this installer - re-running is safe and resumes where it left off." }

    # Disk space: a full archival node + fast-sync snapshot is tens of GB. Warn
    # BEFORE downloading so a small disk doesn't fill up mid-sync with weird errors.
    # Also decides FULL vs LEAN, which used to be a prompt. Free space is exactly what
    # that choice depends on, so measuring it answers the question better than asking
    # a user who has no way to judge. -Lean still forces lean explicitly.
    # Measure first, decide after - so a failure to READ the disk is shrugged off
    # while a genuine "not enough space" still stops the install.
    $freeGB = $null; $driveName = ''
    try {
        $drive = New-Object System.IO.DriveInfo((Split-Path $DigiByteDir -Qualifier) + '\')
        $freeGB = [math]::Round($drive.AvailableFreeSpace / 1GB, 1)
        $driveName = $drive.Name
    } catch { Log '  (could not check free disk space - continuing)' 'WARN' }

    if ($null -ne $freeGB) {
        if ($freeGB -lt 25) {
            throw "Not enough disk space: $freeGB GB free on drive $driveName, and this needs at least 25 GB to install and sync. Free up space (or point -DigiByteDir at a bigger drive) and re-run."
        }
        if ($freeGB -lt 70) {
            $script:LeanNode = $true
            Log "  disk space: $freeGB GB free on $driveName - installing a LEAN node automatically." 'WARN'
            Log '  lean skips the optional service indexes to fit. It still hosts DigiAssets and is still paid.'
        } else {
            Log "  disk space: $freeGB GB free on $driveName - OK." 'OK'
        }
    }

    if ($script:LeanNode) { Log '  node type: LEAN (optional service indexes skipped).' }
    else { Log '  node type: FULL public service node - also serves DigiByte light clients + explorers.' 'OK' }

    Log 'Checking prerequisites (Visual C++ x64 runtime the node needs)...' 'STEP'
    Ensure-VCRuntime

    # 1. DigiByte (GUI wallet) -----------------------------------------------
    Step 1 "Installing DigiByte Core $DigiByteVersion (wallet GUI)..."
    Install-DigiByteBinaries (Resolve-DigiByteAsset "v$DigiByteVersion")
    $rpc = Write-DigiByteConf
    Protect-SecretFile $DgbConf   # digibyte.conf holds the RPC password (B-INST7)
    Restore-Snapshot   # fast-sync: extract pre-synced blockchain + chain.db before first launch (fresh install only)
    if (Get-ScheduledTask -TaskName $TaskDigiByte -ErrorAction SilentlyContinue) { Unregister-ScheduledTask -TaskName $TaskDigiByte -Confirm:$false }  # drop legacy headless task
    if (-not $NoStartOnLogon) { Register-GuardedLogonTask $TaskWallet (Get-DigiByteQt) $DigiByteDir 'digibyte-qt' "-datadir=$DgbData -conf=$DgbConf" }
    Start-DigiByteWallet | Out-Null
    Log '  DigiByte wallet (GUI) running + opens at every logon. Blockchain syncs in the background.' 'OK'
    Log '  what this does: runs a FULL DigiByte node - validates + relays the blockchain for the whole network, not just your wallet.'

    # 2. IPFS Desktop (GUI) --------------------------------------------------
    Step 2 'Installing IPFS Desktop (GUI, tray icon)...'
    if (Get-ScheduledTask -TaskName $TaskIpfs -ErrorAction SilentlyContinue) { Unregister-ScheduledTask -TaskName $TaskIpfs -Confirm:$false }  # drop legacy headless kubo task
    # An install that predates IPFS Desktop set a MACHINE-level IPFS_PATH pointing
    # at our old ipfs-repo. kubo honours that env var, and IPFS Desktop runs kubo -
    # so leaving it set silently points Desktop at the stale repo instead of its
    # own. Clear it, but ONLY when it still points at our legacy folder, so an
    # operator who deliberately set IPFS_PATH elsewhere is left alone.
    try {
        $staleIpfsPath = [Environment]::GetEnvironmentVariable('IPFS_PATH', 'Machine')
        if ($staleIpfsPath -and ($staleIpfsPath.TrimEnd('\') -ieq $LegacyIpfsRepo.TrimEnd('\'))) {
            [Environment]::SetEnvironmentVariable('IPFS_PATH', $null, 'Machine')
            $env:IPFS_PATH = $null
            Log '  cleared a stale machine-level IPFS_PATH left by the old headless kubo install.' 'OK'
        }
    } catch { Log "  (could not check IPFS_PATH: $($_.Exception.Message))" 'WARN' }
    $ipfsVer = Install-IpfsDesktop
    # Pre-authorize BEFORE the first launch. Step 4's Ensure-Firewall runs too late:
    # by then kubo has already bound 4001 and Windows has already shown its prompt.
    Add-IpfsFirewallRules
    Start-IpfsDesktop | Out-Null
    Log '  IPFS Desktop running (tray icon) + auto-starts at logon.' 'OK'
    Log '  what this does: IPFS stores the DigiAsset files; your node PINS them so they stay online for everyone.'

    # 3. DigiAsset node ------------------------------------------------------
    Step 3 'Installing DigiAsset for Windows (latest release)...'
    Install-DigiAsset
    # The node depends on IPFS + DigiByte RPC - wait for them before launching so
    # it doesn't FATAL on an IPFS timeout. (The logon task, registered in step 5,
    # does the same wait every login.) Registered/launched after step 5 copies
    # the script the launch task runs.
    #
    # This wait now also has to happen BEFORE config.cfg is written: the payout
    # address is created in the local DigiByte wallet, which needs RPC answering.
    Log '  waiting for IPFS + DigiByte to be ready before the node starts...'
    Wait-ForIpfs 300 | Out-Null
    Wait-ForDigiByteRpc 300 | Out-Null
    Ensure-DigiByteWallet      # a fresh DigiByte Core has no wallet until one is created
    Resolve-PayoutAddress      # creates the payout address in that wallet (no prompt)
    Write-NodeConfig $rpc
    Protect-SecretFile $NodeConfig   # config.cfg mirrors the RPC password (B-INST7)
    Protect-Wallet             # applies the passphrase collected up front; may restart the wallet + re-wait for RPC
    Start-Node | Out-Null
    Log '  DigiAsset node dashboard started.' 'OK'

    # 4. Firewall + automatic router forward ---------------------------------
    Step 4 'Opening the local firewall + trying to auto-forward on your router...'
    Ensure-Firewall
    Log '  local firewall now allows inbound 4001/TCP + 4001/UDP (DigiAsset/IPFS) and 12024/TCP (DigiByte).' 'OK'
    if (-not $NoUpnp) {
        Log '  attempting automatic router port-forward (UPnP)...' 'STEP'
        $upnpAny = Invoke-UpnpForward
        if ($upnpAny) { Log '  UPnP forward attempted - the port 4001 reachability test below confirms it.' 'OK' }
        else { Log '  UPnP could not map the ports - forward 4001 (+ 12024) on your router manually (see summary).' 'WARN' }
    } else {
        Log '  You must forward these on your home router - see the summary below.' 'WARN'
    }
    Log '  why this matters: without an open port 4001, the pool cannot verify you, so you are NOT paid and cannot host for others.'

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

    # Drop the companion tools next to the node so they are always handy. They
    # live in node/ in the repo but are staged FLAT into C:\DigiAssetWindows.
    foreach ($tool in 'monitor-node.ps1','stop-node.ps1','update-node.ps1','memwatch.ps1') {
        try { Get-File "https://raw.githubusercontent.com/$Repo/master/node/$tool" (Join-Path $DigiAssetDir $tool) 2 | Out-Null } catch {}
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
            "Your payout address : $(if ($PayoutAddress) { $PayoutAddress } else { '(not set yet - the maintenance run will create one)' })",
            'This address lives in the DigiByte wallet on THIS PC. Open the DigiByte',
            'wallet to see your earnings arrive - there is nothing else to set up.',
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
    Write-Host 'ABOUT EARNINGS:' -ForegroundColor Cyan
    if ($PayoutAddress) {
        Write-Host '  * Earnings go into the DigiByte wallet on THIS PC - open it to see them.' -ForegroundColor White
        Write-Host "    Your payout address: $PayoutAddress" -ForegroundColor Gray
    } else {
        Write-Host '  * No payout address yet (DigiByte was still starting). The maintenance' -ForegroundColor Yellow
        Write-Host '    task creates one automatically within the hour - nothing for you to do.' -ForegroundColor Yellow
    }
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
    Write-Host 'YOUR SETTINGS (all in one documented file; edit then restart the node):' -ForegroundColor Cyan
    Write-Host "  * Config file : $NodeConfig" -ForegroundColor Gray
    Write-Host '                  (open in Notepad - every setting has a # comment explaining it)' -ForegroundColor DarkGray
    Write-Host "  * Pool joined : $PoolServer   (key: psp2server - DigiStamp Pool)" -ForegroundColor Gray
    Write-Host "  * Payout addr : $(if ($PayoutAddress) { $PayoutAddress } else { '(pending - created automatically)' })   (key: psp2payout; change with -PayoutAddress)" -ForegroundColor Gray
    Write-Host '  * In the node window, the "PSP Pool" line reads "reachable" once it connects to' -ForegroundColor Gray
    Write-Host '    the pool. If it says "unreachable", double-check psp2server is the pool PUBLIC' -ForegroundColor Gray
    Write-Host '    https URL (not 127.0.0.1) and that the pool is online.' -ForegroundColor Gray
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
    Write-Host ' ONE MORE THING - BACK UP YOUR WALLET (IMPORTANT)' -ForegroundColor Yellow
    Write-Host '============================================================' -ForegroundColor Yellow
    Write-Host '  Your wallet is already created and your payout address is already in it.' -ForegroundColor White
    Write-Host '  Your earnings live in THIS wallet, on THIS PC - so protect it:' -ForegroundColor White
    Write-Host '   1. Open the DigiByte wallet (it is starting now / on your taskbar).' -ForegroundColor White
    Write-Host '   2. File > Backup Wallet... -> save wallet.dat somewhere safe/offline.' -ForegroundColor White
    Write-Host '      Without a backup, a dead disk or a Windows reinstall loses the earnings.' -ForegroundColor Red
    Write-Host '   3. OPTIONAL - Settings > Encrypt Wallet -> set a passphrase and WRITE IT DOWN.' -ForegroundColor White
    Write-Host '      Only do this if you will not lose the passphrase: lose it and the coins are' -ForegroundColor Red
    Write-Host '      GONE, and nobody can recover them. Receiving payouts works either way.' -ForegroundColor Red
    Write-Host '============================================================' -ForegroundColor Yellow

    # VERY LAST on screen (most visible) - the one manual action to get hosting + paid.
    Write-Host ''
    Write-Host '============================================================' -ForegroundColor Cyan
    Write-Host ' LAST STEP - FORWARD THESE PORTS ON YOUR ROUTER' -ForegroundColor Cyan
    Write-Host '============================================================' -ForegroundColor Cyan
    Write-Host '  Your Windows firewall is ALREADY open. The installer tried UPnP - if the' -ForegroundColor White
    Write-Host '  port 4001 test earlier did NOT say OPEN, add these on your router MANUALLY:' -ForegroundColor White
    Write-Host ''
    if ($localIp) {
        Write-Host '  Forward them TO this PC:  ' -ForegroundColor Gray -NoNewline
        Write-Host $localIp -ForegroundColor Green
    } else {
        Write-Host '  Find this PC IP with  ipconfig  (the IPv4 Address) and forward TO it.' -ForegroundColor Gray
    }
    Write-Host '     TCP 4001    DigiAsset / IPFS hosting  (REQUIRED - the pool verifies + pays you)' -ForegroundColor White
    Write-Host '     UDP 4001    DigiAsset / IPFS (QUIC)   (recommended - faster peer connections)' -ForegroundColor White
    Write-Host '     TCP 12024   DigiByte hosting          (recommended - serve DigiByte peers)' -ForegroundColor White
    Write-Host '  Keep 5001 / 14022 / 8090 PRIVATE - never forward them (local-only).' -ForegroundColor Red
    Write-Host '============================================================' -ForegroundColor Cyan

    # Durable proof of completion (the window may close; this file + log line stay).
    Log "Install completed successfully (script v$SCRIPT_VERSION)." 'OK'
    try { Set-Content -Path (Join-Path $LogDir 'INSTALL-COMPLETE.txt') -Value ("DigiAsset for Windows install completed - script v$SCRIPT_VERSION - " + (Get-Date).ToString('s')) -Encoding ascii } catch {}

    # Pop the live status page on top of the summary so the user immediately sees
    # their node coming up. Last action of the install - purely informational.
    Open-WebConsole
}

# ---------------------------------------------------------------------------
#  LAUNCH-NODE MODE  (run by the node's logon task: wait for deps, supervise)
# ---------------------------------------------------------------------------
function Invoke-LaunchNode {
    Ensure-Dir $LogDir
    Log "----- launch-node (script v$SCRIPT_VERSION) -----"
    Log "launch-node: exe=$NodeExe  workdir=$DigiAssetDir"
    if (-not (Test-Path $NodeExe)) { Log "launch-node: node exe NOT FOUND at $NodeExe - cannot start (re-run the installer or update-binaries.ps1)." 'ERROR'; return }

    # This task runs AS THE USER, so per-user paths + installs land in the right
    # profile. Ensure IPFS Desktop is actually installed (repairs the rare genuine
    # loss correctly, unlike the SYSTEM maintenance task) and started.
    if (-not (Test-IpfsDesktopInstalled)) {
        Log 'launch-node: IPFS Desktop not installed - installing now...' 'WARN'
        try { Install-IpfsDesktop | Out-Null } catch { Log "launch-node: IPFS Desktop install failed: $($_.Exception.Message)" 'WARN' }
    }
    # Again before launching, to catch a rule lost to an IPFS Desktop auto-update.
    # This task runs as the USER, not elevated, so New-NetFirewallRule may be denied -
    # it is best-effort here and the elevated installer/maintenance run is what
    # reliably repairs it. Never let a refused rule stop the node from starting.
    try { Add-IpfsFirewallRules } catch { Log "launch-node: firewall pre-authorization skipped: $($_.Exception.Message)" 'WARN' }
    Start-IpfsDesktop | Out-Null

    # Persistent supervisor: keep the node alive for the whole logon session. The
    # node's task has no execution-time limit, so we never permanently give up
    # (the old 8-try loop quit while a freshly-seeded wallet was still verifying
    # the chain and its RPC wasn't answering, and nothing restarted it after).
    # The node needs IPFS (:5001) + DigiByte RPC (:14022); a seeded wallet can take
    # a long time to VERIFY before RPC answers, so we wait patiently the first time,
    # then (re)start the node whenever it exits, logging PID + uptime each cycle.
    $firstTime = $true
    while ($true) {
        if (Test-ProcRunning 'DigiAssetWindows') { Start-Sleep -Seconds 60; continue }

        if ($firstTime) {
            Log 'launch-node: waiting for IPFS API (:5001) - IPFS Desktop can take a bit to boot...'
            if (-not (Wait-ForIpfs 600)) { Log 'launch-node: IPFS API not up after 10 min; starting node anyway (it retries internally).' 'WARN' }
            Log 'launch-node: waiting for DigiByte RPC (a seeded wallet may still be verifying the chain - this can take a while)...'
            if (-not (Wait-ForDigiByteRpc 1800)) { Log 'launch-node: DigiByte RPC not up after 30 min; starting node anyway.' 'WARN' }
            Ensure-DigiByteWallet   # create/load a wallet so the node has a payout address
            $firstTime = $false
        }

        $t0 = Get-Date
        Log 'launch-node: starting node...'
        Start-Node | Out-Null
        Start-Sleep -Seconds 8
        $proc = Get-Process DigiAssetWindows -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($proc) {
            Log "launch-node: node started (PID $($proc.Id))." 'OK'
            while (Test-ProcRunning 'DigiAssetWindows') { Start-Sleep -Seconds 30 }
            Log ("launch-node: node exited after {0}s - re-checking deps and restarting." -f [int]((Get-Date)-$t0).TotalSeconds) 'WARN'
        } else {
            Log ("launch-node: node did not stay up ({0}s after launch). Run it in a window to see the error: {1}" -f [int]((Get-Date)-$t0).TotalSeconds, $NodeExe) 'WARN'
        }

        # Backoff + re-verify deps before restarting so a crashing node can't tight-loop.
        Start-Sleep -Seconds 20
        Wait-ForIpfs 120 | Out-Null
        Wait-ForDigiByteRpc 300 | Out-Null
    }
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
            # Restart the headless daemon NOW so the node isn't left with no wallet/
            # RPC until the next logon (a SYSTEM task can't relaunch the GUI into the
            # user session). digibyted restores RPC immediately; the user-session
            # task swaps to the GUI wallet at next logon (Start-DigiByteWallet stops
            # this daemon first, so there's no datadir-lock conflict).
            Start-DigiByte | Out-Null
            Log "  DigiByte daemon restarted after update." 'OK'
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

    # IPFS Desktop self-updates (Electron autoupdater). It's a PER-USER install, so
    # a running process or a copy in ANY user profile counts as present. This SYSTEM
    # task must NOT reinstall from its own (SYSTEM) profile path - that churns a
    # useless reinstall every cycle. Genuine repair happens in the user's logon
    # launcher (Invoke-LaunchNode), which runs in the correct profile.
    if (Test-IpfsDesktopInstalled) { Log 'IPFS Desktop present.' 'OK' }
    else { Log 'IPFS Desktop not found in any user profile - will be restored at next logon.' 'WARN' }

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
    if ($null -ne $prog) { Log ('DigiByte wallet: running, sync {0:P1}' -f $prog) 'OK'; Ensure-DigiByteWallet; Repair-PayoutAddress }
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
    # The self-elevated install runs in its own window that would otherwise close the
    # instant the script ends - leaving no proof it finished. Hold it open long enough
    # to read the summary, but on a TIMER rather than a keypress: a successful install
    # should not need one last click from the user. Enter closes it early.
    if ($Mode -eq 'Install' -and [Environment]::UserInteractive) {
        try {
            Write-Host ''
            for ($s = 60; $s -gt 0; $s--) {
                Write-Host ("`r  All done. This window closes in {0,2}s (press Enter to close now)..." -f $s) -NoNewline -ForegroundColor Gray
                if ($Host.UI.RawUI.KeyAvailable) { break }
                Start-Sleep -Seconds 1
            }
            Write-Host ''
        } catch {
            # No interactive console (piped/redirected host): nothing to hold open.
        }
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

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
    [ValidateSet('Install','Service')]
    [string]$Mode           = 'Install',
    [string]$DigiByteDir    = 'C:\DigiByte',
    [string]$DigiAssetDir   = 'C:\DigiAsset',
    [string]$PayoutAddress  = '',
    [string]$PoolServer     = 'https://pool.digistamp.co',
    # Pinned baseline versions used for a FIRST install. Service mode then
    # tracks the latest releases and updates past these. If a pinned version
    # isn't published yet, the installer falls back to the current latest.
    [string]$DigiByteVersion = '9.26.4',
    [string]$KuboVersion     = '0.42.0'
)

$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

# ---------------------------------------------------------------------------
#  Constants
# ---------------------------------------------------------------------------
$SCRIPT_VERSION = '1.0.0'
$Repo           = 'chopperbriano/DigiAssetWindows'
$RawScriptUrl   = "https://raw.githubusercontent.com/$Repo/master/setup-digiasset.ps1"

$DgbData        = Join-Path $DigiByteDir  'data'          # blockchain + digibyte.conf
# DigiByte ships a win64 NSIS installer (not a zip). After a silent install the
# daemon lands at <DigiByteDir>\daemon\digibyted.exe; we discover the real path
# at install time and remember it here (see Get-Digibyted).
$DgbExeMarker   = Join-Path $DigiAssetDir 'state\digibyted-path.txt'
$DgbExeDefault  = Join-Path $DigiByteDir  'daemon\digibyted.exe'
$NodeExe        = Join-Path $DigiAssetDir 'DigiAssetWindows.exe'
$CliExe         = Join-Path $DigiAssetDir 'DigiAssetWindows-cli.exe'
$IpfsExe        = Join-Path $DigiAssetDir 'ipfs.exe'
$IpfsRepo       = Join-Path $DigiAssetDir 'ipfs-repo'
$NodeConfig     = Join-Path $DigiAssetDir 'config.cfg'
$DgbConf        = Join-Path $DgbData      'digibyte.conf'
$LogDir         = Join-Path $DigiAssetDir 'logs'
$LogFile        = Join-Path $LogDir       'setup.log'
$StateFile      = Join-Path $DigiAssetDir 'state\versions.json'
$InstalledScript= Join-Path $DigiAssetDir 'setup-digiasset.ps1'
$Tmp            = Join-Path $env:TEMP     'digiasset-setup'

$TaskDigiByte   = 'DigiStampDigiByte'
$TaskIpfs       = 'DigiStampIPFS'
$TaskNode       = 'DigiStampNode'
$TaskMaint      = 'DigiStampMaintenance'

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
function Register-GuardedLogonTask($name, $exe, $workdir, $procName) {
    $guard = "if (-not (Get-Process '$procName' -ErrorAction SilentlyContinue)) { Start-Process -FilePath '$exe' -WorkingDirectory '$workdir' }"
    $a = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument "-WindowStyle Hidden -ExecutionPolicy Bypass -Command `"$guard`""
    $t = New-ScheduledTaskTrigger -AtLogOn
    $u = [Security.Principal.WindowsIdentity]::GetCurrent().Name
    $p = New-ScheduledTaskPrincipal -UserId $u -LogonType Interactive -RunLevel Highest
    $s = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit ([TimeSpan]::Zero)
    Register-ScheduledTask -TaskName $name -Action $a -Trigger $t -Principal $p -Settings $s -Force | Out-Null
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

    # Optional SHA256 verification if the release ships a checksums file.
    try {
        $sumsUrl  = ($asset.url -replace '/[^/]+$', '/SHA256SUMS')
        $sumsFile = Join-Path $Tmp 'DGB_SHA256SUMS'
        if (Get-File $sumsUrl $sumsFile 1) {
            $want = (Get-Content $sumsFile | Where-Object { $_ -match [regex]::Escape($asset.name) } | Select-Object -First 1)
            if ($want) {
                $wantHash = ($want -split '\s+')[0].ToLower()
                $gotHash  = (Get-FileHash $inst -Algorithm SHA256).Hash.ToLower()
                if ($wantHash -ne $gotHash) { Remove-Item $inst -Force; throw 'DigiByte checksum mismatch - aborting.' }
                Log '  DigiByte checksum verified (SHA-256).' 'OK'
            }
        }
    } catch { Log "  (DigiByte checksum step skipped: $($_.Exception.Message))" 'WARN' }

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
    if (-not (Test-ProcRunning 'digibyted')) {
        Start-Process $dgb -ArgumentList "-datadir=`"$DgbData`"" -WindowStyle Hidden
    }
    return $true
}

function Register-DigiByteTask {
    $dgb = Get-Digibyted
    Register-DaemonTask $TaskDigiByte $dgb "-datadir=`"$DgbData`"" (Split-Path -Parent $dgb)
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
    if (-not (Test-ProcRunning 'ipfs')) { Start-Process -FilePath $IpfsExe -ArgumentList 'daemon --enable-gc' -WindowStyle Hidden }
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
function Start-Node {
    if (-not (Test-Path $NodeExe)) { return $false }
    if (-not (Test-ProcRunning 'DigiAssetWindows')) { Start-Process -FilePath $NodeExe -WorkingDirectory $DigiAssetDir }
    return $true
}

# ---------------------------------------------------------------------------
#  Config writers
# ---------------------------------------------------------------------------
function Write-DigiByteConf {
    Ensure-Dir $DgbData
    $cfg = Read-Conf $DgbConf
    $rpcUser = $cfg['rpcuser']; $rpcPass = $cfg['rpcpassword']
    if (-not $rpcUser -or -not $rpcPass) {
        $rpcUser = 'digiasset'; $rpcPass = New-Password 32
        $addnodes = @('191.81.59.115','175.45.182.173','45.76.235.153','24.74.186.115','24.101.88.154','8.214.25.169','47.75.38.245')
        $lines = @(
            "rpcuser=$rpcUser","rpcpassword=$rpcPass","rpcbind=127.0.0.1","rpcport=$RpcPort",
            "rpcallowip=127.0.0.1","whitelist=127.0.0.1","listen=1","server=1","txindex=1","deprecatedrpc=addresses"
        )
        $lines += ($addnodes | ForEach-Object { "addnode=$_" })
        Set-Content -Path $DgbConf -Value $lines -Encoding ASCII
        Log '  wrote digibyte.conf with fresh RPC credentials.'
    } else {
        Log '  digibyte.conf already has RPC credentials - leaving it.'
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
    Write-Host @"

This sets your PC up to HOST DigiAsset content and EARN DGB from the DigiStamp
pool. It installs and auto-starts everything, and keeps it updated for you:

  * DigiByte Core wallet   -> $DigiByteDir   (blockchain, runs in background)
  * IPFS (file storage)    -> $DigiAssetDir  (runs in background)
  * DigiAsset for Windows  -> $DigiAssetDir  (the node + live dashboard)

You forward ONE port (4001) on your home router - shown at the end.
DigiByte's first sync takes hours and runs in the background.
Nothing here spends your coins.

"@ -ForegroundColor Gray
    $go = Read-Host 'Press Enter to continue, or type N then Enter to cancel'
    if ($go -match '^[Nn]') { Write-Host 'Cancelled - nothing was changed.'; return }

    # 0. Payout address ------------------------------------------------------
    $treasury = Get-TreasuryInfo   # pool's published treasury address + balance (may be $null if unreachable)
    if (-not $PayoutAddress) {
        Write-Host "`n--- Your payout address ---" -ForegroundColor Cyan
        Write-Host 'The DigiByte address where the pool sends your hosting earnings.'
        Write-Host 'Use an address from a wallet YOU control (starts with D, S, or dgb1).'
        Write-Host ''
        Write-Host 'Please be realistic about earnings:' -ForegroundColor Yellow
        Write-Host '  * Payments are TINY - this is a tip jar for hosting, not a salary.'
        Write-Host '  * You are ONLY paid when the pool TREASURY has funds. The treasury is'
        Write-Host '    shared out among all verified nodes; when it is empty, nobody is paid'
        Write-Host '    that period. The pool never pays money it does not have.'
        Write-Host "  * See the live treasury balance + every payout at $PoolServer"
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

    # 1. DigiByte ------------------------------------------------------------
    Step 1 "Installing DigiByte Core $DigiByteVersion..."
    Install-DigiByteBinaries (Resolve-DigiByteAsset "v$DigiByteVersion")
    $rpc = Write-DigiByteConf
    Register-DigiByteTask
    Start-DigiByte | Out-Null
    Log '  DigiByte running + set to start on boot (syncing in the background).' 'OK'

    # 2. IPFS ----------------------------------------------------------------
    $kubo = Resolve-KuboVersion $KuboVersion
    Step 2 "Installing IPFS (kubo) $kubo..."
    Install-Ipfs $kubo
    Initialize-Ipfs
    Register-DaemonTask $TaskIpfs $IpfsExe 'daemon --enable-gc' $DigiAssetDir
    Start-Ipfs | Out-Null
    Log '  IPFS running + set to start on boot.' 'OK'

    # 3. DigiAsset node ------------------------------------------------------
    Step 3 'Installing DigiAsset for Windows (latest release)...'
    Install-DigiAsset
    Write-NodeConfig $rpc
    Register-GuardedLogonTask $TaskNode $NodeExe $DigiAssetDir 'DigiAssetWindows'
    Start-Node | Out-Null
    Log '  DigiAsset node started + opens at every logon.' 'OK'

    # 4. Firewall ------------------------------------------------------------
    Step 4 'Opening the local Windows firewall for hosting (inbound)...'
    Ensure-Firewall
    Log '  local firewall now allows inbound 4001/TCP + 4001/UDP (DigiAsset/IPFS) and 12024/TCP (DigiByte).' 'OK'
    Log '  You must ALSO forward these on your home router - see the summary below.' 'WARN'

    # 5. Maintenance task ----------------------------------------------------
    Step 5 'Installing the auto-update + self-heal maintenance task...'
    if ($PSCommandPath -and (Resolve-Path $PSCommandPath).Path -ne (Resolve-Path -LiteralPath $InstalledScript -ErrorAction SilentlyContinue).Path) {
        Copy-Item $PSCommandPath $InstalledScript -Force
    } elseif (-not (Test-Path $InstalledScript)) {
        Get-File $RawScriptUrl $InstalledScript 2 | Out-Null
    }
    # Drop the companion tools next to the node so they are always handy.
    foreach ($tool in 'monitor-node.ps1','stop-node.ps1') {
        try { Get-File "https://raw.githubusercontent.com/$Repo/master/$tool" (Join-Path $DigiAssetDir $tool) 2 | Out-Null } catch {}
    }
    # Record what we installed so Service mode knows the baseline.
    $state = Read-State
    $state.digibyte  = $DigiByteVersion
    $state.kubo      = $kubo
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
    Write-Host 'Everything is installed, auto-starting on boot, and self-updating.'
    Write-Host ''
    Write-Host 'HOSTING PORTS - your local Windows firewall is ALREADY open for these.' -ForegroundColor Cyan
    Write-Host 'To accept incoming connections you must ALSO forward them on your home router:'
    Write-Host '   TCP 4001    DigiAsset / IPFS hosting  (REQUIRED - the pool verifies + pays you)'
    Write-Host '   UDP 4001    DigiAsset / IPFS (QUIC)   (recommended - faster peer connections)'
    Write-Host '   TCP 12024   DigiByte hosting          (recommended - serve DigiByte peers)'
    Write-Host '   Keep 5001 / 14022 / 8090 PRIVATE - never forward them (they are local-only).'
    Write-Host ''
    Write-Host 'ABOUT EARNINGS:' -ForegroundColor Cyan
    Write-Host '  * Payments are TINY and only shared when the pool treasury has funds.'
    if ($treasury -and $treasury.donationAddress) {
        Write-Host "  * Pool treasury address: $($treasury.donationAddress)"
    }
    Write-Host "  * See what is in the treasury any time at $PoolServer"
    Write-Host "  * Saved for you: $treasuryNote"
    Write-Host ''
    Write-Host 'WHAT HAPPENS NOW:' -ForegroundColor Cyan
    Write-Host '  * DigiByte is syncing the blockchain in the background (hours the first time).'
    Write-Host '  * Once synced, the node registers with the pool automatically.'
    Write-Host "  * Updates + health checks run on every boot and every 6 hours."
    Write-Host ''
    Write-Host 'HANDY COMMANDS (Administrator PowerShell):' -ForegroundColor Cyan
    Write-Host "  * Check status : powershell -ExecutionPolicy Bypass -File $DigiAssetDir\monitor-node.ps1"
    Write-Host "  * Stop / remove: powershell -ExecutionPolicy Bypass -File $DigiAssetDir\stop-node.ps1"
    Write-Host "  * Logs         : $LogFile"
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

    # --- 2. Updates: DigiByte, IPFS, DigiAsset -----------------------------
    # DigiByte
    try {
        $latest = Get-DigiByteLatestTag
        if ($latest -and (Test-Newer $latest $state.digibyte)) {
            Log "DigiByte update: $($state.digibyte) -> $latest" 'STEP'
            Stop-DigiByteGracefully
            Install-DigiByteBinaries (Resolve-DigiByteAsset $latest)
            Register-DigiByteTask
            Start-DigiByte | Out-Null
            $state.digibyte = $latest.TrimStart('v'); Write-State $state
        }
    } catch { $problems += "DigiByte update failed: $($_.Exception.Message)"; Log $problems[-1] 'ERROR' }

    # IPFS
    try {
        $latest = Get-KuboLatestVersion
        if ($latest -and (Test-Newer $latest $state.kubo)) {
            Log "IPFS update: $($state.kubo) -> $latest" 'STEP'
            Install-Ipfs $latest
            Start-Ipfs | Out-Null
            $state.kubo = $latest; Write-State $state
        }
    } catch { $problems += "IPFS update failed: $($_.Exception.Message)"; Log $problems[-1] 'ERROR' }

    # DigiAsset node
    try {
        $latest = Get-DigiAssetLatestTag
        if ($latest -and (Test-Newer $latest $state.digiasset)) {
            Log "DigiAsset update: $($state.digiasset) -> $latest" 'STEP'
            Install-DigiAsset
            Start-Node | Out-Null
            $state.digiasset = $latest; Write-State $state
        }
    } catch { $problems += "DigiAsset update failed: $($_.Exception.Message)"; Log $problems[-1] 'ERROR' }

    # --- 3. Health + AGGRESSIVE self-heal ----------------------------------
    # Re-register any missing boot task, re-open firewall, re-download missing
    # binaries, restart anything that is down. Only alert if it stays broken.

    Ensure-Firewall
    foreach ($t in @(
        @{ n=$TaskDigiByte }, @{ n=$TaskIpfs }, @{ n=$TaskMaint }
    )) {
        if (-not (Get-ScheduledTask -TaskName $t.n -ErrorAction SilentlyContinue)) {
            Log "boot task '$($t.n)' missing - re-registering." 'WARN'
            switch ($t.n) {
                $TaskDigiByte { Register-DigiByteTask }
                $TaskIpfs     { Register-DaemonTask $TaskIpfs $IpfsExe 'daemon --enable-gc' $DigiAssetDir }
                $TaskMaint    { Register-MaintenanceTask }
            }
        }
    }

    # DigiByte
    if (-not (Test-Path (Get-Digibyted))) {
        Log 'digibyted.exe missing - reinstalling.' 'WARN'
        try { Install-DigiByteBinaries (Resolve-DigiByteAsset "v$($state.digibyte)"); Register-DigiByteTask } catch { $problems += "DigiByte reinstall failed: $($_.Exception.Message)" }
    }
    if (-not (Test-ProcRunning 'digibyted')) { Log 'digibyted not running - starting.' 'WARN'; Start-DigiByte | Out-Null; Start-Sleep 5 }
    if (-not (Test-ProcRunning 'digibyted')) { $problems += 'DigiByte will not stay running (check the log / datadir).' }

    # IPFS
    if (-not (Test-Path $IpfsExe)) {
        Log 'ipfs.exe missing - reinstalling.' 'WARN'
        try { Install-Ipfs $state.kubo; Initialize-Ipfs } catch { $problems += "IPFS reinstall failed: $($_.Exception.Message)" }
    }
    if (-not (Test-ProcRunning 'ipfs')) { Log 'ipfs not running - starting.' 'WARN'; Start-Ipfs | Out-Null; Start-Sleep 5 }
    if (-not (Test-IpfsUp)) { $problems += 'IPFS API is not responding on 5001.' }

    # DigiAsset node
    if (-not (Test-Path $NodeExe)) {
        Log 'DigiAssetWindows.exe missing - reinstalling.' 'WARN'
        try { Install-DigiAsset } catch { $problems += "DigiAsset reinstall failed: $($_.Exception.Message)" }
    }
    if (-not (Test-ProcRunning 'DigiAssetWindows')) { Log 'node not running - starting.' 'WARN'; Start-Node | Out-Null; Start-Sleep 5 }
    if (-not (Test-ProcRunning 'DigiAssetWindows')) { $problems += 'DigiAsset node will not stay running.' }

    # Sync + reachability are informational (can't force-heal a router / a sync).
    $prog = Get-DigiByteProgress
    if ($null -ne $prog) { Log ('DigiByte sync: {0:P1}' -f $prog) } else { Log 'DigiByte RPC not answering yet (still starting or syncing).' 'WARN' }
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
    if ($Mode -eq 'Service') { Invoke-Service } else { Invoke-Install }
} catch {
    Log ("FATAL: $($_.Exception.Message)") 'ERROR'
    if ($Mode -eq 'Service') { Alert "maintenance run failed: $($_.Exception.Message)" }
    else { Write-Host "`nSomething went wrong: $($_.Exception.Message)" -ForegroundColor Red; Write-Host "See $LogFile" }
    exit 1
}

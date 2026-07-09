<#
.SYNOPSIS
    One-time Cloudflare R2 setup for publishing fast-sync snapshots. Installs
    rclone (if missing), configures the R2 remote from your R2 API token, verifies
    it can reach your bucket, and saves the non-secret defaults so you can then run
    publish-snapshot.ps1 with NO arguments.

.DESCRIPTION
    Run this ONCE on the always-on snapshot box (the one with a fully-synced
    DigiByte wallet + DigiAsset node). After it succeeds, snapshot publishing is a
    single command / a weekly scheduled task.

    You need three things from your Cloudflare dashboard:
      1. Account ID          - Cloudflare dashboard -> R2 -> (top right / overview)
      2. R2 API token keys   - R2 -> "Manage R2 API Tokens" -> Create API token
                               (Object Read & Write on your bucket) -> gives you an
                               Access Key ID + Secret Access Key.
      3. Public bucket URL   - R2 -> your bucket -> Settings -> enable public access
                               (r2.dev) -> the https://pub-XXXX.r2.dev URL.
                               (This must match the installer's baked-in snapshot URL.)

    Your Secret Access Key is handed straight to rclone (which stores it in ITS OWN
    config, obfuscated) and is NEVER written to any file in this repo. If a key was
    ever exposed, rotate it in the Cloudflare dashboard and re-run this.

.EXAMPLE
    # interactive (prompts for anything you don't pass)
    powershell -ExecutionPolicy Bypass -File .\setup-cloudflare-snapshots.ps1

.EXAMPLE
    # non-interactive
    .\setup-cloudflare-snapshots.ps1 -AccountId abc123 -AccessKeyId AKID -SecretAccessKey SECRET `
        -Bucket digibyte-snapshots -PublicUrl https://pub-xxxx.r2.dev
#>
[CmdletBinding()]
param(
    [string]$AccountId,
    [string]$AccessKeyId,
    [string]$SecretAccessKey,
    [string]$Bucket     = 'digibyte-snapshots',
    [string]$PublicUrl  = '',
    [string]$RemoteName = 'r2'
)
$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$ScriptVersion = '1.0.0'
function Say($m,$c='Gray'){ Write-Host $m -ForegroundColor $c }
function Step($n,$m){ Write-Host ''; Write-Host "[$n] $m" -ForegroundColor Cyan }
$here = if ($PSScriptRoot) { $PSScriptRoot } else { Split-Path -Parent $MyInvocation.MyCommand.Path }
$cfgFile = Join-Path $here 'snapshot-config.json'   # non-secret defaults (gitignored)

# --- Elevate (installing rclone to Program Files + PATH needs admin) ----------
$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) {
    if ($PSCommandPath) {
        # Forward what we can; the secret is re-prompted in the elevated window
        # rather than passed on a visible command line.
        $fwd = "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" -Bucket `"$Bucket`" -RemoteName `"$RemoteName`""
        if ($AccountId)   { $fwd += " -AccountId `"$AccountId`"" }
        if ($AccessKeyId) { $fwd += " -AccessKeyId `"$AccessKeyId`"" }
        if ($PublicUrl)   { $fwd += " -PublicUrl `"$PublicUrl`"" }
        Say 'Cloudflare setup needs Administrator - approve the UAC prompt...' 'Yellow'
        Start-Process powershell.exe -Verb RunAs -ArgumentList $fwd; return
    } else { throw 'Run this in an elevated (Administrator) PowerShell.' }
}

Say '=================================================================' 'Cyan'
Say " Cloudflare R2 snapshot setup   (v$ScriptVersion)" 'Cyan'
Say '=================================================================' 'Cyan'

# --- 1. Install rclone if it isn't already available -------------------------
Step 1 'rclone'
$rclone = (Get-Command rclone -ErrorAction SilentlyContinue).Source
if ($rclone) {
    Say "  already installed: $rclone" 'Green'
} else {
    Say '  rclone not found - downloading the official Windows build...' 'White'
    $zip = Join-Path $env:TEMP 'rclone-dl.zip'
    $ext = Join-Path $env:TEMP 'rclone-dl'
    Invoke-WebRequest 'https://downloads.rclone.org/rclone-current-windows-amd64.zip' -OutFile $zip -UseBasicParsing -TimeoutSec 120
    if (Test-Path $ext) { Remove-Item $ext -Recurse -Force }
    Expand-Archive -Path $zip -DestinationPath $ext -Force
    $src = Get-ChildItem $ext -Recurse -Filter 'rclone.exe' | Select-Object -First 1
    if (-not $src) { throw 'downloaded rclone zip did not contain rclone.exe' }
    $dest = 'C:\Program Files\rclone'
    New-Item -ItemType Directory -Force -Path $dest | Out-Null
    Copy-Item $src.FullName (Join-Path $dest 'rclone.exe') -Force
    # Add to the Machine PATH (persist) + the current session (so we can use it now).
    $mp = [Environment]::GetEnvironmentVariable('Path','Machine')
    if ($mp -notlike "*$dest*") { [Environment]::SetEnvironmentVariable('Path', "$mp;$dest", 'Machine') }
    $env:Path += ";$dest"
    $rclone = Join-Path $dest 'rclone.exe'
    Say "  installed: $rclone" 'Green'
}

# --- 2. Collect R2 details (prompt for anything not passed) -------------------
Step 2 'Cloudflare R2 account details'
if (-not $AccountId)   { $AccountId   = (Read-Host '  Cloudflare Account ID').Trim() }
if (-not $AccessKeyId) { $AccessKeyId = (Read-Host '  R2 Access Key ID').Trim() }
if (-not $SecretAccessKey) {
    $sec = Read-Host '  R2 Secret Access Key' -AsSecureString
    $SecretAccessKey = [Runtime.InteropServices.Marshal]::PtrToStringAuto([Runtime.InteropServices.Marshal]::SecureStringToBSTR($sec))
}
if (-not $Bucket)    { $Bucket    = (Read-Host '  Bucket name (e.g. digibyte-snapshots)').Trim() }
if (-not $PublicUrl) { $PublicUrl = (Read-Host '  Public bucket URL (https://pub-XXXX.r2.dev)').Trim() }
foreach ($pair in @(@('Account ID',$AccountId),@('Access Key ID',$AccessKeyId),@('Secret Access Key',$SecretAccessKey),@('Bucket',$Bucket),@('Public URL',$PublicUrl))) {
    if (-not $pair[1]) { throw "$($pair[0]) is required." }
}
$endpoint = "https://$AccountId.r2.cloudflarestorage.com"

# --- 3. Create / update the rclone R2 remote ---------------------------------
Step 3 "Configuring rclone remote '$RemoteName' -> $endpoint"
# rclone stores the secret in ITS config (obfuscated), not in this repo.
& $rclone config create $RemoteName s3 provider Cloudflare access_key_id $AccessKeyId secret_access_key $SecretAccessKey endpoint $endpoint region auto acl private --non-interactive | Out-Null
if ($LASTEXITCODE -ne 0) { throw "rclone config create failed (exit $LASTEXITCODE)." }
Say "  remote '$RemoteName' saved." 'Green'

# --- 4. Verify we can reach the bucket ---------------------------------------
Step 4 "Verifying access to ${RemoteName}:${Bucket}"
& $rclone lsf "${RemoteName}:${Bucket}/" --s3-no-check-bucket --max-depth 1 2>$null | Out-Null
if ($LASTEXITCODE -ne 0) {
    Say "  Could not list ${RemoteName}:${Bucket}." 'Yellow'
    Say '  Check: the bucket name is correct, and your API token has Object Read & Write' 'Yellow'
    Say '  on this bucket. (A brand-new empty bucket lists fine; a wrong key/bucket 403s.)' 'Yellow'
    throw "R2 access check failed - fix the token/bucket and re-run."
}
Say "  OK - reached the bucket." 'Green'

# --- 5. Save non-secret defaults so publish-snapshot needs no arguments -------
Step 5 'Saving snapshot defaults (no secrets)'
[ordered]@{ remote=$RemoteName; bucket=$Bucket; publicUrl=$PublicUrl.TrimEnd('/'); accountId=$AccountId; created=(Get-Date).ToString('s') } |
    ConvertTo-Json | Set-Content -Path $cfgFile -Encoding UTF8
Say "  wrote $cfgFile  (remote/bucket/publicUrl - no keys)." 'Green'

Write-Host ''
Say '===================== R2 IS READY =====================' 'Green'
Say ' Now publish a snapshot with just:' 'White'
Say '   powershell -ExecutionPolicy Bypass -File .\publish-snapshot.ps1' 'Cyan'
Say ' Or schedule it weekly:' 'White'
Say '   powershell -ExecutionPolicy Bypass -File .\publish-snapshot.ps1 -Schedule' 'Cyan'
Say '' 'White'
Say " IMPORTANT: make sure the installer's snapshot URL matches:" 'Yellow'
Say "   $($PublicUrl.TrimEnd('/'))/snapshot.json" 'Yellow'

<#
.SYNOPSIS
    Smoke-test EVERY DigiAssetWindows-cli DigiAsset command against a running node.

.DESCRIPTION
    Runs each DigiAsset RPC method through DigiAssetWindows-cli.exe with valid,
    predefined syntax and prints a pass/fail table. It auto-discovers a real asset
    (index + id), a holder address, and an exchange-rate address from the node so
    the asset/address/rate commands run against live data - override any of them
    with the params below.

    Classification per command:
      PASS  - the node answered with a result (command works).
      NOTE  - the command ran but returned an error/empty (e.g. "asset not found",
              "no KYC") - the method is recognized and functioning, just no data.
      FAIL  - the node was unreachable, the command timed out, or it was rejected
              as an unknown method. These are the ones to look at.

    Destructive commands (resyncmetadata, shutdown) are SKIPPED unless you pass
    -IncludeDestructive; shutdown always runs last and asks first.

    Read-only: it never spends coins and never touches the wallet. Run it on the
    node box (the CLI reads config.cfg / RPC creds from the node folder).

.PARAMETER Cli          Path to DigiAssetWindows-cli.exe (default: C:\DigiAssetWindows\...).
.PARAMETER AssetIndex   Asset index to test with (default: auto-discovered).
.PARAMETER AssetId      Asset id to test with (default: auto-discovered).
.PARAMETER Address      Address to test with (default: a holder of the sample asset).
.PARAMETER Domain       Domain to test getdomainaddress with (default: 'digibyte').
.PARAMETER IncludeDestructive  Also run resyncmetadata + shutdown (shutdown last, with a prompt).
.PARAMETER OutFile      Write the full transcript to this file too.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\test-cli.ps1
.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\test-cli.ps1 -AssetIndex 12345 -Address D... -OutFile cli-test.log
#>
[CmdletBinding()]
param(
    [string]$Cli = 'C:\DigiAssetWindows\DigiAssetWindows-cli.exe',
    [string]$AssetIndex = '',
    [string]$AssetId = '',
    [string]$Address = '',
    [string]$Domain = 'digibyte',
    [int]   $TimeoutSec = 30,
    [switch]$IncludeDestructive,
    [string]$OutFile = ''
)
$ErrorActionPreference = 'Continue'

# ---- Resolve the CLI ---------------------------------------------------------
if (-not (Test-Path $Cli)) {
    $alt = Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) '..\build\cli\Release\DigiAssetWindows-cli.exe'
    if (Test-Path $alt) { $Cli = (Resolve-Path $alt).Path }
    else { throw "DigiAssetWindows-cli.exe not found at $Cli. Pass -Cli <path>." }
}
$workDir = Split-Path -Parent $Cli   # so the CLI finds config.cfg / RPC creds
$log = New-Object System.Collections.Generic.List[string]
function Out2($m, $c = 'Gray') { Write-Host $m -ForegroundColor $c; $log.Add(($m -replace '\x1b\[[0-9;]*m','')) }

# ---- Core runner: run one CLI command, capture + classify --------------------
$results = New-Object System.Collections.Generic.List[object]
function Invoke-Cli {
    param([string]$Name, [string[]]$CliArgs, [string]$Group = 'general')
    $display = ($CliArgs -join ' ')
    $raw = ''
    $code = 0
    try {
        Push-Location $workDir
        $raw = (& $Cli @CliArgs 2>&1 | Out-String)
        $code = $LASTEXITCODE
    } catch {
        $raw = "$($_.Exception.Message)"
        $code = -1
    } finally { Pop-Location }

    $t = ($raw + '').Trim()
    $lower = $t.ToLower()
    $status = 'PASS'

    if ($lower -match 'no connection could be made|actively refused|failed to connect|could ?n.?t connect|connection refused|timed out|timeout|unable to connect|no response') {
        $status = 'FAIL'   # transport - node not reachable
    } elseif ($lower -match 'method not found|-32601|unknown method|unknown command|invalid method') {
        $status = 'FAIL'   # command not recognized
    } elseif ($code -ne 0 -and $t -eq '') {
        $status = 'FAIL'   # non-zero exit, no output at all
    } elseif ($lower -match '"error"|^error\b|error:|exception|invalid parameter|not found|does not exist|no such|missing') {
        $status = 'NOTE'   # method ran, returned an error/empty result
    } elseif ($t -eq '') {
        $status = 'NOTE'   # empty but clean exit (e.g. void-ish)
    } else {
        $status = 'PASS'
    }

    $detail = ($t -replace '\s+', ' ')
    if ($detail.Length -gt 90) { $detail = $detail.Substring(0, 90) + '...' }
    $results.Add([pscustomobject]@{ Status = $status; Name = $Name; Cmd = "cli $display"; Detail = $detail; Group = $Group })

    $col = switch ($status) { 'PASS' { 'Green' } 'NOTE' { 'Yellow' } default { 'Red' } }
    Out2 ("  [{0}] {1,-26} {2}" -f $status, $Name, $detail) $col
    return $t
}

# Small helper: pull the first regex capture from CLI JSON-ish text.
function First-Match($text, $pattern) {
    $m = [regex]::Match($text, $pattern)
    if ($m.Success) { return $m.Groups[1].Value }
    return ''
}

Out2 '==============================================================' 'Cyan'
Out2 " DigiAssetWindows-cli command smoke test" 'Cyan'
Out2 " CLI: $Cli" 'Cyan'
Out2 '==============================================================' 'Cyan'

# ---- Preflight: is the node reachable? ---------------------------------------
Out2 "`n[preflight]" 'White'
$ver = Invoke-Cli 'version' @('version') 'preflight'
if ($results[0].Status -eq 'FAIL') {
    Out2 "`nThe node did not answer 'version'. Is DigiAssetWindows.exe running and synced enough to serve RPC (port 14024)?" 'Red'
    Out2 "Start the node, then re-run this test." 'Red'
    if ($OutFile) { Set-Content -Path $OutFile -Value $log -Encoding UTF8 }
    exit 2
}
$sync = Invoke-Cli 'syncstate' @('syncstate') 'preflight'
$syncH = First-Match $sync '"height"\s*:\s*(\d+)'
if ($syncH) { Out2 "  node analyzer height: $syncH" 'Gray' }

# ---- Auto-discover a sample asset / address / exchange rate ------------------
Out2 "`n[discovery]" 'White'
if (-not $AssetIndex -or -not $AssetId) {
    $sample = Invoke-Cli 'listlastassets(discover)' @('listlastassets', '1') 'discovery'
    if (-not $AssetIndex) { $AssetIndex = First-Match $sample '"assetIndex"\s*:\s*(\d+)' }
    if (-not $AssetId)    { $AssetId    = First-Match $sample '"assetId"\s*:\s*"([^"]+)"' }
}
if (-not $Address -and $AssetIndex) {
    $holders = Invoke-Cli 'getassetholders(discover)' @('getassetholders', "$AssetIndex") 'discovery'
    # holders is an object { "address": count, ... } - grab the first key that looks like an address
    $Address = First-Match $holders '"([A-Za-z0-9]{20,})"\s*:'
}
# an exchange-rate publisher (address + index) for getdgbequivalent, if any exist
$rates = Invoke-Cli 'getexchangerates(discover)' @('getexchangerates') 'discovery'
$rateAddr = First-Match $rates '"address"\s*:\s*"([^"]+)"'
$rateIdx  = First-Match $rates '"index"\s*:\s*(\d+)'
if (-not $rateAddr) { $rateAddr = $Address; $rateIdx = '0' }

$aiDisp  = if ($AssetIndex) { $AssetIndex } else { '(none)' }
$aidDisp = if ($AssetId) { $AssetId.Substring(0, [Math]::Min(16, $AssetId.Length)) + '...' } else { '(none)' }
$adDisp  = if ($Address) { $Address } else { '(none found)' }
Out2 ("  using  assetIndex={0}  assetId={1}" -f $aiDisp, $aidDisp) 'Gray'
Out2 ("  using  address={0}" -f $adDisp) 'Gray'

# ---- The test battery --------------------------------------------------------
# Each entry: Name, argument array (strings). Asset/address ones are skipped
# cleanly if discovery found nothing.
Out2 "`n[no-argument / list commands]" 'White'
Invoke-Cli 'getnodestats'              @('getnodestats')                       'basic'  | Out-Null
Invoke-Cli 'getipfscount'              @('getipfscount')                       'basic'  | Out-Null
Invoke-Cli 'debugwaittimes'            @('debugwaittimes')                     'basic'  | Out-Null
Invoke-Cli 'getexchangerates'          @('getexchangerates')                   'basic'  | Out-Null
Invoke-Cli 'algostats'                 @('algostats')                          'basic'  | Out-Null
Invoke-Cli 'addressstats'              @('addressstats')                       'basic'  | Out-Null
Invoke-Cli 'getpsp'                    @('getpsp', '1')                        'basic'  | Out-Null
Invoke-Cli 'getoldstreamkey'           @('getoldstreamkey', '0')               'basic'  | Out-Null
Invoke-Cli 'listlastblocks'            @('listlastblocks', '5')                'list'   | Out-Null
Invoke-Cli 'listassets'                @('listassets', '5', '1')               'list'   | Out-Null
Invoke-Cli 'listlastassets'            @('listlastassets', '5')                'list'   | Out-Null
Invoke-Cli 'listlastassetspageindexes' @('listlastassetspageindexes', '5')     'list'   | Out-Null

Out2 "`n[asset commands]" 'White'
if ($AssetIndex) {
    Invoke-Cli 'getassetdata'          @('getassetdata', "$AssetIndex")        'asset'  | Out-Null
    Invoke-Cli 'getassetholders'       @('getassetholders', "$AssetIndex")     'asset'  | Out-Null
} else { Out2 '  (skipped getassetdata/getassetholders - no sample asset found)' 'DarkGray' }
if ($AssetId) {
    Invoke-Cli 'getassetindexes'       @('getassetindexes', "$AssetId")        'asset'  | Out-Null
    Invoke-Cli 'listassetissuances'    @('listassetissuances', "$AssetId")     'asset'  | Out-Null
} else { Out2 '  (skipped getassetindexes/listassetissuances - no sample assetId found)' 'DarkGray' }

Out2 "`n[address commands]" 'White'
if ($Address) {
    Invoke-Cli 'getaddressholdings'    @('getaddressholdings', "$Address")     'address' | Out-Null
    Invoke-Cli 'listaddresshistory'    @('listaddresshistory', "$Address")     'address' | Out-Null
    Invoke-Cli 'getaddresskyc'         @('getaddresskyc', "$Address")          'address' | Out-Null
    Invoke-Cli 'getencryptedkey'       @('getencryptedkey', "$Address")        'address' | Out-Null
} else { Out2 '  (skipped address commands - no sample address found; pass -Address D...)' 'DarkGray' }

Out2 "`n[lookup commands]" 'White'
Invoke-Cli 'getdomainaddress'          @('getdomainaddress', "$Domain")        'lookup' | Out-Null
Invoke-Cli 'getdgbequivalent'          @('getdgbequivalent', "$rateAddr", "$rateIdx", '100000000') 'lookup' | Out-Null

Out2 "`n[async queue trio]" 'White'
Invoke-Cli 'asyncstart'                @('asyncstart', 'syncstate')            'async'  | Out-Null
Start-Sleep -Milliseconds 500
Invoke-Cli 'asyncget'                  @('asyncget', 'syncstate')              'async'  | Out-Null
Invoke-Cli 'asyncclear'                @('asyncclear', 'syncstate')            'async'  | Out-Null

# ---- Destructive (opt-in) ----------------------------------------------------
if ($IncludeDestructive) {
    Out2 "`n[destructive - -IncludeDestructive]" 'Magenta'
    Invoke-Cli 'resyncmetadata'        @('resyncmetadata')                     'destructive' | Out-Null
    $ans = Read-Host 'Run "shutdown" now? It STOPS the node. (y/N)'
    if ($ans -match '^[Yy]') {
        Invoke-Cli 'shutdown'          @('shutdown')                           'destructive' | Out-Null
    } else { Out2 '  (skipped shutdown)' 'DarkGray' }
} else {
    Out2 "`n(skipped resyncmetadata + shutdown - pass -IncludeDestructive to run them)" 'DarkGray'
}

# ---- Summary -----------------------------------------------------------------
$pass = ($results | Where-Object Status -eq 'PASS').Count
$note = ($results | Where-Object Status -eq 'NOTE').Count
$fail = ($results | Where-Object Status -eq 'FAIL').Count
Out2 "`n==============================================================" 'Cyan'
Out2 (" RESULT   PASS={0}   NOTE={1}   FAIL={2}   (of {3} commands)" -f $pass, $note, $fail, $results.Count) $(if ($fail) { 'Red' } else { 'Green' })
Out2 '==============================================================' 'Cyan'
if ($fail) {
    Out2 'Failed commands (unreachable / unknown method):' 'Red'
    $results | Where-Object Status -eq 'FAIL' | ForEach-Object { Out2 ("  - {0}  ->  {1}" -f $_.Name, $_.Detail) 'Red' }
}
if ($note) {
    Out2 "NOTE = the command works but returned an error/empty (often just 'no data for this input'). Review if unexpected." 'Yellow'
}

if ($OutFile) { Set-Content -Path $OutFile -Value $log -Encoding UTF8; Out2 "`nTranscript written to $OutFile" 'Gray' }
exit $fail

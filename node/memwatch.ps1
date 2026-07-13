<#
.SYNOPSIS
    Sample the memory of DigiAssetWindows.exe (+ the pool server) over time to a
    CSV and report the growth rate - the first step in finding a memory leak.

.DESCRIPTION
    A real leak shows up as Private Bytes rising WITHOUT BOUND. The catch: memory
    also grows legitimately while the node is doing initial sync (caches filling),
    then plateaus. So to trust the result:

        Run this only AFTER the node is fully synced (at the tip), or against the
        pool while it handles steady traffic. Then a steadily-climbing MB/hour
        means a leak; a rate that trends toward zero means it is just caching.

    Private Bytes (PrivateMemorySize64) is the best leak signal - it excludes
    shared/mapped memory (like the mmap'd chain.db) that inflates Working Set.

.PARAMETER IntervalSec  Seconds between samples (default 300 = 5 min).
.PARAMETER Samples      Stop after this many samples (0 = run until Ctrl-C).
.PARAMETER Processes    Process names to watch (no .exe). Default: node + pool.
.PARAMETER OutCsv       CSV log path (default .\memwatch-<date>.csv in the cwd).

.EXAMPLE
    # Watch the node every 5 min until you press Ctrl-C:
    powershell -ExecutionPolicy Bypass -File .\memwatch.ps1
.EXAMPLE
    # Tight 60s sampling of the pool during a load test, 30 samples:
    .\memwatch.ps1 -Processes DigiAssetPoolServer -IntervalSec 60 -Samples 30
#>
[CmdletBinding()]
param(
    [int]$IntervalSec = 300,
    [int]$Samples     = 0,
    [string[]]$Processes = @('DigiAssetWindows', 'DigiAssetPoolServer'),
    [string]$OutCsv = ''
)
$ErrorActionPreference = 'Stop'
if (-not $OutCsv) { $OutCsv = Join-Path (Get-Location) ('memwatch-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.csv') }

function MB($bytes) { return [math]::Round($bytes / 1MB, 1) }

# Per-process running state: first sample (baseline) + previous sample.
$state = @{}
"timestamp,process,pid,privateMB,workingSetMB,handles,threads" | Out-File -FilePath $OutCsv -Encoding ASCII

Write-Host "=== memwatch ===" -ForegroundColor Cyan
Write-Host ("Watching : " + ($Processes -join ', ')) -ForegroundColor Gray
Write-Host ("Interval : " + $IntervalSec + "s   Log: " + $OutCsv) -ForegroundColor Gray
Write-Host "NOTE: growth during initial sync is normal. Judge a leak only at the tip / steady state." -ForegroundColor Yellow
Write-Host "Press Ctrl-C to stop and print the summary.`n" -ForegroundColor Gray

$n = 0
try {
    while ($true) {
        $n++
        $now = Get-Date
        foreach ($name in $Processes) {
            $p = Get-Process -Name $name -ErrorAction SilentlyContinue | Sort-Object StartTime | Select-Object -First 1
            if (-not $p) { Write-Host ("[{0:HH:mm:ss}] {1}: not running" -f $now, $name) -ForegroundColor DarkGray; continue }

            $priv = $p.PrivateMemorySize64
            $ws   = $p.WorkingSet64
            ("{0:o},{1},{2},{3},{4},{5},{6}" -f $now, $name, $p.Id, (MB $priv), (MB $ws), $p.HandleCount, $p.Threads.Count) |
                Out-File -FilePath $OutCsv -Append -Encoding ASCII

            if (-not $state.ContainsKey($name)) {
                $state[$name] = @{ t0 = $now; priv0 = $priv; pidSeen = $p.Id }
            }
            $s = $state[$name]
            # If the process restarted (new PID), reset the baseline so we don't
            # report a bogus negative/positive step across a restart.
            if ($s.pidSeen -ne $p.Id) { $s.t0 = $now; $s.priv0 = $priv; $s.pidSeen = $p.Id; Write-Host ("  ({0} restarted - baseline reset)" -f $name) -ForegroundColor DarkYellow }

            $dtHr    = ($now - $s.t0).TotalHours
            $deltaMB = MB ($priv - $s.priv0)
            $rate    = if ($dtHr -gt 0.001) { [math]::Round(($priv - $s.priv0) / 1MB / $dtHr, 2) } else { 0 }
            $color   = if ($rate -gt 5) { 'Red' } elseif ($rate -gt 1) { 'Yellow' } else { 'Green' }
            Write-Host ("[{0:HH:mm:ss}] {1,-22} priv {2,8} MB   ws {3,8} MB   handles {4,6}   since-start {5,7} MB   rate {6,7} MB/hr" -f `
                        $now, $name, (MB $priv), (MB $ws), $p.HandleCount, $deltaMB, $rate) -ForegroundColor $color
        }
        if ($Samples -gt 0 -and $n -ge $Samples) { break }
        Start-Sleep -Seconds $IntervalSec
    }
} finally {
    Write-Host "`n=== summary ===" -ForegroundColor Cyan
    foreach ($name in $Processes) {
        if (-not $state.ContainsKey($name)) { continue }
        $s = $state[$name]
        $p = Get-Process -Name $name -ErrorAction SilentlyContinue | Select-Object -First 1
        if (-not $p) { Write-Host ("{0}: not running at end." -f $name) -ForegroundColor DarkGray; continue }
        $dtHr    = ($(Get-Date) - $s.t0).TotalHours
        $rate    = if ($dtHr -gt 0.001) { [math]::Round(($p.PrivateMemorySize64 - $s.priv0) / 1MB / $dtHr, 2) } else { 0 }
        $verdict = if ($dtHr -lt 0.5) { 'inconclusive (watch >= 1 hr at the tip)' }
                   elseif ($rate -gt 2) { 'SUSPECT LEAK - Private Bytes climbing steadily' }
                   elseif ($rate -gt 0.5) { 'mild growth - keep watching / may still be caching' }
                   else { 'stable - no obvious leak' }
        Write-Host ("{0}: {1} MB/hr over {2:N1} hr  ->  {3}" -f $name, $rate, $dtHr, $verdict) -ForegroundColor White
    }
    Write-Host ("CSV: " + $OutCsv) -ForegroundColor Gray
    Write-Host "If a process SUSPECTS a leak, localize it with the VS Memory Usage profiler (snapshot-diff at the tip) or Dr. Memory." -ForegroundColor Gray
}

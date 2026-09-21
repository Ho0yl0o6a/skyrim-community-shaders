param(
    [ValidateRange(3, 30)][int]$Seconds = 10,
    # LaunchTest.ps1 keeps the runtime log beside the workspace: its default home
    # is on the system drive, and when that volume is short of space the writes
    # stop without error and the timing records this reads simply disappear.
    [string]$RuntimeLog = (Join-Path $PSScriptRoot '../../.research/testlogs/remix-dxvk.log')
)
$ErrorActionPreference = 'Stop'
function Get-TestState {
    Invoke-RestMethod -Uri 'http://127.0.0.1:8920/api/tool/inspect' -Method Post `
        -ContentType 'application/json' -Body '{"kind":"state"}' -TimeoutSec 3
}
function Get-Percentile([double[]]$Values, [double]$Fraction) {
    if (!$Values.Count) { return $null }
    $orderedValues = @($Values | Sort-Object)
    $index = [Math]::Min($orderedValues.Count - 1, [Math]::Ceiling($Fraction * $orderedValues.Count) - 1)
    [Math]::Round($orderedValues[$index], 3)
}
# Read only newly appended timing records. No settings, saves or files are changed.
$stream = [IO.File]::Open($RuntimeLog, [IO.FileMode]::Open, [IO.FileAccess]::Read,
    [IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete)
try {
    $null = $stream.Seek(0, [IO.SeekOrigin]::End)
    $reader = [IO.StreamReader]::new($stream)
    $before = Get-TestState
    if (!$before.playerLoaded) { throw 'No loaded player; load the Riverwood test first.' }
    $watch = [Diagnostics.Stopwatch]::StartNew()
    do { Start-Sleep -Milliseconds 250 } while ($watch.Elapsed.TotalSeconds -lt $Seconds)
    $after = Get-TestState
    $watch.Stop()
    if ($after.pid -ne $before.pid -or $after.frame -lt $before.frame) { throw 'Game restarted during measurement.' }
    $records = $reader.ReadToEnd() -split '\r?\n'
    $timings = @{}
    foreach ($record in $records) {
        if ($record -notmatch '\[CSRemix.GPU\].*scope=(\w+)\s') { continue }
        $scope = $Matches[1]
        foreach ($entry in [regex]::Matches($record, '(\w+)=([0-9]+(?:\.[0-9]+)?(?:e\+?[0-9]+)?)')) {
            if ($entry.Groups[1].Value -in @('frame', 'dropped', 'abandoned')) { continue }
            # A GPU stage that reads as seconds is a wrapped negative timestamp
            # delta, not a slow stage. The runtime drops these at source now;
            # this keeps an older build's log from poisoning a median.
            $value = [double]::Parse($entry.Groups[2].Value, [Globalization.NumberStyles]::Float, [Globalization.CultureInfo]::InvariantCulture)
            if ($value -gt 1000) { continue }
            $key = "$scope.$($entry.Groups[1].Value)"
            if (!$timings.ContainsKey($key)) { $timings[$key] = [Collections.Generic.List[double]]::new() }
            $timings[$key].Add($value)
        }
    }
    $summary = [ordered]@{}
    foreach ($key in @($timings.Keys | Sort-Object)) {
        $summary[$key] = [ordered]@{
            samples = $timings[$key].Count
            medianMs = Get-Percentile $timings[$key].ToArray() 0.5
            p95Ms = Get-Percentile $timings[$key].ToArray() 0.95
        }
    }
    [ordered]@{
        pid = $after.pid
        seconds = [Math]::Round($watch.Elapsed.TotalSeconds, 3)
        frames = $after.frame - $before.frame
        frameRate = [Math]::Round(($after.frame - $before.frame) / $watch.Elapsed.TotalSeconds, 2)
        gpu = $summary
        note = 'Frame rate only; does not validate output resolution or visual correctness. GPU samples require CS_REMIX_GPU_TIMING=1 at launch.'
    } | ConvertTo-Json -Depth 5
} finally {
    if ($reader) { $reader.Dispose() } else { $stream.Dispose() }
}

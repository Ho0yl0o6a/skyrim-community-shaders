param(
    [string]$LogPath = (Join-Path $PSScriptRoot '../../.research/testlogs/remix-dxvk.log'),
    [ValidateRange(1, 1000000)][int]$MinimumFrames = 120
)
$ErrorActionPreference = 'Stop'
$required = @('frame','samples','violationFrames','draws','nodes','hostNodes','instances',
    'badEntries','badOwners','badPrims','gcMarked','unowned','staleUnowned')
$violationFields = @('badEntries','badOwners','badPrims','gcMarked')
$rows = @(foreach ($line in (& rg --no-heading --fixed-strings '[CSRemix.retainedAudit]' $LogPath)) {
    $row = [ordered]@{}
    foreach ($match in [regex]::Matches($line, '(\w+)=(\d+)')) {
        $row[$match.Groups[1].Value] = [uint64]::Parse($match.Groups[2].Value)
    }
    foreach ($field in $required) {
        if (!$row.Contains($field)) { throw "Incomplete runtime audit record: missing $field" }
    }
    [pscustomobject]$row
})
if ($LASTEXITCODE -gt 1) { throw 'Runtime log search failed.' }
$failures = @()
if ($rows.Count -lt $MinimumFrames) { $failures += 'Insufficient runtime frame coverage.' }
$bad = @($rows | Where-Object {
    $row = $_
    @($violationFields | Where-Object { $row.$_ -gt 0 }).Count -gt 0
})
if ($bad.Count) { $failures += 'Runtime ownership invariant violation detected.' }
$stale = @($rows | Where-Object { $_.staleUnowned -gt 0 })
if ($stale.Count) { $failures += 'Non-host instance survived without a current-frame draw; inspect lifetime policy and cell before attributing a visible leak.' }
if ($rows.Count -and $rows[-1].violationFrames -gt 0) {
    $failures += 'Runtime cumulative counter records an ownership violation, possibly outside this log excerpt.'
}
$lastFrame = $null
$lastSample = $null
foreach ($row in $rows) {
    if ($null -ne $lastFrame -and ($row.frame -le $lastFrame -or $row.samples -ne ($lastSample + 1))) {
        $failures += 'Audit records are duplicated, out of order or incomplete.'
        break
    }
    $lastFrame = $row.frame
    $lastSample = $row.samples
}
[ordered]@{
    passed=($failures.Count -eq 0);failures=$failures;frames=$rows.Count;
    first=($rows | Select-Object -First 1);last=($rows | Select-Object -Last 1);
    firstViolation=($bad | Select-Object -First 1);staleFrames=$stale.Count;
    retainedRange=($rows.draws | Measure-Object -Minimum -Maximum | Select-Object Minimum,Maximum);
    frequentRetainedCounts=@($rows | Group-Object draws | Sort-Object Count -Descending | Select-Object -First 8 Name,Count);
    scope='Runtime CPU census after garbage collection and before graph overrides/TLAS preparation. Not GPU readback, final pixels, cell identity, or proof of all submission classes.'
} | ConvertTo-Json -Depth 5
if ($failures.Count) { throw ($failures -join ' ') }

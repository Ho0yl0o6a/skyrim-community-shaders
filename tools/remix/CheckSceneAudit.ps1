param(
    [switch]$Start,
    [switch]$Freeze,
    [ValidateRange(1, 1000000)][int]$MinimumInteriorFrames = 60,
    [ValidateRange(0, 1000)][int]$MinimumCellChanges = 0,
    [switch]$RequireExteriorControls
)
$ErrorActionPreference = 'Stop'
function Invoke-DB([string]$Tool, [hashtable]$Body) {
    Invoke-RestMethod -Uri "http://127.0.0.1:8920/api/tool/$Tool" -Method Post `
        -ContentType application/json -Body ($Body | ConvertTo-Json -Compress) -TimeoutSec 10
}
if ($Start -and $Freeze) { throw 'Start and Freeze are mutually exclusive.' }
if ($Start -or $Freeze) {
    $value = if ($Start) { 'true' } else { 'false' }
    $result = Invoke-DB 'communityshaders.remix' @{name='cs.sceneAudit';value=$value}
    if (!$result.success) { throw 'Scene audit toggle rejected; verify the deployed plugin.' }
    if ($Start) { Write-Output 'Scene audit enabled and reset.'; return }
}
$audit = Invoke-DB 'communityshaders.inspect' @{kind='remixScene';filter='audit'}
if ($null -eq $audit.samples) { throw 'No scene audit in the running plugin.' }
$failures = @()
if ($audit.interiorSamples -lt $MinimumInteriorFrames) { $failures += 'Insufficient interior frame coverage.' }
if ($audit.cellChanges -lt $MinimumCellChanges) { $failures += 'Insufficient cell-transition coverage.' }
if ($audit.suspectSamples -gt 0) { $failures += 'Suspect host membership/dome detected; inspect firstSuspect. Sky-enabled interiors may intentionally contain sky.' }
if ($audit.failedSamples -gt 0) { $failures += 'World submission failed during the audit.' }
if ($audit.emptyReadySamples -gt 0) { $failures += 'Ready world submission contained no host instances.' }
if ($RequireExteriorControls -and ($audit.exteriorDomeSamples -eq 0 -or
    (($audit.exteriorMax | Measure-Object -Sum).Sum -eq 0))) {
    $failures += 'Missing outdoor geometry/dome positive controls.'
}
[ordered]@{passed=($failures.Count -eq 0);failures=$failures;audit=$audit} | ConvertTo-Json -Depth 8
if ($failures.Count) { throw ($failures -join ' ') }

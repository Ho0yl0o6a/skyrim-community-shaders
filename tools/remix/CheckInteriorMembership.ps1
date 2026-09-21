param(
    [ValidateSet('RiverwoodSleepingGiantInn', 'RiverwoodRiverwoodTrader')]
    [string]$ExpectedCell = 'RiverwoodSleepingGiantInn'
)
$ErrorActionPreference = 'Stop'
function Invoke-DB([string]$Name, [hashtable]$Body) {
    Invoke-RestMethod -Uri "http://127.0.0.1:8920/api/tool/$Name" -Method Post -ContentType 'application/json' -TimeoutSec 10 -Body ($Body | ConvertTo-Json -Compress)
}
$scene = Invoke-DB 'inspect' @{kind='scene'}
if ($scene.cell.editorId -ne $ExpectedCell) { throw "This regression requires the player in $ExpectedCell." }
# Match actual exterior shader classes/features, not "LOD" in ancestor names:
# ObjectLODRoot also parents the inn's walls, furniture and actors. Likewise,
# its authored dust beams use a Cloud texture without being exterior sky.
$exteriorFilters = @(
    'FXSplashLargeChurn', 'FXWaterfall', 'Tamriel',
    'BSDistantTreeShaderProperty', 'BSSkyShaderProperty',
    'feature:9', 'feature:15', 'feature:17', 'feature:18'
)
$positiveFilters = @('Flames:', 'MaleHead')
$groups = foreach ($filter in ($exteriorFilters + $positiveFilters)) {
    $sample = Invoke-DB 'communityshaders.inspect' @{kind='remixScene';filter=$filter}
    [ordered]@{
        filter = $filter
        loaded = $sample.loaded
        submittedScene = $sample.submitted
        matched = $sample.matched
        submittedSample = @($sample.entries | Where-Object submitted).Count
    }
}
$failures = @()
foreach ($group in ($groups | Where-Object { $_.filter -in $exteriorFilters })) {
    if ($group.matched -gt 0) { $failures += "Exterior geometry retained: $($group.filter) ($($group.matched))" }
}
foreach ($filter in $positiveFilters) {
    $group = $groups | Where-Object filter -eq $filter
    if ($group.submittedSample -eq 0) { $failures += "Interior positive control missing: $filter" }
}
[ordered]@{
    cell = $scene.cell.editorId
    groups = @($groups)
    failures = $failures
    note = 'Plugin membership checks only. Sky-dome state, runtime-retained instances, transition frames and image correctness require separate checks. LOD ancestor and Cloud texture names are not exterior-membership predicates.'
} | ConvertTo-Json -Depth 5
if ($failures.Count) { throw ($failures -join '; ') }

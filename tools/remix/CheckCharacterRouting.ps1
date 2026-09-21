param([string]$Report, [string]$Filter = 'feature:6')
$ErrorActionPreference = 'Stop'
function Invoke-DB([string]$Tool, [hashtable]$Body) {
    Invoke-RestMethod -Uri "http://127.0.0.1:8920/api/tool/$Tool" -Method Post `
        -ContentType application/json -Body ($Body | ConvertTo-Json -Compress) -TimeoutSec 10
}
$initial = Invoke-DB 'inspect' @{kind='state'}
if (!$initial.playerLoaded) { throw 'Load a world containing characters first.' }
$rows = @()
for ($sample = 0; $sample -lt 6; ++$sample) {
    # Skyrim HairTint is feature 6; geometry names are not reliable classifiers.
    $scene = Invoke-DB 'communityshaders.inspect' @{kind='remixScene';filter=$Filter}
    if (!$scene.submittedCamera.valid -or !$scene.submitted) { throw 'No submitted world.' }
    if (!$scene.matched -or $scene.entries.Count -ne $scene.matched) {
        throw 'Hair census is empty or truncated; cannot validate every matching record.'
    }
    $active = @($scene.entries | Where-Object submitted)
    if (!$active.Count) { throw 'No submitted hair.' }
    foreach ($entry in $active) {
        if ($entry.feature -ne 6 -or !$entry.importedMaterial -or !$entry.hairCardsRequested -or $entry.thinFoliage) {
            throw "Wrong hair material/category routing: $($entry.name)"
        }
    }
    if (!@($active | Where-Object { $_.submittedBones -gt 0 }).Count) { throw 'No GPU bone palettes on submitted hair.' }
    $rows += [ordered]@{frame=$scene.frame;matched=$scene.matched;active=@($active | Select-Object name,feature,hairCardsRequested,submittedBones,importedMaterial)}
    Start-Sleep -Milliseconds 150
}
$final = Invoke-DB 'inspect' @{kind='state'}
if ($final.pid -ne $initial.pid -or $rows[-1].frame -le $rows[0].frame) { throw 'Process changed or rendering stopped.' }
$result = [ordered]@{passed=$true;pid=$final.pid;filter=$Filter;samples=$rows;
    scope='Host hair classification, material import and bone-palette submission for the recorded filter only. Not GPU shading, fiber BRDF, animation continuity or appearance validation.'} | ConvertTo-Json -Depth 7
if ($Report) { $result | Out-File -LiteralPath $Report -Encoding utf8 }
$result

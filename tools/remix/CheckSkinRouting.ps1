param([string]$Report)
$ErrorActionPreference = 'Stop'
function Invoke-DB([string]$Tool, [hashtable]$Body) {
    Invoke-RestMethod -Uri "http://127.0.0.1:8920/api/tool/$Tool" -Method Post `
        -ContentType application/json -Body ($Body | ConvertTo-Json -Compress) -TimeoutSec 10
}
$initial = Invoke-DB 'inspect' @{kind='state'}
if (!$initial.playerLoaded) { throw 'Load a world containing characters first.' }
$rows = @()
for ($sample = 0; $sample -lt 6; ++$sample) {
    foreach ($filter in @('feature:4', 'feature:5', 'Hair')) {
        $scene = Invoke-DB 'communityshaders.inspect' @{kind='remixScene';filter=$filter}
        if (!$scene.submittedCamera.valid -or !$scene.submitted) { throw 'No submitted world.' }
        if (!$scene.matched -or $scene.entries.Count -ne $scene.matched) { throw "Empty or truncated census: $filter" }
        $active = @($scene.entries | Where-Object submitted)
        if (!$active.Count) { throw "No active material samples: $filter" }
        foreach ($entry in $active) {
            $expectedSkin = $entry.feature -in @(4,5)
            if (!$entry.importedMaterial -or $entry.skinDiffusionImported -ne $expectedSkin -or $entry.thinFoliage) {
                throw "Wrong skin routing: $($entry.name), feature $($entry.feature)"
            }
        }
        $rows += [ordered]@{frame=$scene.frame;filter=$filter;matched=$scene.matched;
            active=@($active | Select-Object name,feature,skinDiffusionImported,submittedBones,modelSpaceNormals)}
    }
    Start-Sleep -Milliseconds 150
}
$final = Invoke-DB 'inspect' @{kind='state'}
if ($final.pid -ne $initial.pid -or $rows[-1].frame -le $rows[0].frame) { throw 'Process changed or rendering stopped.' }
$result = [ordered]@{passed=$true;pid=$final.pid;samples=$rows;
    scope='Host FaceGen diffusion imports and non-skin hair exclusion over the recorded frames. Does not prove shader execution, calibrated scattering, animation or appearance.'} | ConvertTo-Json -Depth 7
if ($Report) { $result | Out-File -LiteralPath $Report -Encoding utf8 }
$result

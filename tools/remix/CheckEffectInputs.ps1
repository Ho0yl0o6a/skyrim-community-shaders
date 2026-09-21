param(
    [string[]]$Filters = @('WhiteWater', 'Smoke', 'Flames', 'BlacksmithForgeGlow', 'JetBoost', 'fire'),
    [switch]$RequireImports,
    [switch]$RequireImportedAnimation
)
$ErrorActionPreference = 'Stop'
function Read-Effects([string]$Filter) {
    Invoke-RestMethod -Uri 'http://127.0.0.1:8920/api/tool/communityshaders.inspect' -Method Post -ContentType 'application/json' -TimeoutSec 8 -Body (@{kind='remixScene';filter=$Filter}|ConvertTo-Json -Compress)
}
$first = @{}
foreach ($filter in $Filters) { $first[$filter] = Read-Effects $filter }
Start-Sleep -Seconds 2
$results = foreach ($filter in $Filters) {
    $second = Read-Effects $filter
    $previous = @{}
    foreach ($entry in $first[$filter].entries) {
        if ($entry.effect) { $previous[$entry.name + ':' + ($entry.world -join ',')] = $entry }
    }
    $effects = @($second.entries | Where-Object effect)
    $uvChanged = 0
    $colorChanged = 0
    $importedUvChanged = 0
    foreach ($entry in $effects) {
        $key = $entry.name + ':' + ($entry.world -join ',')
        if (!$previous.ContainsKey($key)) { continue }
        if (($entry.effect.uvTransform -join ',') -ne ($previous[$key].effect.uvTransform -join ',')) { ++$uvChanged }
        if (($entry.effect.baseColor -join ',') -ne ($previous[$key].effect.baseColor -join ',')) { ++$colorChanged }
        if ($entry.submitted -and $entry.effectParameters -and $previous[$key].effectParameters -and
            (($entry.effectParameters[0..3] -join ',') -ne ($previous[$key].effectParameters[0..3] -join ','))) { ++$importedUvChanged }
    }
    [ordered]@{
        filter = $filter
        totalMatches = $second.matched
        sampledEffects = $effects.Count
        triangleShapes = @($effects | Where-Object shape).Count
        otherGeometry = @($effects | Where-Object { !$_.shape }).Count
        residentSources = @($effects | Where-Object { $_.effect.source.renderer }).Count
        authoredPalettes = @($effects | Where-Object { $_.effect.palettePath -and $_.effect.palette.renderer }).Count
        uvAnimated = $uvChanged
        colorAnimated = $colorChanged
        submitted = @($effects | Where-Object submitted).Count
        importedUvAnimated = $importedUvChanged
        sources = @($effects.effect.sourcePath | Sort-Object -Unique)
        palettes = @($effects.effect.palettePath | Where-Object { $_ } | Sort-Object -Unique)
    }
}
if ($RequireImports -and ($results.submitted | Measure-Object -Sum).Sum -eq 0) { throw 'No effect materials submitted in the requested sample.' }
if ($RequireImportedAnimation -and ($results.importedUvAnimated | Measure-Object -Sum).Sum -eq 0) { throw 'No imported effect UVs changed; verify the game is unpaused.' }
[ordered]@{
    groups = @($results)
    note = 'Native input inventory, at most 48 nearest matches per filter; overlapping groups are not additive. Not an effect rendering or particle animation test.'
} | ConvertTo-Json -Depth 6

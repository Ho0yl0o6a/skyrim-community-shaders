param([string]$Filter = 'Plane03:0', [switch]$RequireImports, [switch]$RequireImportedAnimation)
$ErrorActionPreference = 'Stop'
function Read-Refraction {
    $body = @{kind='remixScene';filter=$Filter} | ConvertTo-Json -Compress
    $sample = Invoke-RestMethod -Uri 'http://127.0.0.1:8920/api/tool/communityshaders.inspect' -Method Post -ContentType 'application/json' -Body $body -TimeoutSec 10
    @($sample.entries | Where-Object { $null -ne $_.refraction })
}
function Get-Key($Entry) { "$($Entry.name)|$($Entry.world -join ',')" }
$before = @{}
foreach ($entry in (Read-Refraction)) { $before[(Get-Key $entry)] = $entry }
if (!$before.Count) { throw "No native refraction meshes matched '$Filter'." }
Start-Sleep -Seconds 2
$rows = foreach ($entry in (Read-Refraction)) {
    $previous = $before[(Get-Key $entry)]
    if (!$previous) { continue }
    [ordered]@{
        name = $entry.name
        world = $entry.world
        submitted = $entry.submitted
        power = $entry.refraction.power
        fade = $entry.refraction.fade
        activeUVIndex = $entry.refraction.activeUVIndex
        imported = ($null -ne $entry.effectParameters -and (([int]$entry.effectParameters[19] -band 512) -ne 0))
        importedStrengthMatches = ($null -ne $entry.effectParameters -and
            [Math]::Abs($entry.effectParameters[8] - $entry.refraction.power) -lt 0.00001 -and
            [Math]::Abs($entry.effectParameters[13] - $entry.refraction.fade) -lt 0.00001)
        importedUVChanged = ($null -ne $previous.effectParameters -and $null -ne $entry.effectParameters -and
            (($previous.effectParameters[0..3] | ConvertTo-Json -Compress) -ne ($entry.effectParameters[0..3] | ConvertTo-Json -Compress)))
        diffuse = $entry.refraction.diffusePath
        normal = $entry.refraction.normalPath
        uvChanged = (($previous.refraction.uvTransforms | ConvertTo-Json -Compress) -ne ($entry.refraction.uvTransforms | ConvertTo-Json -Compress))
        uvTransforms = $entry.refraction.uvTransforms
    }
}
[ordered]@{
    matchedStableSamples = @($rows).Count
    animatedSamples = @($rows | Where-Object uvChanged).Count
    importedSamples = @($rows | Where-Object imported).Count
    importedAnimatedSamples = @($rows | Where-Object importedUVChanged).Count
    entries = @($rows)
    scope = 'Read-only native and imported refraction inputs. Does not establish GPU animation or image parity.'
} | ConvertTo-Json -Depth 6
if (!@($rows).Count) { throw 'No stable refraction samples remained for comparison.' }
if ($RequireImports -and @($rows | Where-Object { !$_.submitted -or !$_.imported -or !$_.importedStrengthMatches }).Count) {
    throw 'A refraction mesh is missing or its imported strength differs from the native input.'
}
if ($RequireImportedAnimation -and !@($rows | Where-Object importedUVChanged).Count) {
    throw 'No imported refraction UV animation was observed.'
}

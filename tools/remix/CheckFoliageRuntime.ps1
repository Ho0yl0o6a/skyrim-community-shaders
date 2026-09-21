param(
    [string]$ReportPath = (Join-Path $PSScriptRoot '../../.research/testlogs/foliage-native-inputs.json'),
    [ValidateSet('Any','Gamma','Linear')][string]$ExpectedColorSpace = 'Any',
    # Scene-specific assertion: assets without normals legitimately use the fallback.
    [ValidateSet('Any','Authored','Face')][string]$ExpectedGrassNormals = 'Any'
)
$ErrorActionPreference = 'Stop'
function Invoke-FoliageCheck([string]$Tool, [hashtable]$Body) {
    Invoke-RestMethod -Uri "http://127.0.0.1:8920/api/tool/$Tool" -Method Post -ContentType application/json -Body ($Body | ConvertTo-Json -Compress) -TimeoutSec 20
}
$state = Invoke-FoliageCheck 'inspect' @{kind='state'}
if (!$state.playerLoaded) { throw 'A loaded world is required.' }
$grass = Invoke-FoliageCheck 'communityshaders.inspect' @{kind='remixScene';filter='BSGrassShaderProperty'}
$trees = Invoke-FoliageCheck 'communityshaders.inspect' @{kind='remixScene';filter='Pine'}
$liveGrass = @($grass.entries | Where-Object {$_.submitted})
$nativeLeaves = @($trees.entries | Where-Object {$_.submitted -and $_.nativeFoliage -and $_.foliageSoftTexture})
$bark = @($trees.entries | Where-Object {$_.submitted -and $_.lightingMaterial.diffusePath -match 'Bark'})
$failures = @()
if (!$liveGrass.Count) { $failures += 'No submitted grass in inspector sample.' }
if (!$nativeLeaves.Count) { $failures += 'No submitted native soft-light leaf material in inspector sample.' }
if (!$bark.Count) { $failures += 'No submitted bark control in inspector sample.' }
foreach ($entry in $liveGrass) {
    if (!$entry.nativeFoliage -or !$entry.thinFoliage -or !($entry.foliageFlags -band 1) -or $entry.foliageParameters.Count -ne 6) {
        $failures += "Grass missing native scattering inputs: $($entry.name)"
    }
    if ($ExpectedGrassNormals -ne 'Any') {
        if ($null -eq $entry.useFaceNormals -or
            $entry.useFaceNormals -ne ($ExpectedGrassNormals -eq 'Face')) {
            $failures += "Unexpected grass normal source: $($entry.name), useFaceNormals=$($entry.useFaceNormals)"
        }
        $sphereFromProperty = ([Convert]::ToUInt64($entry.shaderFlags, 16) -band [uint64]0x4000000000000000) -ne 0
        if ((($entry.foliageFlags -band 64) -ne 0) -ne $sphereFromProperty) {
            $failures += "Grass sphere-normal flag differs from kEffectLighting: $($entry.name)"
        }
    }
}
foreach ($entry in $bark) {
    if ($entry.thinFoliage -or $entry.nativeFoliage) { $failures += "Bark incorrectly classified as thin: $($entry.name)" }
}
if ($ExpectedColorSpace -ne 'Any') {
    foreach ($entry in @($liveGrass) + @($nativeLeaves)) {
        $isGamma = ($entry.foliageFlags -band 32) -ne 0
        if ($isGamma -ne ($ExpectedColorSpace -eq 'Gamma')) {
            $failures += "Unexpected native colour domain: $($entry.name), flags=$($entry.foliageFlags)"
        }
    }
}
$report = [ordered]@{
    time = (Get-Date).ToString('o')
    process = $state
    camera = (Invoke-FoliageCheck 'camera' @{action='get'})
    scope = 'Host import census only; nearest inspector entries, not GPU pixel or full-scene fidelity proof.'
    expectedColorSpace = $ExpectedColorSpace
    expectedGrassNormals = $ExpectedGrassNormals
    grassMatched = $grass.matched
    treesMatched = $trees.matched
    submittedGrass = $liveGrass.Count
    authoredLeafControls = $nativeLeaves.Count
    barkControls = $bark.Count
    entries = @(@($liveGrass) + @($nativeLeaves) + @($bark) | Select-Object name,submitted,shaderFlags,thinFoliage,nativeFoliage,useFaceNormals,foliageFlags,foliageParameters,foliageSoftTexture,foliageBackTexture,placements,world)
    failures = $failures
    passed = $failures.Count -eq 0
}
$report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $ReportPath -Encoding utf8
[ordered]@{report=[IO.Path]::GetFullPath($ReportPath);passed=$report.passed;grass=$liveGrass.Count;leaves=$nativeLeaves.Count;bark=$bark.Count;failures=$failures} | ConvertTo-Json -Compress
if (!$report.passed) { throw 'Foliage input census failed; see report.' }

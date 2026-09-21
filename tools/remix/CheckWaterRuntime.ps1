param(
    [string]$Log = 'C:/Users/timpy/OneDrive/Documents/My Games/Skyrim Special Edition/SKSE/CommunityShaders.log',
    [switch]$RequireImports,
    [switch]$RequireAnimation,
    [switch]$RequireFlow,
    [switch]$RequireWading
)
$ErrorActionPreference = 'Stop'
$imports = @(Select-String -LiteralPath $Log -Pattern '\[RemixScene.waterMaterial\].*import=(true|false).*layers=(\d+)')
$failures = @($imports | Where-Object { $_.Matches[0].Groups[1].Value -ne 'true' }).Count
if ($RequireImports -and ($imports.Count -eq 0 -or $failures -ne 0)) {
    throw "Water imports failed: imports=$($imports.Count), failures=$failures"
}
function Read-Water {
    Invoke-RestMethod -Uri http://127.0.0.1:8920/api/tool/communityshaders.inspect -Method Post -ContentType application/json -TimeoutSec 8 -Body '{"kind":"remixScene","filter":"BSWaterShaderProperty"}'
}
$first = Read-Water
$previous = @{}
foreach ($entry in $first.entries) {
    if ($entry.waterParameters) { $previous[$entry.name + ':' + ($entry.world -join ',')] = $entry.waterParameters }
}
Start-Sleep -Seconds 2
$second = Read-Water
$animated = 0
$flowAnimated = 0
foreach ($entry in $second.entries) {
    $key = $entry.name + ':' + ($entry.world -join ',')
    if ($entry.waterParameters -and $previous.ContainsKey($key)) {
        if (($entry.waterParameters[6..11] -join ',') -ne ($previous[$key][6..11] -join ',')) { ++$animated }
        if ($entry.waterFlowUVValid -and $entry.waterParameters[22] -ne 0 -and $entry.waterParameters[23] -ne $previous[$key][23]) { ++$flowAnimated }
    }
}
if ($RequireAnimation -and $animated -eq 0) { throw 'No changing imported water offsets; check that the game is unpaused.' }
$tiles = @(Select-String -LiteralPath $Log -Pattern '\[RemixMaterial.waterFlowTile\] copied ')
$tileErrors = @(Select-String -LiteralPath $Log -Pattern '\[RemixMaterial.waterFlowTile\] owned atlas copy failed')
$uvFits = @(Select-String -LiteralPath $Log -Pattern '\[RemixScene.waterFlowUV\].*affine=(true|false) maxError=([^ ]+)')
$uvRejected = @($uvFits | Where-Object { $_.Matches[0].Groups[1].Value -ne 'true' }).Count
if ($RequireFlow -and ($tiles.Count -eq 0 -or $tileErrors.Count -ne 0 -or $flowAnimated -eq 0 -or $uvRejected -ne 0)) {
    throw "Flow validation incomplete: tiles=$($tiles.Count), errors=$($tileErrors.Count), animated=$flowAnimated, rejectedUV=$uvRejected"
}
$wading = @($second.entries | Where-Object { $_.submitted -and $_.waterFlowUVValid -and $_.waterParameters.Count -ge 26 -and $_.waterParameters[25] -eq 1 })
if ($RequireWading -and $wading.Count -eq 0) { throw 'No submitted wading flow material in the nearest water list.' }
[ordered]@{
    imports = $imports.Count
    failedImports = $failures
    loadedWater = $second.matched
    nearestSubmitted = @($second.entries | Where-Object submitted).Count
    animatedMaterials = $animated
    animatedFlowMaterials = $flowAnimated
    copiedFlowTiles = $tiles.Count
    failedFlowTiles = $tileErrors.Count
    flowUvFits = $uvFits.Count
    rejectedFlowUvFits = $uvRejected
    submittedWadingFlowMaterials = $wading.Count
    note = 'CPU import/animation evidence only; does not validate shader motion, flow maps, water transport, or visual correctness.'
} | ConvertTo-Json

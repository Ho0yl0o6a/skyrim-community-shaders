param([string[]]$Filters = @('OuterGlass01', 'OuterGlass02', 'OrbOuterGlass', 'FarmTable01', 'MaleHead'))
$ErrorActionPreference = 'Stop'
# On-demand GPU-buffer readbacks. No scene changes or saved-setting writes.
$rows = @()
$failures = @()
foreach ($filter in $Filters) {
    $sample = Invoke-RestMethod -Uri 'http://127.0.0.1:8920/api/tool/communityshaders.inspect' -Method Post `
        -ContentType application/json -TimeoutSec 20 -Body (@{kind='remixScene';filter="vertices:$filter"} | ConvertTo-Json -Compress)
    $checked = 0
    foreach ($entry in $sample.entries) {
        if (!$entry.submitted -or !$entry.vertexColors.hasColors) { continue }
        $flags = [Convert]::ToUInt64($entry.shaderFlags, 16)
        if (($flags -band ([uint64]1 -shl 37)) -eq 0) { continue }
        # These channels have separate landscape/foliage/hair semantics.
        if ($entry.materialType -ne 2 -or $entry.feature -in @(6,8,9,12,14,15,18,19) -or
            ($flags -band (([uint64]1 -shl 34) -bor ([uint64]1 -shl 61)))) { continue }
        $native = @($entry.vertexColors.nativeSamplesRGBA)
        $imported = @($entry.vertexColors.uploadedSamplesRGBA)
        if ($native.Count -ne 4 -or $imported.Count -ne 4) { throw "Missing diagnostic samples for $($entry.name)" }
        $mismatches = 0
        for ($i=0; $i -lt 4; $i++) {
            for ($c=0; $c -lt 4; $c++) {
                $expected = [int]$native[$i][$c]
                if ($c -eq 3) {
                    $value = [Math]::Clamp(($expected / 255.0) * $entry.lightingMaterial.alpha, 0.0, 1.0)
                    $expected = [int][Math]::Round($value * 255, [MidpointRounding]::AwayFromZero)
                }
                if ([int]$imported[$i][$c] -ne $expected) { $mismatches++ }
            }
        }
        if ($entry.colorBlend.rgbOperation -ne 3 -or $entry.colorBlend.alphaOperation -ne 3 -or $entry.colorBlend.alphaArg2 -ne 2) {
            $failures += "Incorrect vertex modulation state: $($entry.name)"
        }
        if ($mismatches) { $failures += "Vertex RGBA differs in $mismatches sampled channels: $($entry.name)" }
        $checked++
        $rows += [ordered]@{name=$entry.name;filter=$filter;sampledVertices=4;mismatches=$mismatches;nativeMinRGBA=$entry.vertexColors.nativeMinRGBA}
    }
    if (!$checked) { $failures += "No submitted ordinary vertex-colour meshes checked: $filter" }
}
[ordered]@{checks=$rows;failures=$failures;scope='Four native/uploaded vertex samples and submitted blend state per mesh, not GPU shading or image parity.'} | ConvertTo-Json -Depth 6
if ($failures.Count) { throw ($failures -join '; ') }

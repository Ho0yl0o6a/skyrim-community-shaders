param(
    [string]$Log = 'C:/Users/timpy/OneDrive/Documents/My Games/Skyrim Special Edition/SKSE/CommunityShaders.log',
    [switch]$RequireLayeredImports,
    [switch]$RequireTangentFrames
)
$ErrorActionPreference = 'Stop'
# Read-only validation of the native import boundary. This is not visual proof.
$imports = @(Select-String -LiteralPath $Log -Pattern '\[RemixScene.landMaterial\] six-layer import=(true|false) diffuse=(\d+) normal=(\d+)')
$counts = [ordered]@{}
$failures = 0
$sixLayer = 0
foreach ($entry in $imports) {
    $match = $entry.Matches[0]
    if ($match.Groups[1].Value -ne 'true') { ++$failures }
    $diffuse = [int]$match.Groups[2].Value
    $normal = [int]$match.Groups[3].Value
    $key = "diffuse=$diffuse normal=$normal"
    if (!$counts.Contains($key)) { $counts[$key] = 0 }
    ++$counts[$key]
    if ($diffuse -eq 6 -and $normal -eq 6) { ++$sixLayer }
}
if ($RequireLayeredImports -and ($imports.Count -eq 0 -or $failures -ne 0 -or $sixLayer -eq 0)) {
    throw "Landscape import validation failed: imports=$($imports.Count), failures=$failures, six-layer=$sixLayer"
}
$tangentSample = $null
if ($RequireTangentFrames) {
    $live = Invoke-RestMethod -Uri 'http://127.0.0.1:8920/api/tool/communityshaders.inspect' -Method Post `
        -ContentType application/json -TimeoutSec 10 -Body '{"kind":"remixScene","filter":"feature:19"}'
    $submitted = @($live.entries | Where-Object submitted)
    $missing = @($submitted | Where-Object { $_.nativeTangentFrame -ne $true })
    if (!$submitted.Count -or $missing.Count) {
        throw "Terrain tangent transport failed: submitted sample=$($submitted.Count), missing authored basis=$($missing.Count)."
    }
    $tangentSample = [ordered]@{ matched=$live.matched; submittedSample=$submitted.Count; authoredBasis=$submitted.Count - $missing.Count }
}
[ordered]@{
    imports = $imports.Count
    failedImports = $failures
    sixLayerImports = $sixLayer
    layerCounts = $counts
    tangentSample = $tangentSample
    note = 'Cumulative material imports and bounded live plugin mesh sample only. Not GPU buffer readback or native image parity. Verify weights, blends, normals, coverage, and final appearance separately.'
} | ConvertTo-Json -Depth 5

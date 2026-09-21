param(
    [string]$Log = 'C:/Users/timpy/OneDrive/Documents/My Games/Skyrim Special Edition/SKSE/CommunityShaders.log',
    [switch]$RequireAtlasImports
)
$ErrorActionPreference = 'Stop'
# Read-only cumulative import evidence, not visual or LOD-transition validation.
$imports = @(Select-String -LiteralPath $Log -Pattern '\[RemixScene.treeMaterial\] atlas=''([^'']+)'' import=(true|false) alphaTest=(\d+) threshold=(\d+)')
$failed = 0
$wrongCutout = 0
$atlases = @{}
foreach ($entry in $imports) {
    $match = $entry.Matches[0]
    $atlas = $match.Groups[1].Value
    if (!$atlases.ContainsKey($atlas)) { $atlases[$atlas] = 0 }
    ++$atlases[$atlas]
    if ($match.Groups[2].Value -ne 'true') { ++$failed }
    if ([int]$match.Groups[3].Value -ne 4 -or [int]$match.Groups[4].Value -ne 128) { ++$wrongCutout }
}
if ($RequireAtlasImports -and ($imports.Count -eq 0 -or $failed -ne 0 -or $wrongCutout -ne 0)) {
    throw "Tree atlas validation failed: imports=$($imports.Count), failed=$failed, unexpectedCutout=$wrongCutout"
}
[ordered]@{
    imports = $imports.Count
    failedImports = $failed
    unexpectedRiverwoodCutout = $wrongCutout
    atlases = $atlases
    note = 'Cumulative material-import evidence only. Verify placement, silhouettes, lighting, motion and LOD transitions in game.'
} | ConvertTo-Json -Depth 4

param(
    [string]$RuntimeLog = "$env:USERPROFILE\OneDrive\Documents\My Games\Skyrim Special Edition\SKSE\remix-dxvk.log",
    [switch]$RequireWindProbe,
    [switch]$RequireOpacityReuse
)
$ErrorActionPreference = 'Stop'
# Read-only evidence check. This does not establish visual correctness or prove
# that every loaded asset is present; it checks actual runtime GPU/cache reports.
$lines = Get-Content -LiteralPath $RuntimeLog
$placements = 0L
$allocations = 0
$uniqueTriangles = 0L
$boundTriangles = 0L
$opacityBytes = 0L
$opacityBuilds = 0
$probeSamples = 0
$maxMotion = 0.0
$maxError = 0.0
foreach ($line in $lines) {
    if ($line -match '\[CSRemix\] native grass allocated placements=(\d+) vertices=(\d+) indices=(\d+)') {
        ++$allocations
        $placements += [long]$Matches[1]
        if ([long]$Matches[3] % 3) { throw 'Grass index count is not triangle-aligned.' }
    }
    if ($line -match '\[CSRemix\] grass opacity cache built uniqueTriangles=(\d+) boundTriangles=(\d+) bytes=(\d+)') {
        $unique = [long]$Matches[1]
        $bound = [long]$Matches[2]
        if ($unique -le 0 -or $bound -lt $unique -or $bound % $unique) {
            throw 'Invalid repeated opacity topology reported by runtime.'
        }
        $uniqueTriangles += $unique
        $boundTriangles += $bound
        $opacityBytes += [long]$Matches[3]
        ++$opacityBuilds
    }
    if ($line -match 'grass GPU probe sample=\d+ vertices=\d+ timer=\S+ maxPositionError=(\S+) maxMovement=(\S+) attributeErrors=(\d+)') {
        ++$probeSamples
        $errorValue = [double]::Parse($Matches[1], [Globalization.CultureInfo]::InvariantCulture)
        $motion = [double]::Parse($Matches[2], [Globalization.CultureInfo]::InvariantCulture)
        if (![double]::IsFinite($errorValue) -or $errorValue -gt 0.01 -or [int]$Matches[3] -ne 0) {
            throw 'Grass GPU probe did not preserve expected position/UV/alpha.'
        }
        $maxError = [Math]::Max($maxError, $errorValue)
        $maxMotion = [Math]::Max($maxMotion, $motion)
    }
}
if ($RequireWindProbe -and ($probeSamples -ne 2 -or $maxMotion -le 0)) {
    throw 'Expected two completed GPU wind samples with measured vertex movement.'
}
if ($RequireOpacityReuse -and $opacityBuilds -eq 0) { throw 'No repeated grass opacity maps were built.' }
[ordered]@{
    allocations = $allocations
    placementsAtAllocation = $placements
    windSamples = $probeSamples
    maximumPositionError = $maxError
    maximumMovement = $maxMotion
    opacityBuilds = $opacityBuilds
    uniqueOpacityTriangles = $uniqueTriangles
    expandedOpacityTriangles = $boundTriangles
    opacityCacheBytes = $opacityBytes
    note = 'Counts are cumulative in this log, not a live scene census or a visual correctness assertion.'
} | ConvertTo-Json

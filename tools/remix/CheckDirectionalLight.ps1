param(
    [ValidateRange(2, 100)][int]$Samples = 12,
    [ValidateRange(50, 2000)][int]$IntervalMs = 250
)
$ErrorActionPreference = 'Stop'
function Invoke-DB([string]$Name, [hashtable]$Body) {
    Invoke-RestMethod -Uri "http://127.0.0.1:8920/api/tool/$Name" -Method Post -ContentType application/json -Body ($Body | ConvertTo-Json -Compress) -TimeoutSec 10
}
function Near([double]$Actual, [double]$Expected, [string]$Label) {
    if (![double]::IsFinite($Actual) -or [math]::Abs($Actual - $Expected) -gt 1e-5 * [math]::Max(1.0, [math]::Abs($Expected))) {
        throw "$Label differs: actual=$Actual expected=$Expected"
    }
}
$scene = Invoke-DB 'inspect' @{kind='scene'}
$cellId = [Convert]::ToUInt32($scene.cell.formId.Substring(2), 16)
$rows = @()
for ($sample = 0; $sample -lt $Samples; ++$sample) {
    $result = Invoke-DB 'communityshaders.inspect' @{kind='remixScene';filter='directional'}
    $light = $result.directional
    if (!$light -or !$light.valid) { throw 'Missing or invalid native directional capture.' }
    if ($light.cell -ne $cellId) { throw 'Directional light belongs to a different cell.' }
    if ($light.frameSource -ne 'CS-present' -or $light.capturedFrame -ne $light.submitFrame) {
        throw 'Directional capture is stale or uses an unexpected frame source.'
    }
    $length = [math]::Sqrt(($light.nativeDirection | ForEach-Object { $_ * $_ } | Measure-Object -Sum).Sum)
    if ($length -lt 1e-6) { throw 'Native direction is degenerate.' }
    $positive = $false
    for ($channel = 0; $channel -lt 3; ++$channel) {
        Near $light.direction[$channel] ($light.nativeDirection[$channel] / $length) "Direction[$channel]"
        $source = [math]::Max(0.0, [double]$light.diffuse[$channel]) * [math]::Max(0.0, [double]$light.fade) * [math]::Max(0.0, [double]$light.sunlightScale)
        if (!$light.linearLighting) {
            $expected = [math]::Pow($source, 1.6)
        } elseif ($light.alreadyLinear) {
            $expected = $source / [math]::PI
        } else {
            $multiplier = if ($light.interior) { 1.0 } else { [double]$light.gammaScale }
            $expected = [math]::Pow($source / [math]::Max($multiplier, 1e-5), $light.gamma) * $light.directionalMultiplier * $multiplier
        }
        if ($null -eq $light.radianceScale -or ![double]::IsFinite($light.radianceScale) -or $light.radianceScale -lt 0 -or $light.radianceScale -gt 16) {
            throw 'Missing or invalid directional radiance scale.'
        }
        $expected *= $light.radianceScale
        Near $light.radiance[$channel] $expected "Radiance[$channel]"
        $positive = $positive -or $expected -gt 0
    }
    if ([bool]$light.submitted -ne $positive -or [bool]$light.registered -ne $positive) {
        throw 'Registered/submitted state does not match native light activity.'
    }
    $rows += [ordered]@{frame=$light.submitFrame;cell=$light.cell;interior=$light.interior;
        direction=$light.direction;radiance=$light.radiance;radianceScale=$light.radianceScale;submitted=$light.submitted}
    Start-Sleep -Milliseconds $IntervalMs
}
if ($rows[-1].frame -le $rows[0].frame) { throw 'Present counter did not advance.' }
[ordered]@{passed=$true;cell=$scene.cell.editorId;samples=$rows;
    scope='CPU capture, normalization, intensity conversion, freshness and API submission only; not GPU illumination, shadow or image parity.'} | ConvertTo-Json -Depth 6

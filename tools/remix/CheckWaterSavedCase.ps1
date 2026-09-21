param(
    [string]$Output,
    [switch]$RequireNoHiddenOverlays,
    [switch]$RequireNoRasterOverlays
)
$ErrorActionPreference = 'Stop'
function Read-DB([string]$Name, [string]$Body) {
    Invoke-RestMethod "http://127.0.0.1:8920/api/tool/$Name" -Method Post -ContentType application/json -Body $Body -TimeoutSec 15
}
$scene = Read-DB inspect '{"kind":"scene"}'
$camera = Read-DB camera '{"action":"get"}'
$first = Read-DB communityshaders.inspect '{"kind":"remixScene","filter":"BSWaterShaderProperty"}'
function Get-Key($Entry) {
    # Distant segments can share a name and world origin, but not their bounds.
    "$($Entry.name):$($Entry.world -join ','):$($Entry.boundsMin -join ','):$($Entry.boundsMax -join ','):$($Entry.water.flags)"
}
$previous = @{}
foreach ($entry in $first.entries) { $previous[(Get-Key $entry)] = $entry }
Start-Sleep -Seconds 2
$second = Read-DB communityshaders.inspect '{"kind":"remixScene","filter":"BSWaterShaderProperty"}'
$records = foreach ($entry in $second.entries) {
    $old = $previous[(Get-Key $entry)]
    $wading = ($entry.water.flags -band 1) -ne 0
    [ordered]@{
        name = $entry.name; world = $entry.world; boundsMin = $entry.boundsMin; boundsMax = $entry.boundsMax
        flags = $entry.water.flags; wading = $wading; appCulled = $entry.ancestors[0].appCulled
        submitted = $entry.submitted; matchedPrevious = $null -ne $old
        nativeScrollChanged = ($null -ne $old) -and (($old.water.scroll -join ',') -ne ($entry.water.scroll -join ','))
        importedScrollChanged = ($null -ne $old) -and $entry.waterParameters -and $old.waterParameters -and
            (($old.waterParameters[6..11] -join ',') -ne ($entry.waterParameters[6..11] -join ','))
        flowValid = $entry.waterFlowUVValid
        flowClockChanged = ($null -ne $old) -and $entry.waterParameters -and $old.waterParameters -and
            ($entry.waterParameters[23] -ne $old.waterParameters[23])
    }
}
$hidden = @($records | Where-Object { $_.wading -and $_.appCulled -and $_.submitted })
$overlays = @($records | Where-Object { $_.wading -and $_.submitted })
$result = [ordered]@{
    scene = $scene; cameraBefore = $camera; cameraAfter = (Read-DB camera '{"action":"get"}')
    firstFrame = $first.frame; lastFrame = $second.frame; hiddenWadingSubmitted = $hidden.Count
    rasterWadingSubmitted = $overlays.Count
    records = @($records)
    note = 'Nearest 48 water records only. CPU parameter motion is not proof of GPU or visual animation; hidden-overlay check does not establish water parity.'
}
$json = $result | ConvertTo-Json -Depth 12
if ($Output) { $json | Set-Content -LiteralPath $Output -Encoding utf8 }
$json
if ($RequireNoHiddenOverlays -and $hidden.Count) { throw "$($hidden.Count) hidden wading overlays are still submitted." }
if ($RequireNoRasterOverlays -and $overlays.Count) { throw "$($overlays.Count) stencil-only wading overlays are still submitted as physical surfaces." }

param([string]$Output)
$ErrorActionPreference = 'Stop'
function DB([string]$Name, [hashtable]$Body) {
    Invoke-RestMethod "http://127.0.0.1:8920/api/tool/$Name" -Method Post -ContentType application/json -Body ($Body | ConvertTo-Json -Depth 6 -Compress) -TimeoutSec 15
}
$original = DB inspect @{kind='scene'}
$camera = DB camera @{action='get'}
if ($original.worldspace.editorId -ne 'Tamriel' -or $camera.freeCam) { throw 'Requires the loaded exterior water save, not free camera.' }
$menu = DB menu @{action='list'}
if ($menu.openMenus -contains 'Console' -or $menu.messageBoxOpen) { throw 'Close the modal/Console before sampling live water updates.' }
$samples = [System.Collections.Generic.List[object]]::new()
$route = @(
    @(13628.0,-48004.0,-234.44),
    @(9805.035,-51782.133,-228.644),
    @(10005.035,-51782.133,-228.644),
    @(13665.155,-48229.410,-234.44)
)
try {
    foreach ($point in $route) {
        $null = DB papyrus @{action='call';script='ObjectReference';function='SetPosition';self=@{form='0x14'};args=$point}
        foreach ($pov in @('first','third')) {
            $null = DB camera @{action='setPov';pov=$pov}
            Start-Sleep -Milliseconds 1200
            $water = DB communityshaders.inspect @{kind='remixScene';filter='BSWaterShaderProperty'}
            $overlays = @($water.entries | Where-Object { ($_.water.flags -band 1) -ne 0 })
            $ordinary = @($water.entries | Where-Object { ($_.water.flags -band 1) -eq 0 -and $_.submitted })
            $samples.Add([ordered]@{
                frame=$water.frame; requested=$point; camera=(DB camera @{action='get'}); scene=(DB inspect @{kind='scene'})
                ordinarySubmitted=$ordinary.Count
                overlays=@($overlays | Select-Object name,world,submitted,@{n='appCulled';e={$_.ancestors[0].appCulled}})
            })
        }
    }
} finally {
    $null = DB papyrus @{action='call';script='ObjectReference';function='SetPosition';self=@{form='0x14'};args=$original.position}
    if ($camera.pov -in @('first','third')) { $null = DB camera @{action='setPov';pov=$camera.pov} }
}
$bad = @($samples | Where-Object { @($_.overlays | Where-Object submitted).Count -gt 0 -or $_.ordinarySubmitted -eq 0 })
$active = @($samples | Where-Object { @($_.overlays | Where-Object { -not $_.appCulled }).Count -gt 0 })
$result = [ordered]@{
    passed=($bad.Count -eq 0 -and $active.Count -gt 0 -and $samples.Count -eq 8)
    samplesWithNativeActiveOverlays=$active.Count; badSamples=$bad.Count; samples=$samples
    note='Controlled SetPosition and POV transitions, not walking, animation, fast-travel, or water appearance parity. Requires native-visible overlay samples to exercise the reported recurrence.'
}
$json=$result | ConvertTo-Json -Depth 14
if ($Output) { $json | Set-Content -LiteralPath $Output -Encoding utf8 }
$json
if (-not $result.passed) { throw 'Water overlay movement test failed or did not exercise native-visible overlays.' }

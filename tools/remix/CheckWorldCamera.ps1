param(
    [ValidateSet('first', 'third')][string]$Pov = 'third',
    [ValidateRange(2, 100)][int]$Samples = 12,
    [double]$Tolerance = 0.25
)
$ErrorActionPreference = 'Stop'
function Invoke-DB([string]$Tool, [hashtable]$Body) {
    Invoke-RestMethod -Uri "http://127.0.0.1:8920/api/tool/$Tool" -Method Post `
        -ContentType 'application/json' -Body ($Body | ConvertTo-Json -Compress) -TimeoutSec 8
}
# Parked-camera source regression only. This does NOT qualify camera motion,
# visual jitter, matrix orientation, albedo/depth parity or transition stability.
$state = Invoke-DB 'inspect' @{kind='state'}
if (!$state.playerLoaded) { throw 'Load a test game first.' }
$menu = Invoke-DB 'menu' @{action='list'}
if ($menu.messageBoxOpen -or $menu.openMenus -contains 'Loading Menu') { throw 'A modal/loading screen is open.' }
$camera = Invoke-DB 'camera' @{action='get'}
if ($camera.freeCam) { throw 'Leave free camera before checking player POV.' }
$null = Invoke-DB 'camera' @{action='setPov';pov=$Pov}
Start-Sleep -Milliseconds 700
$rows = @()
for ($i=0; $i -lt $Samples; $i++) {
    $before = Invoke-DB 'camera' @{action='get'}
    $scene = Invoke-DB 'communityshaders.inspect' @{kind='remixScene';filter='camera'}
    $after = Invoke-DB 'camera' @{action='get'}
    if ($before.pov -ne $Pov -or $after.pov -ne $Pov) { throw 'Camera mode changed during sampling.' }
    if (!$scene.submittedCamera.valid -or $scene.submitted -eq 0) { throw 'No submitted Remix world camera/scene.' }
    $eye = $scene.submittedCamera.eyeWorld
    # The three HTTP queries span frames; either bracketing game-camera sample
    # may be closer to the submitted view during idle animation.
    $errors = foreach ($sample in @($before, $after)) {
        [Math]::Sqrt([Math]::Pow($eye[0]-$sample.camX,2) +
            [Math]::Pow($eye[1]-$sample.camY,2) + [Math]::Pow($eye[2]-$sample.camZ,2))
    }
    $error = ($errors | Measure-Object -Minimum).Minimum
    if ($error -gt $Tolerance) { throw "World camera differs from player camera by $error units." }
    $rows += [ordered]@{frame=$scene.frame;error=$error;eye=$eye;submitted=$scene.submitted}
    Start-Sleep -Milliseconds 150
}
$final = Invoke-DB 'inspect' @{kind='state'}
if ($final.pid -ne $state.pid) { throw 'Process changed during sampling.' }
if ($rows[-1].frame -le $rows[0].frame) { throw 'Remix scene frames did not advance.' }
[ordered]@{pov=$Pov;pid=$final.pid;samples=$rows;passed=$true;
    scope='Parked world-camera position source only; not a visual/motion/parity pass.'} | ConvertTo-Json -Depth 5

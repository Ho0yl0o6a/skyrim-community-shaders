param([ValidateRange(1, 10)][int]$Rounds = 2)
$ErrorActionPreference = 'Stop'
function Invoke-DB([string]$Tool, [hashtable]$Body) {
    Invoke-RestMethod -Uri "http://127.0.0.1:8920/api/tool/$Tool" -Method Post `
        -ContentType application/json -Body ($Body | ConvertTo-Json -Depth 5 -Compress) -TimeoutSec 8
}
$initial = Invoke-DB 'inspect' @{kind='state'}
$camera = Invoke-DB 'camera' @{action='get'}
if (!$initial.playerLoaded -or $camera.freeCam) { throw 'Start in a loaded world with free camera off.' }
$menus = Invoke-DB 'menu' @{action='list'}
if ($menus.messageBoxOpen -or $menus.openMenus -contains 'Loading Menu') { throw 'Dismiss modal/loading menus before testing.' }
$rows = @()
for ($round = 0; $round -lt $Rounds; ++$round) {
    foreach ($pov in @('first', 'third')) {
        $null = Invoke-DB 'camera' @{action='setPov';pov=$pov}
        Start-Sleep -Milliseconds 800
        for ($sample = 0; $sample -lt 6; ++$sample) {
            $before = Invoke-DB 'camera' @{action='get'}
            $scene = Invoke-DB 'communityshaders.inspect' @{kind='remixScene';filter='camera'}
            $after = Invoke-DB 'camera' @{action='get'}
            if ($before.pov -ne $pov -or $after.pov -ne $pov) { throw "Expected $pov POV, observed $($before.pov)/$($after.pov) (possibly idle vanity)." }
            if (!$scene.submittedCamera.valid -or !$scene.submitted) { throw 'No world scene/camera.' }
            if ($null -eq $scene.viewModel) { throw 'Build lacks view-model diagnostics.' }
            if ($pov -eq 'first') {
                if (!$scene.viewModel.cameraValid -or !$scene.viewModel.submitted) { throw 'First-person model or current native camera absent.' }
            } elseif ($scene.viewModel.cameraValid -or $scene.viewModel.submitted) {
                throw 'View-model registrations linger in third person.'
            }
            $rows += [ordered]@{round=$round;sample=$sample;pov=$pov;frame=$scene.frame;
                world=$scene.submittedCamera;viewModel=$scene.viewModel}
            Start-Sleep -Milliseconds 100
        }
    }
}
$final = Invoke-DB 'inspect' @{kind='state'}
if ($final.pid -ne $initial.pid -or $rows[-1].frame -le $rows[0].frame) { throw 'Process changed or rendering stopped.' }
[ordered]@{passed=$true;pid=$final.pid;samples=$rows;
    scope='Host camera/category lifetime across POV switches only. Does not prove GPU retirement, appearance, animation or native image parity.'} |
    ConvertTo-Json -Depth 8

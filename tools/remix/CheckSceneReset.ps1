param([ValidateRange(1, 10)][int]$Rounds = 3)
$ErrorActionPreference = 'Stop'
function Invoke-DB([string]$Tool, [hashtable]$Body) {
    Invoke-RestMethod -Uri "http://127.0.0.1:8920/api/tool/$Tool" -Method Post `
        -ContentType application/json -Body ($Body | ConvertTo-Json -Compress) -TimeoutSec 10
}
function Set-Raytracing([string]$Value) {
    $result = Invoke-DB 'communityshaders.remix' @{name='rtx.enableRaytracing';value=$Value}
    if (!$result.success) { throw "Raytracing setting rejected: $Value" }
}
$initial = Invoke-DB 'inspect' @{kind='state'}
$cell = (Invoke-DB 'inspect' @{kind='scene'}).cell.editorId
$render = Invoke-DB 'communityshaders.inspect' @{kind='remixScene';filter='camera'}
if (!$initial.playerLoaded -or !$render.submittedCamera.valid -or !$render.submitted) {
    throw 'Start with a loaded, rendering Remix scene.'
}
$rows = @()
try {
    for ($round=1; $round -le $Rounds; ++$round) {
        $before = $render.frame
        Set-Raytracing 'False'
        Start-Sleep -Milliseconds 500
        Set-Raytracing 'True'
        Start-Sleep -Seconds 2
        $state = Invoke-DB 'inspect' @{kind='state'}
        $render = Invoke-DB 'communityshaders.inspect' @{kind='remixScene';filter='camera'}
        $currentCell = (Invoke-DB 'inspect' @{kind='scene'}).cell.editorId
        if ($state.pid -ne $initial.pid -or $currentCell -ne $cell -or
            !$render.submittedCamera.valid -or !$render.submitted -or $render.frame -le $before) {
            throw "Scene failed to resume after reset $round."
        }
        $rows += [ordered]@{round=$round;before=$before;after=$render.frame;submitted=$render.submitted}
    }
} finally {
    Set-Raytracing 'True'
}
[ordered]@{passed=$true;pid=$initial.pid;cell=$cell;rounds=$rows;
    scope='Live process and host scene recovery after temporary raytracing disable. Verify retainedClear events and runtime ownership log separately; not image parity.'} |
    ConvertTo-Json -Depth 6

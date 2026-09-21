param([Parameter(Mandatory)][string]$Label, [ValidateRange(2,20)][int]$Cycles=6)
$ErrorActionPreference='Stop'
function Invoke-DB($name,$body) {
    Invoke-RestMethod "http://127.0.0.1:8920/api/tool/$name" -Method Post -ContentType application/json -Body ($body|ConvertTo-Json -Compress) -TimeoutSec 8
}
# Exercises origin translation, returning to the same view after each sweep.
# Capture latency is variable: this is a return-to-pose consistency check, not
# a recording of every presented frame or a smooth-motion acceptance test.
$initial=Invoke-DB inspect @{kind='state'}
if(!$initial.playerLoaded) {throw 'A loaded test world is required.'}
$start=& "$PSScriptRoot/Capture.ps1" -Position 17224.77,-47204.45,30 -Pitch 0 -Yaw 0 -Label "$Label-start" -SettleMs 500 | ConvertFrom-Json
$rows=for($cycle=0;$cycle -lt $Cycles;$cycle++) {
    for($step=1;$step -le 32;$step++) {
        $offset=160*[Math]::Sin(2*[Math]::PI*$step/32)
        $null=Invoke-DB camera @{action='drive';x=17224.77+$offset;y=-47204.45;z=30;pitch=0;yaw=0}
        Start-Sleep -Milliseconds 30
    }
    $shot=& "$PSScriptRoot/Capture.ps1" -Label ("$Label-return-{0:d2}" -f $cycle) -SettleMs 80 | ConvertFrom-Json
    $scene=Invoke-DB communityshaders.inspect @{kind='remixScene';filter='camera'}
    if(!$scene.submittedCamera.valid -or $scene.submitted -eq 0) {throw 'Missing submitted scene/camera.'}
    [ordered]@{shot=$shot;frame=$scene.frame;submitted=$scene.submitted;submittedCamera=$scene.submittedCamera}
}
$final=Invoke-DB inspect @{kind='state'}
if($final.pid -ne $initial.pid -or $final.frame -le $initial.frame) {throw 'Process changed or stopped advancing.'}
[ordered]@{pid=$final.pid;start=$start;cycles=$Cycles;rows=@($rows);scope='Camera translation return-to-pose, not continuous presented-frame validation'} | ConvertTo-Json -Depth 7

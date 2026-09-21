param([string]$Label = 'character-motion', [ValidateRange(2,64)][int]$Frames = 64,
    [switch]$FreezeAnimation, [switch]$PoseAudit, [ValidateRange(0,1000)][int]$DebugView = 0,
    [switch]$AllowCameraMotion, [ValidateRange(0,0.2)][double]$YawSweep = 0,
    [double]$CameraX = 13735, [double]$CameraY = -48155, [double]$CameraZ = -158,
    [double]$CameraPitch = 0, [double]$CameraYaw = -2.07738137,
    [ValidateCount(2,2)][double[]]$OrbitCenter)
$ErrorActionPreference = 'Stop'
function Invoke-CharacterTool([string]$Name, [hashtable]$Body) {
    Invoke-RestMethod "http://127.0.0.1:8920/api/tool/$Name" -Method Post -ContentType application/json -Body ($Body | ConvertTo-Json -Compress) -TimeoutSec 10
}
$state = Invoke-CharacterTool inspect @{kind='state'}
if (!$state.playerLoaded -or (Invoke-CharacterTool camera @{action='get'}).freeCam) {
    throw 'Start in a loaded Riverwood Save1 world outside free camera.'
}
$root = 'I:/SteamLibrary/steamapps/common/Skyrim Special Edition/Screenshots'
$pattern = "*-sequence-$($state.pid)"
$before = @(Get-ChildItem -LiteralPath $root -Directory -Filter $pattern | Select-Object -ExpandProperty FullName)
$entered = $false
$auditArmed = $false
$trajectory = @()
try {
    if ($PoseAudit) {
        $existingAudit = Invoke-CharacterTool communityshaders.inspect @{kind='remixScene';filter='characterAudit'}
        if ($existingAudit.enabled -or !$existingAudit.PSObject.Properties['frames']) {
            throw 'Pose audit is already active or this host does not support it.'
        }
    }
    $null = Invoke-CharacterTool camera @{action='setPov';pov='third'}
    Start-Sleep -Milliseconds 500
    if ($FreezeAnimation) {
        $null = Invoke-CharacterTool console @{command='tfc 1'}
    } else {
        $null = Invoke-CharacterTool camera @{action='freecam';on=$true}
    }
    Start-Sleep -Milliseconds 700
    $entered = (Invoke-CharacterTool camera @{action='get'}).freeCam
    if (!$entered) { throw 'Free camera did not engage.' }
    if ($DebugView) {
        if (!(Invoke-CharacterTool communityshaders.remix @{name='rtx.debugView.debugViewIdx';value="$DebugView"}).success) {
            throw 'Debug view rejected.'
        }
    }
    $null = Invoke-CharacterTool camera @{action='drive';x=$CameraX;y=$CameraY;z=$CameraZ;pitch=$CameraPitch;yaw=$CameraYaw}
    Start-Sleep -Milliseconds 1000
    $reference = Invoke-CharacterTool camera @{action='get'}
    if ($PoseAudit) {
        $auditArmed = (Invoke-CharacterTool communityshaders.remix @{name='cs.sceneAudit';value='true'}).success
        if (!$auditArmed) { throw 'Pose audit rejected.' }
    }
    $request = Invoke-CharacterTool communityshaders.capture @{kind='sequence';frames=$Frames}
    if ($request.error) { throw $request.error }
    if ($YawSweep) {
        for ($step = 1; $step -le 24; $step++) {
            $angle = $YawSweep * [Math]::Sin(2 * [Math]::PI * $step / 24)
            $yaw = $reference.camYaw + $angle
            $x = $reference.camX
            $y = $reference.camY
            if ($OrbitCenter) {
                # Skyrim yaw is clockwise from +Y; rotate position and view together.
                $dx = $reference.camX - $OrbitCenter[0]
                $dy = $reference.camY - $OrbitCenter[1]
                $x = $OrbitCenter[0] + [Math]::Cos($angle) * $dx + [Math]::Sin($angle) * $dy
                $y = $OrbitCenter[1] - [Math]::Sin($angle) * $dx + [Math]::Cos($angle) * $dy
            }
            $null = Invoke-CharacterTool camera @{action='drive';x=$x;y=$y;z=$reference.camZ;pitch=$reference.camPitch;yaw=$yaw}
            Start-Sleep -Milliseconds 30
            $trajectory += [ordered]@{step=$step;requested=@{x=$x;y=$y;yaw=$yaw};observed=(Invoke-CharacterTool camera @{action='get'})}
        }
    }
    $deadline = [DateTime]::UtcNow.AddSeconds(55)
    do {
        Start-Sleep -Milliseconds 500
        $directories = @(Get-ChildItem -LiteralPath $root -Directory -Filter $pattern | Where-Object FullName -NotIn $before)
        if ($directories.Count -gt 1) { throw 'Multiple new sequences; refuse ambiguous attribution.' }
        $complete = $directories.Count -eq 1 -and
            @(Get-ChildItem -LiteralPath $directories[0].FullName -Filter '*.png.json').Count -eq $Frames
    } while (!$complete -and [DateTime]::UtcNow -lt $deadline)
    if (!$complete) { throw 'Sequence did not finish; inspect live process/log before retrying.' }
    if ($auditArmed) {
        $null = Invoke-CharacterTool communityshaders.remix @{name='cs.sceneAudit';value='false'}
        $poseRecords = Invoke-CharacterTool communityshaders.inspect @{kind='remixScene';filter='characterAudit'}
    }
    $after = Invoke-CharacterTool camera @{action='get'}
    $stationary = !$YawSweep
    foreach ($field in @('camX','camY','camZ','camPitch','camYaw')) {
        if ([Math]::Abs([double]$reference.$field - [double]$after.$field) -gt .001) {
            $stationary = $false
            if (!$AllowCameraMotion) { throw "Camera moved ($field); reject fixed-camera interpretation." }
        }
    }
    $target = Join-Path $PSScriptRoot "../../.research/sequences/$Label-$([DateTimeOffset]::UtcNow.ToUnixTimeSeconds())"
    $null = New-Item -ItemType Directory -Path $target
    Get-ChildItem -LiteralPath $directories[0].FullName -File | Copy-Item -Destination $target
    if ($PoseAudit) {
        [IO.File]::WriteAllText((Join-Path $target 'pose-audit.json'), ($poseRecords | ConvertTo-Json -Depth 12))
    }
    $manifest = [ordered]@{source=$directories[0].FullName;directory=[IO.Path]::GetFullPath($target);camera=$reference;cameraAfter=$after;cameraStationary=$stationary;yawSweep=$YawSweep;orbitCenter=$OrbitCenter;trajectory=$trajectory;request=$request;freezeRequested=[bool]$FreezeAnimation;debugView=$DebugView}
    $encoded = $manifest | ConvertTo-Json -Depth 7
    [IO.File]::WriteAllText((Join-Path $target 'capture-manifest.json'), $encoded)
    $encoded
} finally {
    try {
        if ($auditArmed) {
            $null = Invoke-CharacterTool communityshaders.remix @{name='cs.sceneAudit';value='false'}
        }
        if ($DebugView) {
            $null = Invoke-CharacterTool communityshaders.remix @{name='rtx.debugView.debugViewIdx';value='0'}
        }
    } finally {
        if ($entered -and (Invoke-CharacterTool camera @{action='get'}).freeCam) {
            if ($FreezeAnimation) {
                $null = Invoke-CharacterTool console @{command='tfc 1'}
            } else {
                $null = Invoke-CharacterTool camera @{action='freecam';on=$false}
            }
        }
    }
}

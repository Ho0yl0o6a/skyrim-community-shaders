param([string]$Label = 'player-movement', [switch]$NoPoseAudit,
    [ValidateSet('first','third')][string]$Pov = 'third', [switch]$Stationary,
    [switch]$ObserveOnly,
    [ValidateSet('None','Draw','Sheathe')][string]$WeaponAction = 'None')
$ErrorActionPreference = 'Stop'
if ($ObserveOnly -and $WeaponAction -ne 'None') { throw 'Observation must not change weapon state.' }
function DB([string]$Name, [hashtable]$Body) {
    Invoke-RestMethod "http://127.0.0.1:8920/api/tool/$Name" -Method Post -ContentType application/json -Body ($Body | ConvertTo-Json -Depth 6 -Compress) -TimeoutSec 15
}
$state = DB inspect @{kind='state'}
if (!$state.playerLoaded -or (DB camera @{action='get'}).freeCam) { throw 'Requires loaded world outside free camera.' }
if ((DB communityshaders.inspect @{kind='remixScene';filter='characterAudit'}).enabled) { throw 'Audit already active.' }
$key = $null
if (!$ObserveOnly) {
    $key = (DB papyrus @{action='call';script='Input';function='GetMappedKey';args=@('Forward',0)}).returned
    if ($null -eq $key -or $key -lt 0) { throw 'Forward key not mapped.' }
}
$root = 'I:/SteamLibrary/steamapps/common/Skyrim Special Edition/Screenshots'
$pattern = "*-sequence-$($state.pid)"
$before = @(Get-ChildItem -LiteralPath $root -Directory -Filter $pattern | Select-Object -ExpandProperty FullName)
if (!$ObserveOnly) { $null = DB camera @{action='setPov';pov=$Pov} }
$cameraBefore = DB camera @{action='get'}
if ($ObserveOnly) { $Pov = $cameraBefore.pov }
if ($cameraBefore.pov -ne $Pov -or $cameraBefore.freeCam) { throw 'Requested camera mode was not established.' }
$sceneBefore = DB inspect @{kind='scene'}
$weaponBefore = DB papyrus @{action='call';script='Actor';function='IsWeaponDrawn';self=@{form='0x14'};args=@()}
$audit = $null
$sequenceFramesObserved = $false
try {
    if (!$ObserveOnly -and !$Stationary) { $null = DB papyrus @{action='call';script='Input';function='HoldKey';args=@($key)} }
    Start-Sleep -Milliseconds 500
    if (!$NoPoseAudit -and !(DB communityshaders.remix @{name='cs.sceneAudit';value='true'}).success) { throw 'Audit rejected.' }
    $request = DB communityshaders.capture @{kind='sequence';frames=64}
    if (!$request.queued) { throw 'Capture rejected.' }
    if ($WeaponAction -ne 'None') {
        $function = if ($WeaponAction -eq 'Draw') { 'DrawWeapon' } else { 'SheatheWeapon' }
        $null = DB papyrus @{action='call';script='Actor';function=$function;self=@{form='0x14'};args=@()}
    }
    # Follow render progress, not a fixed wall-clock guess. Stop before disk
    # encoding can roll the bounded audit history; always release input below.
    $captureDeadline = [DateTime]::UtcNow.AddSeconds(8)
    do {
        Start-Sleep -Milliseconds 100
        $progress = DB inspect @{kind='state'}
        if ($progress.pid -ne $state.pid) { throw 'Game process changed during capture.' }
        $sequenceFramesObserved = $progress.frame -ge ($request.enqueued_at_frame + 65)
    } while (!$sequenceFramesObserved -and [DateTime]::UtcNow -lt $captureDeadline)
} finally {
    if (!$ObserveOnly -and !$Stationary) { $null = DB papyrus @{action='call';script='Input';function='ReleaseKey';args=@($key)} }
    $null = DB communityshaders.remix @{name='cs.sceneAudit';value='false'}
    if (!$NoPoseAudit) { $audit = DB communityshaders.inspect @{kind='remixScene';filter='characterAudit'} }
}
$sceneAfter = DB inspect @{kind='scene'}
$weaponAfter = DB papyrus @{action='call';script='Actor';function='IsWeaponDrawn';self=@{form='0x14'};args=@()}
$deadline = [DateTime]::UtcNow.AddSeconds(50)
do {
    $dirs = @(Get-ChildItem -LiteralPath $root -Directory -Filter $pattern | Where-Object FullName -NotIn $before)
    if ($dirs.Count -gt 1) { throw 'Ambiguous capture directory.' }
    $complete = $dirs.Count -eq 1 -and @(Get-ChildItem -LiteralPath $dirs[0].FullName -Filter '*.png.json').Count -eq 64
    if (!$complete) { Start-Sleep -Milliseconds 500 }
} while (!$complete -and [DateTime]::UtcNow -lt $deadline)
if (!$complete) { throw 'Capture incomplete; inspect process before retrying.' }
$target = Join-Path $PSScriptRoot "../../.research/sequences/$Label-$([DateTimeOffset]::UtcNow.ToUnixTimeSeconds())"
$null = New-Item -ItemType Directory -Path $target
Get-ChildItem -LiteralPath $dirs[0].FullName -File | Copy-Item -Destination $target
if (!$NoPoseAudit) { [IO.File]::WriteAllText((Join-Path $target 'pose-audit.json'), ($audit | ConvertTo-Json -Depth 12)) }
$dx = $sceneAfter.position[0] - $sceneBefore.position[0]
$dy = $sceneAfter.position[1] - $sceneBefore.position[1]
$distance = [Math]::Sqrt($dx * $dx + $dy * $dy)
$cameraAfter = DB camera @{action='get'}
$modeMismatch = $cameraAfter.pov -ne $Pov -or $cameraAfter.freeCam
if (!$NoPoseAudit) {
    $capturedFrames = @(Get-ChildItem -LiteralPath $target -Filter '*.png.json' | ForEach-Object { (Get-Content -LiteralPath $_.FullName -Raw | ConvertFrom-Json).frame })
    $capturedAudit = @($audit.frames | Where-Object { $_.frame -in $capturedFrames })
    $modeMismatch = $modeMismatch -or $capturedAudit.Count -ne $capturedFrames.Count -or
        @($capturedAudit | Where-Object { [bool]$_.viewModelCameraValid -ne ($Pov -eq 'first') }).Count -gt 0
}
$released = $null
if (!$ObserveOnly) { $released = DB papyrus @{action='call';script='Input';function='IsKeyPressed';args=@($key)} }
$manifest = @{request=$request;before=$sceneBefore;after=$sceneAfter;observeOnly=[bool]$ObserveOnly;requestedPov=$Pov;cameraBefore=$cameraBefore;cameraModeMismatch=$modeMismatch;stationaryRequested=[bool]$Stationary;poseAudit=(!$NoPoseAudit);horizontalDistance=$distance;key=$key;released=$released;camera=$cameraAfter;scope='Controlled movement/hold or passive observation as specified; rendered frames and optional host player inputs, not display pacing or GPU pose proof.'}
$manifest.weaponAction = $WeaponAction
$manifest.weaponBefore = $weaponBefore
$manifest.weaponAfter = $weaponAfter
$manifest.sequenceFramesObserved = $sequenceFramesObserved
[IO.File]::WriteAllText((Join-Path $target 'movement-manifest.json'), ($manifest | ConvertTo-Json -Depth 12))
[IO.Path]::GetFullPath($target)
if (!$sequenceFramesObserved) { throw 'Capture frame-progress deadline exceeded; audit coverage not established.' }
if ($WeaponAction -ne 'None') {
    $wantDrawn = $WeaponAction -eq 'Draw'
    if (!$weaponBefore.called -or !$weaponAfter.called -or
        $weaponBefore.returned -eq $wantDrawn -or $weaponAfter.returned -ne $wantDrawn) {
        throw 'Requested weapon state transition not observed; retain as diagnostic evidence only.'
    }
}
if (!$ObserveOnly) {
    if ($modeMismatch) { throw 'Camera mode changed or current-mode coverage is incomplete; retain as diagnostic evidence only.' }
    if (!$Stationary -and $distance -lt 1) { throw 'No horizontal locomotion observed; retain as idle evidence only.' }
    if ($Stationary -and $distance -gt 1) { throw 'Player moved during requested stationary capture.' }
}

param(
    [string]$Label = 'foliage-transmission-tint',
    [ValidateCount(3,3)][double[]]$Position,
    [double]$Pitch = 0,
    [double]$Yaw = 0
)
$ErrorActionPreference = 'Stop'
function Invoke-DB([string]$Name, [hashtable]$Body) {
    Invoke-RestMethod -Uri "http://127.0.0.1:8920/api/tool/$Name" -Method Post -ContentType application/json -Body ($Body | ConvertTo-Json -Compress) -TimeoutSec 10
}
function Set-Transmission([bool]$Diffuse) {
    if (!(Invoke-DB 'communityshaders.remix' @{name='rtx.debugView.foliageDiffuseTransmission';value=$Diffuse.ToString().ToLowerInvariant()}).success) {
        throw 'Native foliage comparison option unavailable.'
    }
}
if ((Invoke-DB 'camera' @{action='get'}).freeCam) { throw 'Start outside free camera for a known freeze state.' }
$frozen = $false
try {
    $null = Invoke-DB 'console' @{command='tfc 1'}
    Start-Sleep -Milliseconds 700
    $frozen = (Invoke-DB 'camera' @{action='get'}).freeCam
    if (!$frozen) { throw 'Freeze camera did not engage.' }
    if ($Position) {
        $null = Invoke-DB 'camera' @{action='drive';x=$Position[0];y=$Position[1];z=$Position[2];pitch=$Pitch;yaw=$Yaw}
    }
    foreach ($sample in @(@($false,'sss-tint'), @($true,'diffuse-times-sss'), @($false,'sss-tint-restored'))) {
        Set-Transmission $sample[0]
        & "$PSScriptRoot/Capture.ps1" -Label "$Label-$($sample[1])" -SettleMs 4000
    }
} finally {
    try { Set-Transmission $false } catch { Write-Warning $_ }
    if ($frozen -and (Invoke-DB 'camera' @{action='get'}).freeCam) {
        $null = Invoke-DB 'console' @{command='tfc 1'}
        Start-Sleep -Milliseconds 700
    }
    $camera = Invoke-DB 'camera' @{action='get'}
    if ($camera.freeCam) { throw 'Free camera remains active; inspect before toggling again.' }
    $camera | ConvertTo-Json -Compress
}

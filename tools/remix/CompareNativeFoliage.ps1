param(
    [string]$Label = 'native-foliage-response',
    [double[]]$Position = @(15133, -46900, 650),
    [double]$Pitch = -0.15,
    [double]$Yaw = 0
)
$ErrorActionPreference = 'Stop'
# Process-local test; never persists graphics settings or a game save.
function Invoke-DB([string]$Name, [hashtable]$Body) {
    Invoke-RestMethod -Uri "http://127.0.0.1:8920/api/tool/$Name" -Method Post -ContentType 'application/json' -Body ($Body | ConvertTo-Json -Compress) -TimeoutSec 10
}
function Set-Option([string]$Name, [string]$Value) {
    $result = Invoke-DB 'communityshaders.remix' @{name=$Name;value=$Value}
    if (!$result.success) { throw "Rejected $Name=$Value" }
}
$menu = Invoke-DB 'menu' @{action='list'}
if ($menu.messageBoxOpen -or $menu.openMenus -notcontains 'HUD Menu') { throw 'Loaded HUD without a modal required.' }
if ((Invoke-DB 'camera' @{action='get'}).freeCam) { throw 'Start outside free camera for a known freeze state.' }
$frozen = $false
try {
    $null = Invoke-DB 'console' @{command='tfc 1'}
    $frozen = $true
    Start-Sleep -Milliseconds 700
    if (!(Invoke-DB 'camera' @{action='get'}).freeCam) { throw 'Free camera did not engage.' }
    Set-Option 'rtx.subsurface.enableTextureMaps' 'true'
    Set-Option 'rtx.debugView.debugViewIdx' '0'
    foreach ($sample in @(@('true','on'), @('false','off'), @('true','restored'))) {
        Set-Option 'rtx.subsurface.enableThinOpaque' $sample[0]
        & "$PSScriptRoot/Capture.ps1" -Position $Position -Pitch $Pitch -Yaw $Yaw -Label "$Label-$($sample[1])" -SettleMs 4000
    }
} finally {
    try { Set-Option 'rtx.subsurface.enableThinOpaque' 'true' } catch { Write-Warning $_ }
    if ($frozen) {
        $null = Invoke-DB 'console' @{command='tfc 1'}
        Start-Sleep -Milliseconds 700
        if ((Invoke-DB 'camera' @{action='get'}).freeCam) { throw 'Free camera did not disengage; inspect before toggling again.' }
    }
}

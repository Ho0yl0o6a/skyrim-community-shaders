param(
    [double[]]$Position,
    [double]$Pitch = 0.65,
    [double]$Yaw = 0.0,
    [switch]$NoFreecam,
    [string]$OutDir = 'J:/hdresreach/skyrim-community-shaders/.research/captures',
    [string]$Label = 'capture',
    [int]$SettleMs = 1500
)
$ErrorActionPreference = 'Stop'
# Drives the running test process only: optionally positions the free camera,
# triggers the lossless CS screenshot, and copies the new file next to the work
# notes. It never deletes game files and never writes saved settings.
function Invoke-DB([string]$Name, [string]$Body) {
    Invoke-RestMethod -Uri "http://127.0.0.1:8920/api/tool/$Name" -Method Post `
        -ContentType 'application/json' -Body $Body -TimeoutSec 20
}
if ($Position) {
    if (!$NoFreecam) {
        $state = Invoke-DB 'camera' '{"action":"get"}'
        if (!$state.freeCam) {
            $null = Invoke-DB 'camera' '{"action":"freecam","on":true}'
            $deadline = [DateTime]::UtcNow.AddSeconds(10)
            do { Start-Sleep -Milliseconds 300; $state = Invoke-DB 'camera' '{"action":"get"}' }
            while (!$state.freeCam -and [DateTime]::UtcNow -lt $deadline)
            if (!$state.freeCam) { throw 'Free camera did not engage.' }
        }
    }
    $drive = @{ action = 'drive'; x = $Position[0]; y = $Position[1]; z = $Position[2]; pitch = $Pitch; yaw = $Yaw } | ConvertTo-Json -Compress
    $null = Invoke-DB 'camera' $drive
}
Start-Sleep -Milliseconds $SettleMs
$before = (Invoke-DB 'inspect' '{"kind":"screenshots","limit":1}').screenshots
$null = Invoke-DB 'communityshaders.capture' '{"kind":"screenshot"}'
$deadline = [DateTime]::UtcNow.AddSeconds(25)
do {
    Start-Sleep -Milliseconds 500
    $latest = (Invoke-DB 'inspect' '{"kind":"screenshots","limit":1}').screenshots
} while (($latest.path -eq $before.path) -and [DateTime]::UtcNow -lt $deadline)
if ($latest.path -eq $before.path) { throw 'No new screenshot appeared.' }
$null = New-Item -ItemType Directory -Force -Path $OutDir
$target = Join-Path $OutDir ('{0}-{1}.png' -f (Get-Date -Format 'HHmmss'), $Label)
Copy-Item -LiteralPath $latest.path -Destination $target -Force
[ordered]@{ source = $latest.path; copy = $target; bytes = $latest.bytes; camera = (Invoke-DB 'camera' '{"action":"get"}') } | ConvertTo-Json -Compress

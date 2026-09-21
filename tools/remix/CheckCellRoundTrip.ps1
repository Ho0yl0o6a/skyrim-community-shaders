param(
    [ValidateRange(1, 20)][int]$Rounds = 3,
    [ValidateRange(5, 120)][int]$TimeoutSeconds = 40,
    [ValidateSet('RiverwoodSleepingGiantInn', 'RiverwoodRiverwoodTrader')]
    [string[]]$Interiors = @('RiverwoodSleepingGiantInn'),
    [ValidateSet('first', 'third')][string]$Pov = 'third',
    # Diagnostic control only; one-way until game restart. Not a Remix pass.
    [switch]$NativeReference
)
$ErrorActionPreference = 'Stop'
function Invoke-DB([string]$Tool, [hashtable]$Body) {
    Invoke-RestMethod -Uri "http://127.0.0.1:8920/api/tool/$Tool" -Method Post `
        -ContentType application/json -Body ($Body | ConvertTo-Json -Compress) -TimeoutSec 5
}
# Cell-load stability only: console teleport does not test walking, door
# activation, fast travel, image correctness or frame pacing.
$initial = Invoke-DB 'inspect' @{kind='state'}
$scene = Invoke-DB 'inspect' @{kind='scene'}
$camera = Invoke-DB 'camera' @{action='get'}
if (!$initial.playerLoaded -or $scene.cell.editorId -ne 'Riverwood' -or $camera.freeCam) {
    throw 'Start loaded in Riverwood with free camera off.'
}
$rows = @()
if ($NativeReference) {
    $reference = Invoke-DB 'communityshaders.remix' @{name='cs.nativeReference';value='true'}
    if (!$reference.success) { throw 'Native reference mode was rejected.' }
}
$route = @($Interiors | ForEach-Object { $_; 'Riverwood' })
for ($round=1; $round -le $Rounds; $round++) {
    foreach ($cell in $route) {
        $null = Invoke-DB 'camera' @{action='setPov';pov=$Pov}
        # Match an interactive console command: pause via the actual console
        # before coc tears down cell lights/pass lists. Dispatching coc from an
        # SKSE task into an unpaused world also crashes the native control.
        $null = Invoke-DB 'menu' @{action='open';name='Console'}
        $consoleDeadline = [DateTime]::UtcNow.AddSeconds(10)
        do {
            Start-Sleep -Milliseconds 100
            $menus = Invoke-DB 'menu' @{action='list'}
        } while ($menus.openMenus -notcontains 'Console' -and [DateTime]::UtcNow -lt $consoleDeadline)
        if ($menus.openMenus -notcontains 'Console') { throw 'Console did not open before coc.' }
        $null = Invoke-DB 'console' @{command="coc $cell"}
        $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
        $settled = 0
        $firstFrame = $null
        do {
            Start-Sleep -Milliseconds 500
            if (!(Get-Process -Id $initial.pid -ErrorAction SilentlyContinue)) {
                throw "Process $($initial.pid) exited during round $round loading $cell."
            }
            try {
                $state = Invoke-DB 'inspect' @{kind='state'}
                $scene = Invoke-DB 'inspect' @{kind='scene'}
                $menus = Invoke-DB 'menu' @{action='list'}
            } catch {
                # A live process can temporarily stop pumping during a load.
                # Retry this same process until the bounded deadline.
                continue
            }
            if ($state.pid -ne $initial.pid) { throw 'The answering game process changed.' }
            if ($state.playerLoaded -and $scene.cell.editorId -eq $cell -and
                !$menus.messageBoxOpen -and $menus.openMenus -notcontains 'Loading Menu') {
                if ($menus.openMenus -contains 'Console') {
                    $null = Invoke-DB 'menu' @{action='close';name='Console'}
                    $settled = 0
                    $firstFrame = $null
                    continue
                }
                $render = if ($NativeReference) {
                    [pscustomobject]@{frame=$state.frame;submitted=0}
                } else {
                    Invoke-DB 'communityshaders.inspect' @{kind='remixScene';filter='camera'}
                }
                if ($NativeReference -or ($render.submittedCamera.valid -and $render.submitted -gt 0)) {
                    if ($null -eq $firstFrame) { $firstFrame = $render.frame }
                    $settled++
                } else { $settled = 0 }
            } else { $settled = 0; $firstFrame = $null }
        } while ($settled -lt 6 -and [DateTime]::UtcNow -lt $deadline)
        if ($settled -lt 6 -or $render.frame -le $firstFrame) {
            throw "Round $round failed to reach a live Remix scene in $cell."
        }
        $camera = Invoke-DB 'camera' @{action='get'}
        if ($camera.freeCam -or $camera.pov -ne $Pov) {
            throw "Camera did not remain $Pov-person in $cell (actual $($camera.pov))."
        }
        if (!$NativeReference -and $Pov -eq 'first' -and !$render.viewModel.cameraValid) {
            throw "First-person native camera was not captured in $cell."
        }
        $membership = $null
        if (!$NativeReference -and $cell -ne 'Riverwood') {
            $membership = & (Join-Path $PSScriptRoot 'CheckInteriorMembership.ps1') -ExpectedCell $cell | ConvertFrom-Json
        }
        $rows += [ordered]@{round=$round;cell=$cell;pid=$state.pid;
            startFrame=$firstFrame;endFrame=$render.frame;submitted=$render.submitted;pov=$camera.pov;
            membership=$membership;viewModel=$render.viewModel}
        Write-Host "Round $round loaded $cell in $Pov person, frame $($render.frame), native reference=$NativeReference."
    }
}
[ordered]@{passed=$true;nativeReference=[bool]$NativeReference;transitions=$rows;
    scope='Console cell-load stability only; not locomotion, door/fast-travel, visual correctness or pacing.'} |
    ConvertTo-Json -Depth 8

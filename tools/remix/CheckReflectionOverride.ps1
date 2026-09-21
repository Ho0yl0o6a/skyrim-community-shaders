param()
$ErrorActionPreference = 'Stop'
function Invoke-DB([string]$Name, [hashtable]$Body) {
    Invoke-RestMethod "http://127.0.0.1:8920/api/tool/$Name" -Method Post -ContentType application/json -TimeoutSec 10 -Body ($Body | ConvertTo-Json -Compress)
}
function Read-Setting([string]$Name) {
    $result = Invoke-DB papyrus @{action='call';script='Utility';function='GetINIBool';args=@($Name)}
    if (!$result.called -or $result.returnedType -ne 'bool') { throw "Cannot read $Name" }
    return [bool]$result.returned
}
function Write-Setting([string]$Name, [bool]$Value) {
    $result = Invoke-DB papyrus @{action='call';script='Utility';function='SetINIBool';args=@($Name,$Value)}
    if (!$result.called) { throw "Cannot write $Name" }
}
$scene = Invoke-DB inspect @{kind='scene'}
if (!$scene.playerLoaded -or $scene.cell.editorId -ne 'RiverwoodSleepingGiantInn') {
    throw 'Run in the loaded Sleeping Giant Inn with Remix Scene enabled.'
}
$expected = [ordered]@{
    'bReflectLODLand:Water' = $false
    'bReflectLODObjects:Water' = $false
    'bReflectLODTrees:Water' = $false
    'bReflectSky:Water' = $true
}
$original = @{}
$rows = @()
try {
    foreach ($name in $expected.Keys) {
        $original[$name] = Read-Setting $name
        if ($original[$name] -ne $expected[$name]) { throw "Initial override is not active: $name" }
    }
    foreach ($name in $expected.Keys) {
        $before = Invoke-DB 'communityshaders.inspect' @{kind='remixScene';filter='camera'}
        Write-Setting $name (!$expected[$name])
        Start-Sleep -Milliseconds 500
        $actual = Read-Setting $name
        $after = Invoke-DB 'communityshaders.inspect' @{kind='remixScene';filter='camera'}
        $rows += [ordered]@{setting=$name;injected=(!$expected[$name]);actual=$actual;expected=$expected[$name];
            firstFrame=$before.frame;lastFrame=$after.frame;
            passed=($actual -eq $expected[$name] -and $after.frame -gt $before.frame)}
        Write-Setting $name $original[$name]
    }
} finally {
    foreach ($name in $original.Keys) { Write-Setting $name $original[$name] }
}
$passed = @($rows | Where-Object { !$_.passed }).Count -eq 0 -and $rows.Count -eq 4
[ordered]@{passed=$passed;cell=$scene.cell.editorId;settings=$rows;
    scope='Temporary in-memory INI perturbation. No saved INI writes; proves active override reconciliation, not final sky pixels or the reported intermittent interior leak.'} | ConvertTo-Json -Depth 5
if (!$passed) { throw 'Reflection settings were not reconciled while Remix was rendering.' }

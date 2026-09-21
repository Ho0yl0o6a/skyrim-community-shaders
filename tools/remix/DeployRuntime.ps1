param(
    [Parameter(Mandatory = $true)][string]$Label,
    [string]$GameRoot = 'I:/SteamLibrary/steamapps/common/Skyrim Special Edition'
)
$ErrorActionPreference = 'Stop'
# Overwrites only the two Remix runtime DLLs this project builds, after backing
# up the files it replaces. It never deletes anything and never touches files
# outside the Remix runtime directory.
if (Get-Process SkyrimSE -ErrorAction SilentlyContinue) { throw 'Close Skyrim before deploying.' }
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$source = Join-Path $workspace '.research/dxvk-remix/_Comp64Release/src'
$runtime = Join-Path (Resolve-Path -LiteralPath $GameRoot).Path 'Data/SKSE/Plugins/CommunityShaders/bin/Remix'
$stamp = Get-Date -Format 'yyyyMMdd'
$backup = Join-Path $workspace ".research/deployment-backups/$stamp-$Label"
$archive = Join-Path $workspace ".research/deployed-symbols/$stamp-$Label"
$null = New-Item -ItemType Directory -Force -Path $backup, $archive
$built = @(
    @{ From = Join-Path $source 'd3d11/dxvk_d3d11.dll'; Symbols = Join-Path $source 'd3d11/dxvk_d3d11.pdb' },
    @{ From = Join-Path $source 'dxgi/dxvk_dxgi.dll'; Symbols = Join-Path $source 'dxgi/dxvk_dxgi.pdb' }
)
$results = foreach ($item in $built) {
    if (!(Test-Path -LiteralPath $item.From -PathType Leaf)) { throw "Missing runtime artifact: $($item.From)" }
    $target = Join-Path $runtime (Split-Path $item.From -Leaf)
    if (Test-Path -LiteralPath $target) { Copy-Item -LiteralPath $target -Destination $backup -Force }
    Copy-Item -LiteralPath $item.From -Destination $target -Force
    Copy-Item -LiteralPath $item.From -Destination $archive -Force
    if (Test-Path -LiteralPath $item.Symbols) { Copy-Item -LiteralPath $item.Symbols -Destination $archive -Force }
    [ordered]@{ file = $target; sha256 = (Get-FileHash -LiteralPath $target).Hash }
}
[ordered]@{ deployed = $results; backup = $backup; symbols = $archive } | ConvertTo-Json -Depth 4 -Compress

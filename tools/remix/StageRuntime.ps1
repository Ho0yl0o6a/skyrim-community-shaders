param(
    [string]$RuntimeSource = (Join-Path $PSScriptRoot '../../.research/dxvk-remix'),
    [string]$Destination = (Join-Path $PSScriptRoot '../../.research/remix-stage')
)
$ErrorActionPreference = 'Stop'
$RuntimeSource = (Resolve-Path -LiteralPath $RuntimeSource).Path
$Destination = [IO.Path]::GetFullPath($Destination)
# Staging only: explicit input directories, copies only, no mirror or deletion.
$null = New-Item -ItemType Directory -Path $Destination -Force
$dllDirectories = @(
    '_Comp64Release/src/d3d11',
    '_Comp64Release/src/dxgi',
    'external/nv_usd_release/lib',
    'submodules/nrc/bin',
    'submodules/xess/bin',
    'external/nrd/Lib/Release',
    'external/ngx_sdk_dldn/lib/Windows_x86_64/rel',
    'external/ngx_sdk_dlfg/lib/Windows_x86_64/rel',
    'external/reflex/lib',
    'external/aftermath/lib/x64'
)
foreach ($relative in $dllDirectories) {
    $directory = Join-Path $RuntimeSource $relative
    $files = @(Get-ChildItem -LiteralPath $directory -File -Filter '*.dll')
    if (!$files.Count) { throw "No runtime DLLs found in $directory" }
    foreach ($file in $files) { Copy-Item -LiteralPath $file.FullName -Destination $Destination -Force }
}
Copy-Item -LiteralPath (Join-Path $RuntimeSource 'external/nv_usd_release/lib/usd') -Destination $Destination -Recurse -Force
$pluginDir = Join-Path $Destination 'usd/plugins/RemixParticleSystem'
$null = New-Item -ItemType Directory -Path (Join-Path $pluginDir 'resources') -Force
$pluginSource = Join-Path $RuntimeSource '_Comp64Release/src/usd-plugins/RemixParticleSystem'
Copy-Item -LiteralPath (Join-Path $pluginSource 'RemixParticleSystem.dll') -Destination $pluginDir -Force
Copy-Item -LiteralPath (Join-Path $pluginSource 'generatedSchema.usda') -Destination $pluginDir -Force
Copy-Item -LiteralPath (Join-Path $pluginSource 'plugInfo.json') -Destination (Join-Path $pluginDir 'resources') -Force
Copy-Item -LiteralPath (Join-Path $RuntimeSource 'src/usd-plugins/plugInfo.json') -Destination (Join-Path $Destination 'usd/plugins') -Force
Get-Item -LiteralPath (Join-Path $Destination 'dxvk_d3d11.dll'), (Join-Path $Destination 'dxvk_dxgi.dll') | Select-Object FullName,Length

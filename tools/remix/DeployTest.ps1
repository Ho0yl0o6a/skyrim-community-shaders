param([string]$GameRoot = 'I:/SteamLibrary/steamapps/common/Skyrim Special Edition')
$ErrorActionPreference = 'Stop'
if (Get-Process SkyrimSE -ErrorAction SilentlyContinue) { throw 'Close Skyrim before deploying.' }
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$GameRoot = (Resolve-Path -LiteralPath $GameRoot).Path
if (!(Test-Path -LiteralPath (Join-Path $GameRoot 'skse64_loader.exe'))) { throw 'Not an SKSE game directory.' }
$stage = Join-Path $workspace '.research/remix-stage'
$plugin = Join-Path $GameRoot 'Data/SKSE/Plugins/CommunityShaders.dll'
$runtime = Join-Path $GameRoot 'Data/SKSE/Plugins/CommunityShaders/bin/Remix'
$sourceDll = Join-Path $workspace 'build/Dev-Fast/CommunityShaders.dll'
foreach ($required in @($sourceDll, (Join-Path $stage 'dxvk_d3d11.dll'), (Join-Path $stage 'dxvk_dxgi.dll'))) {
    if (!(Test-Path -LiteralPath $required -PathType Leaf)) { throw "Missing build artifact: $required" }
}
$backup = Join-Path $workspace ('.research/deployment-backups/' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
$null = New-Item -ItemType Directory -Path $backup -Force
if (Test-Path -LiteralPath $plugin) { Copy-Item -LiteralPath $plugin -Destination $backup }
# Copy individually, never use mirror/delete. Preserve existing runtime files when overwriting.
foreach ($file in Get-ChildItem -LiteralPath $stage -Recurse -File) {
    $relative = [IO.Path]::GetRelativePath($stage, $file.FullName)
    $target = [IO.Path]::GetFullPath((Join-Path $runtime $relative))
    if (!$target.StartsWith($runtime + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Invalid staged path: $relative"
    }
    if (Test-Path -LiteralPath $target) {
        $saved = Join-Path $backup ('Remix/' + $relative)
        $null = New-Item -ItemType Directory -Path (Split-Path $saved -Parent) -Force
        Copy-Item -LiteralPath $target -Destination $saved
    }
    $null = New-Item -ItemType Directory -Path (Split-Path $target -Parent) -Force
    Copy-Item -LiteralPath $file.FullName -Destination $target -Force
}
Copy-Item -LiteralPath $sourceDll -Destination $plugin -Force
Write-Output "Backup: $backup"
Get-FileHash -LiteralPath $plugin, (Join-Path $runtime 'dxvk_d3d11.dll'), (Join-Path $runtime 'dxvk_dxgi.dll')

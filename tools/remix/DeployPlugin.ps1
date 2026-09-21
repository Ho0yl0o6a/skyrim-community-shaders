param(
    [Parameter(Mandatory = $true)][string]$Label,
    # Which build to ship. Release is the optimised artifact BuildDev.bat and
    # BuildRelease.bat produce; Dev-Fast is the /Od build from BuildDevFast.bat,
    # which is for iterating on correctness and not for measuring. This script
    # took Dev-Fast unconditionally, so a benchmark could silently be measuring
    # unoptimised plugin code -- and was.
    [ValidateSet('Release', 'Dev-Fast')][string]$Build = 'Release',
    [string]$GameRoot = 'I:/SteamLibrary/steamapps/common/Skyrim Special Edition'
)
$ErrorActionPreference = 'Stop'
# Overwrites only the Community Shaders plugin DLL, after backing up the file it
# replaces. It never touches runtime DLLs or any file the project does not own,
# and never deletes anything.
if (Get-Process SkyrimSE -ErrorAction SilentlyContinue) { throw 'Close Skyrim before deploying.' }
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$buildDirectory = if ($Build -eq 'Release') { 'build/ALL/Release' } else { 'build/Dev-Fast' }
$source = Join-Path $workspace "$buildDirectory/CommunityShaders.dll"
$symbols = Join-Path $workspace "$buildDirectory/CommunityShaders.pdb"
if (!(Test-Path -LiteralPath $source -PathType Leaf)) { throw "Missing build artifact: $source (build it, or pass -Build to select the other one)" }
$plugin = Join-Path (Resolve-Path -LiteralPath $GameRoot).Path 'Data/SKSE/Plugins/CommunityShaders.dll'
$stamp = Get-Date -Format 'yyyyMMdd'
$backup = Join-Path $workspace ".research/deployment-backups/$stamp-$Label"
$archive = Join-Path $workspace ".research/deployed-symbols/$stamp-$Label"
$null = New-Item -ItemType Directory -Force -Path $backup, $archive
if (Test-Path -LiteralPath $plugin) { Copy-Item -LiteralPath $plugin -Destination $backup -Force }
Copy-Item -LiteralPath $source -Destination $plugin -Force
Copy-Item -LiteralPath $source, $symbols -Destination $archive -Force
[ordered]@{ plugin = $plugin; build = $Build; builtAt = (Get-Item -LiteralPath $source).LastWriteTime; sha256 = (Get-FileHash -LiteralPath $plugin).Hash; backup = $backup; symbols = $archive } | ConvertTo-Json -Compress

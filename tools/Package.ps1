param([switch]$Diagnostics)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$variant = if ($Diagnostics) { 'Diagnostics' } else { 'Release' }
$binarySubdir = if ($Diagnostics) { 'build\diagnostics\windows\x64\releasedbg' } else { 'build\windows\x64\releasedbg' }
$binary = Join-Path $projectRoot ($binarySubdir + '\GhostTrails.dll')
$script = Join-Path $projectRoot 'dist\Scripts\GhostTrails.pex'
if (!(Test-Path -LiteralPath $binary) -or !(Test-Path -LiteralPath $script)) {
    throw 'Build the selected DLL variant and compile GhostTrails.psc with houseCARL first.'
}
$stage = Join-Path $projectRoot ('dist\' + $variant)
$plugins = Join-Path $stage 'SKSE\Plugins'
$scripts = Join-Path $stage 'Scripts'
$sources = Join-Path $stage 'Source\Scripts'
$sdk = Join-Path $stage 'SDK'
$docs = Join-Path $stage 'docs'
$null = New-Item -ItemType Directory -Force -Path $plugins,$scripts,$sources,$sdk,$docs
Copy-Item -LiteralPath $binary -Destination (Join-Path $plugins 'GhostTrails.dll')
Copy-Item -LiteralPath (Join-Path $projectRoot 'config\GhostTrails.ini') -Destination $plugins
Copy-Item -LiteralPath $script -Destination $scripts
Copy-Item -LiteralPath (Join-Path $projectRoot 'scripts\Source\GhostTrails.psc') -Destination $sources
Copy-Item -LiteralPath (Join-Path $projectRoot 'include\GhostTrailsAPI.h') -Destination $sdk
Copy-Item -LiteralPath (Join-Path $projectRoot 'README.md') -Destination $stage
Copy-Item -LiteralPath (Join-Path $projectRoot 'docs\API.md') -Destination $sdk
Copy-Item -Path (Join-Path $projectRoot 'docs\*.md') -Destination $docs
Copy-Item -LiteralPath (Join-Path $projectRoot 'examples\NativeConsumer.ixx') -Destination $sdk
$archive = Join-Path $projectRoot ('dist\GhostTrails-1.0.1-' + $variant + '.zip')
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $archive -Force
Get-Item -LiteralPath $archive | Select-Object FullName,Length

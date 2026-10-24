param([string]$Fxc)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
if (!$Fxc) {
    $compiler = Get-ChildItem -Path "${env:ProgramFiles(x86)}\Windows Kits\10\bin\*\x64\fxc.exe" |
        Sort-Object FullName | Select-Object -Last 1
    if (!$compiler) { throw 'Windows SDK fxc.exe is required to regenerate the coverage shader.' }
    $Fxc = $compiler.FullName
}
& $Fxc /nologo /T cs_5_0 /E main /O3 /Qstrip_debug /Qstrip_reflect /Vn CoverageShader `
    /Fh (Join-Path $projectRoot 'shaders\Coverage.bytecode.h') (Join-Path $projectRoot 'shaders\Coverage.hlsl')
if ($LASTEXITCODE -ne 0) { throw 'Coverage shader compilation failed.' }
$header = Join-Path $projectRoot 'shaders\Coverage.bytecode.h'
# fxc emits trailing spaces in both its disassembly and byte array.
$lines = [System.IO.File]::ReadAllLines($header) | ForEach-Object { $_.TrimEnd() }
[System.IO.File]::WriteAllLines($header, $lines, [System.Text.UTF8Encoding]::new($false))

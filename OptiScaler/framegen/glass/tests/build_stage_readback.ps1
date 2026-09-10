param([string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
$repository = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../../..'))
if (!$OutputDirectory) { $OutputDirectory = Join-Path $repository 'artifacts/glass-tests' }
$build = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $build | Out-Null
$exe = Join-Path $build 'StageReadback.exe'
& cl.exe /nologo /std:c++20 /EHsc /O2 /MD /W4 /WX /DNOMINMAX `
    (Join-Path $PSScriptRoot 'StageReadback.cpp') "/Fo$($build.TrimEnd('\'))\" "/Fe$exe" `
    /link d3d12.lib dxgi.lib
if ($LASTEXITCODE -ne 0) { throw 'Stage readback test build failed' }
Write-Output $exe

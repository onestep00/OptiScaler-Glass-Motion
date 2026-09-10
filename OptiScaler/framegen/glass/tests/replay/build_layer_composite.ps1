param([string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
$repository = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../../../..'))
if (!$OutputDirectory) { $OutputDirectory = Join-Path $repository 'artifacts/glass-tests' }
$build = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $build | Out-Null
$exe = Join-Path $build 'LayerComposite.exe'
& cl.exe /nologo /std:c++20 /EHsc /O2 /MD /W4 /WX `
    (Join-Path $PSScriptRoot '../LayerComposite.cpp') "/Fo$($build.TrimEnd('\'))\" "/Fe$exe" `
    /link d3d12.lib dxgi.lib d3dcompiler.lib
if ($LASTEXITCODE -ne 0) { throw 'Layer composite build failed' }
Write-Output $exe

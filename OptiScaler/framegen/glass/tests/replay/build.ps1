param([string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
$repository = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../../../..'))
if (!$OutputDirectory) { $OutputDirectory = Join-Path $repository 'artifacts/glass-replay' }
$build = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $build | Out-Null
$exe = Join-Path $build 'nvngx.dll.glass-replay.exe'
& cl.exe /nologo /std:c++20 /EHsc /O2 /MD /WX /D_CRT_SECURE_NO_WARNINGS `
    "/I$repository/external/nlohmann" "/I$repository/external/nvngx_dlss_sdk" `
    (Join-Path $PSScriptRoot 'ReplayMain.cpp') "/Fo$($build.TrimEnd('\'))\" "/Fe$exe" `
    /link d3d12.lib dxgi.lib dxguid.lib
if ($LASTEXITCODE -ne 0) { throw 'FG replay build failed' }
Write-Output $exe

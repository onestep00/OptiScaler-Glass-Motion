param([string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
$repository = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../../..'))
if (!$OutputDirectory) { $OutputDirectory = Join-Path $repository 'artifacts/glass-geometry-objects' }
$build = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $build | Out-Null
$common = @('/nologo','/std:c++20','/EHsc','/O2','/MD','/W4','/WX','/DNOMINMAX',
    "/I$PSScriptRoot", "/I$repository/OptiScaler", "/I$repository/OptiScaler/include", "/Fo$($build.TrimEnd('\'))\")
& cl.exe @common (Join-Path $PSScriptRoot 'GeometryObjectRegistry.cpp') "/Fe$build/GeometryObjectRegistry.exe"
if ($LASTEXITCODE -ne 0) { throw 'Object registry test build failed' }
& "$build/GeometryObjectRegistry.exe"
if ($LASTEXITCODE -ne 0) { throw 'Object registry test failed' }
& cl.exe @common (Join-Path $PSScriptRoot 'CyberpunkObjectCallbacks.cpp') "/Fe$build/CyberpunkObjectCallbacks.exe" `
    /link (Join-Path $repository 'OptiScaler/library/detours/detours.lib')
if ($LASTEXITCODE -ne 0) { throw 'Object callback test build failed' }
& "$build/CyberpunkObjectCallbacks.exe"
if ($LASTEXITCODE -ne 0) { throw 'Object callback test failed' }

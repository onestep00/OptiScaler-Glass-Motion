param([string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
$repository = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../../..'))
if (!$OutputDirectory) { $OutputDirectory = Join-Path $repository 'artifacts/glass-geometry-shader' }
$build = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $build | Out-Null
$include = Join-Path $repository 'external/FidelityFX-SDK/sdk/tools/ffx_shader_compiler/libs/dxc/inc'
$dxc = Join-Path $repository 'OptiScaler/shaders/shader_tools/dxcompiler.dll'
$tool = Join-Path $build 'GeometryShaderTool.exe'
$gpu = Join-Path $build 'GeometryShaderGpu.exe'
$common = @('/nologo','/std:c++20','/EHsc','/O2','/MD','/W4','/WX','/DNOMINMAX',"/Fo$($build.TrimEnd('\'))\")
& cl.exe @common "/I$PSScriptRoot" "/I$include" (Join-Path $PSScriptRoot 'GeometryShaderTool.cpp') `
    (Join-Path $PSScriptRoot '../DxilVertexHistory.cpp') "/Fe$tool"
if ($LASTEXITCODE -ne 0) { throw 'Geometry shader compiler test build failed' }
& cl.exe @common "/I$PSScriptRoot" "/I$include" (Join-Path $PSScriptRoot 'GeometryShaderGpu.cpp') `
    (Join-Path $PSScriptRoot '../GeometryPipeline.cpp') (Join-Path $PSScriptRoot '../DxilVertexHistory.cpp') `
    "/Fe$gpu" /link d3d12.lib dxgi.lib
if ($LASTEXITCODE -ne 0) { throw 'Geometry shader GPU test build failed' }
& $tool $dxc compile (Join-Path $PSScriptRoot 'GeometryVertex.hlsl') (Join-Path $build 'fixture.dxil') vs_6_0
if ($LASTEXITCODE -ne 0) { throw 'Vertex fixture compilation failed' }
& $tool $dxc compile (Join-Path $PSScriptRoot 'GeometryMaterial.hlsl') (Join-Path $build 'fixture-pixel.dxil') ps_6_0
if ($LASTEXITCODE -ne 0) { throw 'Material fixture compilation failed' }
& $tool $dxc rewrite (Join-Path $build 'fixture.dxil') (Join-Path $build 'fixture-history.dxil') '-'
if ($LASTEXITCODE -ne 0) { throw 'Vertex rewriting or DXIL validation failed' }
& $tool $dxc material (Join-Path $build 'fixture-pixel.dxil') (Join-Path $build 'fixture-motion.dxil') '-' (Join-Path $build 'fixture.dxil')
if ($LASTEXITCODE -ne 0) { throw 'Material rewriting or DXIL validation failed' }
& $tool $dxc capture (Join-Path $build 'fixture-pixel.dxil') (Join-Path $build 'fixture-capture.dxil') '-' (Join-Path $build 'fixture.dxil')
if ($LASTEXITCODE -ne 0) { throw 'Simultaneous color/capture rewriting or DXIL validation failed' }
& $gpu $build $dxc
if ($LASTEXITCODE -ne 0) { throw 'Actual vertex history/material motion GPU test failed' }

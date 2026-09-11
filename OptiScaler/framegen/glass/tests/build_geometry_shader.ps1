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
$instances = Join-Path $build 'GeometryInstances.exe'
$common = @('/nologo','/std:c++20','/EHsc','/O2','/MD','/W4','/WX','/DNOMINMAX',"/Fo$($build.TrimEnd('\'))\")
& cl.exe @common "/I$PSScriptRoot" "/I$include" (Join-Path $PSScriptRoot 'GeometryShaderTool.cpp') `
    (Join-Path $PSScriptRoot '../DxilVertexHistory.cpp') "/Fe$tool"
if ($LASTEXITCODE -ne 0) { throw 'Geometry shader compiler test build failed' }
& cl.exe @common "/I$PSScriptRoot" "/I$include" (Join-Path $PSScriptRoot 'GeometryShaderGpu.cpp') `
    (Join-Path $PSScriptRoot '../GeometryPipeline.cpp') (Join-Path $PSScriptRoot '../DxilVertexHistory.cpp') `
    "/Fe$gpu" /link d3d12.lib dxgi.lib
if ($LASTEXITCODE -ne 0) { throw 'Geometry shader GPU test build failed' }
& cl.exe @common "/I$PSScriptRoot" "/I$include" "/I$repository/OptiScaler" "/I$repository/OptiScaler/include" `
    (Join-Path $PSScriptRoot 'GeometryInstances.cpp') (Join-Path $PSScriptRoot '../GeometryPipeline.cpp') `
    (Join-Path $PSScriptRoot '../DxilVertexHistory.cpp') (Join-Path $PSScriptRoot '../GeometryPipelineCache.cpp') `
    (Join-Path $PSScriptRoot 'GeometryCommandFixture.cpp') (Join-Path $PSScriptRoot '../GeometryCommands.cpp') `
    (Join-Path $PSScriptRoot '../GeometryCreation.cpp') "/Fe$instances" /link d3d12.lib dxgi.lib `
    (Join-Path $repository 'OptiScaler/library/detours/detours.lib')
if ($LASTEXITCODE -ne 0) { throw 'Instance geometry GPU test build failed' }
& cl.exe @common "/I$PSScriptRoot" "/I$include" "/I$repository/OptiScaler" "/I$repository/OptiScaler/include" `
    (Join-Path $PSScriptRoot 'GeometryIndirect.cpp') (Join-Path $PSScriptRoot '../GeometryPipeline.cpp') `
    (Join-Path $PSScriptRoot '../DxilVertexHistory.cpp') (Join-Path $PSScriptRoot '../GeometryPipelineCache.cpp') `
    (Join-Path $PSScriptRoot 'GeometryCommandFixture.cpp') (Join-Path $PSScriptRoot '../GeometryCommands.cpp') `
    (Join-Path $PSScriptRoot '../GeometryCreation.cpp') "/Fe$build/GeometryIndirect.exe" /link d3d12.lib dxgi.lib `
    d3dcompiler.lib (Join-Path $repository 'OptiScaler/library/detours/detours.lib')
if ($LASTEXITCODE -ne 0) { throw 'Indirect binding GPU test build failed' }
& "$build/GeometryIndirect.exe"
if ($LASTEXITCODE -ne 0) { throw 'Indirect binding GPU test failed' }
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
& $tool $dxc compile (Join-Path $PSScriptRoot 'GeometryInstanceVertex.hlsl') (Join-Path $build 'instances.dxil') vs_6_0
if ($LASTEXITCODE -ne 0) { throw 'Instance vertex compilation failed' }
& $tool $dxc rewrite-mapped (Join-Path $build 'instances.dxil') (Join-Path $build 'instances-history.dxil') '-'
if ($LASTEXITCODE -ne 0) { throw 'Instance vertex rewriting failed' }
& $tool $dxc capture-mapped (Join-Path $build 'fixture-pixel.dxil') (Join-Path $build 'instances-capture.dxil') '-' (Join-Path $build 'instances.dxil')
if ($LASTEXITCODE -ne 0) { throw 'Instance material rewriting failed' }
& $instances $build $dxc
if ($LASTEXITCODE -ne 0) { throw 'Instance geometry capture test failed' }
& $instances $build $dxc --observe
if ($LASTEXITCODE -ne 0) { throw 'Actual creation observer GPU test failed' }
& $instances $build $dxc --commands
if ($LASTEXITCODE -ne 0) { throw 'Actual graphics command observer GPU test failed' }
& $instances $build $dxc --capture-command
if ($LASTEXITCODE -ne 0) { throw 'Single-draw capture insertion or root restoration failed' }
& $instances $build $dxc --coverage
if ($LASTEXITCODE -ne 0) { throw 'Object material bit coverage failed' }
& $tool $dxc compile (Join-Path $PSScriptRoot 'GeometryMaterialMrt.hlsl') (Join-Path $build 'fixture-mrt.dxil') ps_6_0
if ($LASTEXITCODE -ne 0) { throw 'MRT material compilation failed' }
& $tool $dxc compile (Join-Path $PSScriptRoot 'GeometryMaterialDual.hlsl') (Join-Path $build 'fixture-dual.dxil') ps_6_0
if ($LASTEXITCODE -ne 0) { throw 'Dual-source material compilation failed' }
& $instances $build $dxc --mrt
if ($LASTEXITCODE -ne 0) { throw 'Original auxiliary MRT preservation failed' }
& $instances $build $dxc --dual-mrt
if ($LASTEXITCODE -ne 0) { throw 'Dual-source MRT preservation failed' }

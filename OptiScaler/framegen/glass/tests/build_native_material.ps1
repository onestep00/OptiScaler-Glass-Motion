param([string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
$repository = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../../..'))
if (!$OutputDirectory) { $OutputDirectory = Join-Path $repository 'artifacts/glass-native-material' }
$build = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $build | Out-Null
$include = Join-Path $repository 'OptiScaler/include'
$dxc = Join-Path $repository 'OptiScaler/shaders/shader_tools/dxcompiler.dll'
$tool = Join-Path $build 'GeometryShaderTool.exe'
$gpu = Join-Path $build 'NativePairGpu.exe'
$common = @('/nologo','/std:c++20','/EHsc','/O2','/MD','/W4','/WX','/DNOMINMAX',"/I$PSScriptRoot","/Fo$($build.TrimEnd('\'))\")
& cl.exe @common "/I$include" (Join-Path $PSScriptRoot 'GeometryShaderTool.cpp') `
    (Join-Path $PSScriptRoot '../DxilVertexHistory.cpp') "/Fe$tool"
if ($LASTEXITCODE -ne 0) { throw 'Shader tool build failed' }
& cl.exe @common "/I$include" (Join-Path $PSScriptRoot 'NativePairGpu.cpp') `
    (Join-Path $PSScriptRoot '../GeometryPipeline.cpp') "$build/DxilVertexHistory.obj" "/Fe$gpu" /link d3d12.lib dxgi.lib
if ($LASTEXITCODE -ne 0) { throw 'Native material test build failed' }
$fixtures = @(
    @('NativePairVertex.hlsl','native-pair-vs.dxil','vs_6_0'),
    @('NativePairPixel.hlsl','native-pair-ps.dxil','ps_6_0'),
    @('NativeMaterialPixel.hlsl','native-material-ps.dxil','ps_6_0'),
    @('InputWordsVertex.hlsl','input-words-vs.dxil','vs_6_0')
)
foreach ($fixture in $fixtures) {
    & $tool $dxc compile (Join-Path $PSScriptRoot $fixture[0]) (Join-Path $build $fixture[1]) $fixture[2]
    if ($LASTEXITCODE -ne 0) { throw "Fixture compilation failed: $($fixture[0])" }
}
& $gpu $build $dxc --material
if ($LASTEXITCODE -ne 0) { throw 'Native material capture failed' }
& $gpu $build $dxc
if ($LASTEXITCODE -ne 0) { throw 'Opaque native capture regression failed' }
& $gpu $build $dxc --input-words
if ($LASTEXITCODE -ne 0) { throw 'Raw input capture regression failed' }

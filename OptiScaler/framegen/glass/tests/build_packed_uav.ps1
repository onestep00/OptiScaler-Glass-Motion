param([string]$OutputDirectory)
$ErrorActionPreference='Stop'
$repository=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../../..'))
if(!$OutputDirectory){$OutputDirectory=Join-Path $repository 'artifacts/packed-uav-preservation'}
$build=[IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $build | Out-Null
$include=Join-Path $repository 'external/FidelityFX-SDK/sdk/tools/ffx_shader_compiler/libs/dxc/inc'
$dxc=Join-Path $repository 'OptiScaler/shaders/shader_tools/dxcompiler.dll'
$tool=Join-Path $build 'GeometryShaderTool.exe'
$common=@('/nologo','/std:c++20','/EHsc','/O2','/MD','/W4','/WX','/DNOMINMAX',"/I$PSScriptRoot","/I$include","/Fo$($build.TrimEnd('\'))\")
& cl.exe @common "$PSScriptRoot/GeometryShaderTool.cpp" "$PSScriptRoot/../DxilVertexHistory.cpp" "/Fe$tool"
if($LASTEXITCODE -ne 0){throw 'Shader tool build failed'}
& $tool $dxc compile "$PSScriptRoot/PackedUavVertex.hlsl" "$build/original.vs.dxil" vs_6_0
if($LASTEXITCODE -ne 0){throw 'Vertex compile failed'}
& $tool $dxc compile "$PSScriptRoot/PackedUavMaterial.hlsl" "$build/original.ps.dxil" ps_6_0
if($LASTEXITCODE -ne 0){throw 'Material compile failed'}
& $tool $dxc rewrite-mapped "$build/original.vs.dxil" "$build/patched.vs.dxil" '-'
if($LASTEXITCODE -ne 0){throw 'Vertex rewrite failed'}
& $tool $dxc packed-inplace-mapped "$build/original.ps.dxil" "$build/patched.ps.dxil" '-' "$build/original.vs.dxil"
if($LASTEXITCODE -ne 0){throw 'In-place pixel rewrite failed'}
& cl.exe @common "$PSScriptRoot/PackedUavPreservation.cpp" "$PSScriptRoot/../GeometryPipeline.cpp" `
    "$build/DxilVertexHistory.obj" "/Fe$build/PackedUavPreservation.exe" /link d3d12.lib dxgi.lib
if($LASTEXITCODE -ne 0){throw 'GPU test build failed'}
& "$build/PackedUavPreservation.exe" $build $dxc
if($LASTEXITCODE -ne 0){throw 'Original GPU writes/color preservation failed'}

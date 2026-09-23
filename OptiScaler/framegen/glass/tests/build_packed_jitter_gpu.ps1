param([string]$OutputDirectory)
if (Get-Process -Name Cyberpunk2077 -ErrorAction SilentlyContinue) {
    throw 'Cyberpunk2077 is running; GPU test binaries must not run during a game session.'
}
$ErrorActionPreference = 'Stop'
$repository = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../../..'))
if (!$OutputDirectory) { $OutputDirectory = Join-Path $repository 'artifacts/packed-jitter-gpu' }
$build = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $build | Out-Null
$include = Join-Path $repository 'external/FidelityFX-SDK/sdk/tools/ffx_shader_compiler/libs/dxc/inc'
$dxc = Join-Path $repository 'OptiScaler/shaders/shader_tools/dxcompiler.dll'
$tool = Join-Path $build 'GeometryShaderTool.exe'
$common = @('/nologo','/std:c++20','/EHsc','/O2','/MD','/W4','/WX','/DNOMINMAX',"/I$PSScriptRoot","/I$include","/Fo$($build.TrimEnd('\'))\")
& cl.exe @common "$PSScriptRoot/GeometryShaderTool.cpp" "$PSScriptRoot/../DxilVertexHistory.cpp" "/Fe$tool"
if ($LASTEXITCODE -ne 0) { throw 'Shader tool build failed' }
# The audited Cyberpunk camera block: b1/space0, 848 bytes, row 51 XY.
& $tool $dxc compile "$PSScriptRoot/PackedJitterVertex.hlsl" "$build/original.jitter.vs.dxil" vs_6_0
if ($LASTEXITCODE -ne 0) { throw 'Jitter vertex compile failed' }
# Pair-less vertex shader for the R6 fallback check.
& $tool $dxc compile "$PSScriptRoot/PackedUavVertex.hlsl" "$build/r6.vs.dxil" vs_6_0
if ($LASTEXITCODE -ne 0) { throw 'Pair-less vertex compile failed' }
# Camera-block vertex shader that declares the interpolants the MRT material
# reads, so the coverage-fallback pair matches the game's VS/PS relation.
& $tool $dxc compile "$PSScriptRoot/PackedJitterMaterialVertex.hlsl" "$build/material.vs.dxil" vs_6_0
if ($LASTEXITCODE -ne 0) { throw 'Material vertex compile failed' }
& $tool $dxc compile "$PSScriptRoot/PackedUavMaterial.hlsl" "$build/original.ps.dxil" ps_6_0
if ($LASTEXITCODE -ne 0) { throw 'Material compile failed' }
# MRT-above-one material: the coverage-fallback control. Extra MRT slots are
# skipped by the rewrite, so this shader keeps taking the material path and the
# fallback counter must not move.
& $tool $dxc compile "$PSScriptRoot/GeometryMaterialMrt.hlsl" "$build/mrt.ps.dxil" ps_6_0
if ($LASTEXITCODE -ne 0) { throw 'MRT material compile failed' }
# Colour + SV_Depth material: the packed coverage fallback case. Depth exports
# are preserved on the coverage-only variant only, so this shader is rejected by
# the material pass and increments the fallback counter exactly once.
& $tool $dxc compile "$PSScriptRoot/MaterialDepthPixel.hlsl" "$build/materialdepth.ps.dxil" ps_6_0
if ($LASTEXITCODE -ne 0) { throw 'Depth material compile failed' }
& cl.exe @common "$PSScriptRoot/PackedJitterGpu.cpp" "$PSScriptRoot/../GeometryPipeline.cpp" `
    "$PSScriptRoot/../DxilVertexHistory.cpp" "/Fe$build/PackedJitterGpu.exe" /link d3d12.lib dxgi.lib
if ($LASTEXITCODE -ne 0) { throw 'Capture pair GPU test build failed' }
& "$build/PackedJitterGpu.exe" $build $dxc
if ($LASTEXITCODE -ne 0) { throw 'Capture pair GPU discrimination failed' }

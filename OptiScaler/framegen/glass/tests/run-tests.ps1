param([string]$OutputDirectory)
if (Get-Process -Name Cyberpunk2077 -ErrorAction SilentlyContinue) {
    throw 'Cyberpunk2077 is running; GPU test binaries must not run during a game session.'
}
$ErrorActionPreference = 'Stop'
$compiler = (Get-Command cl.exe -ErrorAction Stop).Source
$optiDirectory = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..'))
$repository = Split-Path $optiDirectory -Parent
if (!$OutputDirectory) { $OutputDirectory = Join-Path $repository 'artifacts\glass-tests' }
$buildDirectory = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $buildDirectory | Out-Null
$objectDirectory = $buildDirectory.TrimEnd('\') + '\'
$common = @('/nologo', '/std:c++20', '/EHsc', '/O2', '/MD', '/D_CRT_SECURE_NO_WARNINGS',
            "/I$PSScriptRoot", "/Fo$objectDirectory")
$selectionExe = Join-Path $buildDirectory 'PackedMotionSelection.exe'
& $compiler @common (Join-Path $PSScriptRoot 'PackedMotionSelection.cpp') "/Fe$selectionExe"
if ($LASTEXITCODE -ne 0) { throw 'Packed frame selection test build failed' }
& $selectionExe
if ($LASTEXITCODE -ne 0) { throw 'Packed frame selection contract failed' }
$materialExe = Join-Path $buildDirectory 'MaterialCaptureBlend.exe'
& $compiler @common (Join-Path $PSScriptRoot 'MaterialCaptureBlend.cpp') "/Fe$materialExe" /link d3d12.lib dxgi.lib dxguid.lib d3dcompiler.lib
if ($LASTEXITCODE -ne 0) { throw 'Material capture blend test build failed' }
& $materialExe
if ($LASTEXITCODE -ne 0) { throw 'Material capture blend test failed' }
$recordingExe = Join-Path $buildDirectory 'ComputeRecording.exe'
& $compiler @common (Join-Path $PSScriptRoot 'ComputeRecording.cpp') "/Fe$recordingExe"
if ($LASTEXITCODE -ne 0) { throw 'Compute recording test build failed' }
& $recordingExe
if ($LASTEXITCODE -ne 0) { throw 'Compute recording contract failed' }
$lifetimeExe = Join-Path $buildDirectory 'CommandLifetime.exe'
& $compiler @common (Join-Path $PSScriptRoot 'CommandLifetime.cpp') "/Fe$lifetimeExe" /link d3d12.lib dxgi.lib dxguid.lib
if ($LASTEXITCODE -ne 0) { throw 'Command lifetime test build failed' }
& $lifetimeExe
if ($LASTEXITCODE -ne 0) { throw 'Command lifetime contract failed' }
$tagsExe = Join-Path $buildDirectory 'TaggedInputs.exe'
& $compiler @common (Join-Path $PSScriptRoot 'TaggedInputs.cpp') "/Fe$tagsExe"
if ($LASTEXITCODE -ne 0) { throw 'Tag metadata test build failed' }
& $tagsExe
if ($LASTEXITCODE -ne 0) { throw 'Tag metadata contract failed' }
$arrayMapExe = Join-Path $buildDirectory 'ArrayMapping.exe'
& $compiler @common (Join-Path $PSScriptRoot 'ArrayMapping.cpp') (Join-Path $PSScriptRoot '../GlassArrayMapping.cpp') "/Fe$arrayMapExe"
if ($LASTEXITCODE -ne 0) { throw 'Array mapping test build failed' }
& $arrayMapExe
if ($LASTEXITCODE -ne 0) { throw 'Array mapping contract failed' }
$hookGateExe = Join-Path $buildDirectory 'HookGate.exe'
& $compiler @common (Join-Path $PSScriptRoot 'HookGate.cpp') "/Fe$hookGateExe"
if ($LASTEXITCODE -ne 0) { throw 'Hook gate test build failed' }
& $hookGateExe
if ($LASTEXITCODE -ne 0) { throw 'Hook gate contract failed' }
$motionDumpExe = Join-Path $buildDirectory 'MotionDump.exe'
& $compiler @common (Join-Path $PSScriptRoot 'MotionDump.cpp') "/Fe$motionDumpExe"
if ($LASTEXITCODE -ne 0) { throw 'Motion dump test build failed' }
& $motionDumpExe
if ($LASTEXITCODE -ne 0) { throw 'Motion dump format contract failed' }
$bridgeExe = Join-Path $buildDirectory 'StreamlineTagBridge.exe'
& $compiler @common "/I$optiDirectory" "/I$repository\external\streamline" `
    (Join-Path $PSScriptRoot 'StreamlineTagBridge.cpp') "/Fe$bridgeExe" /link d3d12.lib dxgi.lib dxguid.lib
if ($LASTEXITCODE -ne 0) { throw 'Streamline bridge test build failed' }
& $bridgeExe
if ($LASTEXITCODE -ne 0) { throw 'Streamline bridge contract failed' }
$gpuExe = Join-Path $buildDirectory 'GpuResources.exe'
& $compiler @common (Join-Path $PSScriptRoot 'GpuResources.cpp') "/Fe$gpuExe" /link d3d12.lib dxgi.lib dxguid.lib
if ($LASTEXITCODE -ne 0) { throw 'GPU test build failed' }
& $gpuExe
if ($LASTEXITCODE -ne 0) { throw 'GPU timer test failed' }

$sessionExe = Join-Path $buildDirectory 'NativeSession.exe'
& $compiler @common (Join-Path $PSScriptRoot 'NativeSession.cpp') "/Fe$sessionExe" /link d3d12.lib dxgi.lib dxguid.lib d3dcompiler.lib
if ($LASTEXITCODE -ne 0) { throw 'Native session test build failed' }
& $sessionExe (Join-Path $PSScriptRoot '..\GlassObjectMotion.hlsl')
if ($LASTEXITCODE -ne 0) { throw 'Native session queue type or release test failed' }

$memoExe = Join-Path $buildDirectory 'PipelineCacheMemo.exe'
& $compiler @common '/DNOMINMAX' "/I$optiDirectory" "/I$optiDirectory\include" `
    "/I$optiDirectory\include" `
    "/I$repository\external\FidelityFX-SDK\sdk\tools\ffx_shader_compiler\libs\dxc\inc" `
    (Join-Path $PSScriptRoot 'PipelineCacheMemo.cpp') (Join-Path $PSScriptRoot '../GeometryPipeline.cpp') `
    (Join-Path $PSScriptRoot '../GeometryCoverageRecorder.cpp') (Join-Path $PSScriptRoot '../DxilVertexHistory.cpp') `
    (Join-Path $PSScriptRoot '../GeometryPipelineCache.cpp') (Join-Path $PSScriptRoot '../NativeGraftCatalog.cpp') (Join-Path $PSScriptRoot 'GeometryCommandFixture.cpp') `
    (Join-Path $PSScriptRoot '../GeometryCommands.cpp') (Join-Path $PSScriptRoot '../GeometryViews.cpp') `
    (Join-Path $PSScriptRoot '../GeometryCreation.cpp') "/Fe$memoExe" /link d3d12.lib dxgi.lib `
    (Join-Path $optiDirectory 'library\detours\detours.lib')
if ($LASTEXITCODE -ne 0) { throw 'Pipeline cache memo test build failed' }
$memoFixture = Join-Path $repository 'artifacts\glass-geometry-shader'
if (!(Test-Path -LiteralPath (Join-Path $memoFixture 'instances.dxil'))) {
    throw "geometry shader fixtures missing; run build_geometry_shader.ps1 to produce $memoFixture"
}
& $memoExe $memoFixture (Join-Path $optiDirectory 'shaders\shader_tools\dxcompiler.dll')
if ($LASTEXITCODE -ne 0) { throw 'Pipeline cache memo contract failed' }

# Packed native graft rewrite on one exported graft and its camera-only array
# variant with a paired original PS (transparent_liquid). Needs the local
# catalog from tools/export_native_grafts.py.
$graftModule = Join-Path $repository 'artifacts\glass-grafts'
$graftWorkspace = Join-Path (Split-Path $repository -Parent) 'glass-native-material-v1'
if (!(Test-Path -LiteralPath (Join-Path $graftModule 'Glass\grafts\index.bin'))) {
    throw "graft catalog missing; run tools\export_native_grafts.py --out $graftModule\Glass"
}
$graftExe = Join-Path $buildDirectory 'NativeGraftPacked.exe'
& $compiler @common '/DNOMINMAX' "/I$repository\external\FidelityFX-SDK\sdk\tools\ffx_shader_compiler\libs\dxc\inc" `
    (Join-Path $PSScriptRoot 'NativeGraftPacked.cpp') (Join-Path $PSScriptRoot '../DxilVertexHistory.cpp') `
    (Join-Path $PSScriptRoot '../NativeGraftCatalog.cpp') "/Fe$graftExe"
if ($LASTEXITCODE -ne 0) { throw 'Native graft packed test build failed' }
& $graftExe (Join-Path $optiDirectory 'shaders\shader_tools\dxcompiler.dll') $graftModule `
    (Join-Path $graftWorkspace 'all-transparent-vs\4140f6d44e631b23d56730116a2fe84189c7740676be7b5c4661301636516ad6.dxbc') `
    (Join-Path $graftWorkspace 'all-transparent-ps\4d4bdc5d8fa67396475344d2efc98c0a071f3066244dc4c41a4f73b91c23aa3e.dxbc') 7 8 7 8
if ($LASTEXITCODE -ne 0) { throw 'Native graft packed rewrite contract failed' }
# Camera-only record (no native twin): glass MeshStatic VS with branched
# projection arms and its paired glass PS.
& $graftExe (Join-Path $optiDirectory 'shaders\shader_tools\dxcompiler.dll') $graftModule `
    (Join-Path $graftWorkspace 'all-transparent-vs\39f8b55578cc38ccb2c3263571fe2a9c1eeb31d24c83de653675021874f442d0.dxbc') `
    (Join-Path $graftWorkspace 'all-transparent-ps\6bc2b7e3e38cd012a38ecb7de0ab2bd49f6210286b3c067a98bbed91cb5b1a7c.dxbc') none none 10 11
if ($LASTEXITCODE -ne 0) { throw 'Native graft camera-only packed rewrite contract failed' }
# Refused vehicle VS: vehicle_destr_blendshape MeshStaticVehicle (no native twin,
# vehicle damage input and grid modifiers) has no record and is listed in
# refused.bin; its draws keep the engine's motion.
& $graftExe (Join-Path $optiDirectory 'shaders\shader_tools\dxcompiler.dll') $graftModule `
    (Join-Path $graftWorkspace 'all-transparent-vs\296676c3819d551b094c916e23edacbf20c0f10cdc7d4cce1f0dcf4fcde2c974.dxbc') refused
if ($LASTEXITCODE -ne 0) { throw 'Native graft vehicle refusal contract failed' }

$imgui = Join-Path $optiDirectory 'include\imgui'
$sources = @((Join-Path $PSScriptRoot 'Settings.cpp'),
             (Join-Path $PSScriptRoot '../GlassArrayMapping.cpp'))
foreach ($name in @('imgui.cpp', 'imgui_draw.cpp', 'imgui_widgets.cpp', 'imgui_tables.cpp')) {
    $sources += Join-Path $imgui $name
}
$settingsExe = Join-Path $buildDirectory 'Settings.exe'
& $compiler @common '/DIMGUI_USER_CONFIG=<ImGuiTestConfig.h>' "/I$optiDirectory\include" `
    "/I$repository\external\simpleini" @sources "/Fe$settingsExe" /link user32.lib gdi32.lib imm32.lib dwmapi.lib
if ($LASTEXITCODE -ne 0) { throw 'Settings test build failed' }
$settingsDirectory = Join-Path $buildDirectory ('settings-' + [Guid]::NewGuid().ToString('N'))
& $settingsExe $settingsDirectory
if ($LASTEXITCODE -ne 0) { throw 'Settings test failed' }

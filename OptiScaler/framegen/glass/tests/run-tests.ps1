param([string]$OutputDirectory)
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
$gpuExe = Join-Path $buildDirectory 'GpuResources.exe'
& $compiler @common (Join-Path $PSScriptRoot 'GpuResources.cpp') "/Fe$gpuExe" /link d3d12.lib dxgi.lib dxguid.lib
if ($LASTEXITCODE -ne 0) { throw 'GPU test build failed' }
& $gpuExe
if ($LASTEXITCODE -ne 0) { throw 'GPU resource or timer test failed' }

$imgui = Join-Path $optiDirectory 'include\imgui'
$sources = @((Join-Path $PSScriptRoot 'Settings.cpp'))
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

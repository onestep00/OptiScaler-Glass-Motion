param([Parameter(Mandatory=$true)][string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
$module = Split-Path $PSScriptRoot -Parent
$opti = [IO.Path]::GetFullPath((Join-Path $module '../..'))
$destination = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $destination -Force | Out-Null
$common = @('/nologo','/std:c++20','/EHsc','/O2','/W4','/WX','/MD','/DNOMINMAX',"/I$opti/include")
$detours = Join-Path $opti 'library/detours/detours.lib'
function Run-Checked([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program failed with exit $LASTEXITCODE" }
}
foreach ($name in @('DiagnosticCallerContext','ArraySourceTrace')) {
    Run-Checked 'ml64.exe' @('/nologo','/c',"/Fo$destination/${name}Asm.obj",(Join-Path $PSScriptRoot "$name.asm"))
}
foreach ($name in @('DiagnosticCallerContext','DiagnosticArraySource','ArraySourceTrace','ArrayWrapper')) {
    $objects = @()
    if ($name -in @('DiagnosticCallerContext','ArraySourceTrace')) { $objects += "$destination/${name}Asm.obj" }
    Run-Checked 'cl.exe' ($common + @((Join-Path $PSScriptRoot "$name.cpp")) + $objects +
        @("/Fo$destination/$name.obj","/Fe$destination/$name.exe",'/link',$detours))
    Run-Checked "$destination/$name.exe" @()
}
Run-Checked 'cl.exe' ($common + @('/LD','/DGLASS_ARRAY_SOURCE_TRACE',
    (Join-Path $module 'ExperimentInstanceUpdates.cpp'),"/Fo$destination/ArraySourceTraceDll.obj",
    "/Fe$destination/ArraySourceTrace.dll",'/link',$detours,"/IMPLIB:$destination/ArraySourceTraceDll.lib"))
Write-Output 'PASS source_trace_build=1 independent_tests=4 game_injection=0'

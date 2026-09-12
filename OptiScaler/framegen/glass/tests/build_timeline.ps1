param([Parameter(Mandatory=$true)][string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
$destination = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $destination) { throw 'Output directory must be new' }
New-Item -ItemType Directory -Path $destination | Out-Null
$module = Split-Path $PSScriptRoot -Parent
$common = @('/nologo','/std:c++20','/EHsc','/O2','/W4','/WX','/MD','/DNOMINMAX')
function Run-Checked([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program failed with exit $LASTEXITCODE" }
}
Run-Checked 'cl.exe' ($common + @((Join-Path $PSScriptRoot 'TimelineModule.cpp'),
    "/Fo$destination/TimelineTest.obj", "/Fe$destination/TimelineTest.exe"))
Run-Checked "$destination/TimelineTest.exe" @("$destination/fixture")
Run-Checked 'cl.exe' ($common + @('/LD', (Join-Path $module 'ExperimentTimelineModule.cpp'),
    "/Fo$destination/Timeline.obj", "/Fe$destination/Timeline.dll", '/link', "/IMPLIB:$destination/Timeline.lib"))
Write-Output 'PASS timeline_build=1 independent_checks=1 game_injection=0'

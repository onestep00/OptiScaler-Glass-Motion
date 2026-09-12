param([Parameter(Mandatory=$true)][string]$OutputDirectory, [string]$RecordedParents)
$ErrorActionPreference = 'Stop'
$repository = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../../..'))
$destination = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $destination) { throw 'Output directory must be new' }
New-Item -ItemType Directory -Path $destination | Out-Null
$common = @('/nologo','/std:c++20','/EHsc','/O2','/W4','/WX','/MD','/DNOMINMAX',"/I$repository/OptiScaler/include")
$detours = Join-Path $repository 'OptiScaler/library/detours/detours.lib'
function Run-Checked([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program failed with exit $LASTEXITCODE" }
}
foreach ($test in @('InstanceSelection','InstanceProducer','PacketParents')) {
    $arguments = $common + @((Join-Path $PSScriptRoot "$test.cpp"), "/Fo$destination/$test.obj", "/Fe$destination/$test.exe")
    if ($test -ne 'InstanceSelection') { $arguments += @('/link',$detours) }
    Run-Checked 'cl.exe' $arguments
    $runArguments = @()
    if ($test -eq 'PacketParents' -and $RecordedParents) { $runArguments += [IO.Path]::GetFullPath($RecordedParents) }
    Run-Checked "$destination/$test.exe" $runArguments
}
Run-Checked 'cl.exe' ($common + @('/LD',(Join-Path $PSScriptRoot '../ExperimentPacketParents.cpp'),
    "/Fo$destination/PacketParentsDll.obj", "/Fe$destination/PacketParents.dll", '/link',$detours,
    "/IMPLIB:$destination/PacketParents.lib"))
Write-Output 'PASS packet_parent_build=1 game_injection=0'

# Builds the softpn NDIS driver projects with the WDK from NuGet (see packages.config).
#   .\build.ps1                      # softpn_ndis.sys + test tool, Debug x64
#   .\build.ps1 -Configuration Release
#   .\build.ps1 -Projects upstream\sys\630\ndisprot630.vcxproj,upstream\test\prottest.vcxproj   # unmodified sample
# Needs: VS with the "Windows Driver Kit" component (provides the WindowsKernelModeDriver10.0 toolset)
# and `nuget restore packages.config -PackagesDirectory packages` once.
param(
    [ValidateSet('Debug', 'Release')] [string] $Configuration = 'Debug',
    [string[]] $Projects = @('sys\softpn_ndis.vcxproj', 'test\softpn_ndis_test.vcxproj')
)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot

if (-not (Test-Path "$here\packages\Microsoft.Windows.WDK.x64.10.0.28000.2526")) {
    throw "WDK NuGet packages missing: run 'nuget restore packages.config -PackagesDirectory packages' in $here"
}
# 64-bit MSBuild: the 32-bit one makes the WDK use its x86 tool folder, which the NuGet package lacks.
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -prerelease -requires Microsoft.Component.MSBuild -property installationPath
$msbuild = Join-Path $vs 'MSBuild\Current\Bin\amd64\MSBuild.exe'
if (-not (Test-Path $msbuild)) { throw "64-bit MSBuild not found: $msbuild" }

foreach ($p in $Projects) {
    & $msbuild (Join-Path $here $p) -nologo -v:m -m "-p:Configuration=$Configuration" -p:Platform=x64
    if ($LASTEXITCODE -ne 0) { throw "build failed: $p" }
}

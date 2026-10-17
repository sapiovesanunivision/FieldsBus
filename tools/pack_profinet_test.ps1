<#
.SYNOPSIS
    Builds and collects the PROFINET (softpn) test kit for another PC.

.DESCRIPTION
    Copies into one folder everything needed to run the PROFINET tests of
    docs/profinet-test-runbook.md on a PC without Visual Studio:

      pn_device_demo, pn_probe, pn_selftest, pn_controller_sim, fb_device_demo  (+ .pdb)
      SoftFieldbusSample (C#) + SoftFieldbus.Net.dll + Ijwhost.dll              (+ .pdb)
      the C++ runtime DLLs (app-local, from the VS redist folder)
      the GSDML, the runbook, and VERSION.txt (commit, configuration, date)

    RelWithDebInfo is the default: normal runtime (no debug DLLs needed on the target),
    optimised like Release, with PDBs for the VS remote debugger.

    The target PC needs Npcap (https://npcap.com) or the softpn_ndis driver (drivers/softpn_ndis,
    pack_kit.ps1) and, for the C# sample, the .NET 8 runtime. --backend auto|npcap|ndis selects it.

.EXAMPLE
    pwsh tools\pack_profinet_test.ps1 -Build
    pwsh tools\pack_profinet_test.ps1 -Build -Destination \\10.0.0.128\D\SoftPN-Test
#>
[CmdletBinding()]
param(
    [string]$Destination = '',
    [ValidateSet('RelWithDebInfo', 'Release', 'Debug')][string]$Configuration = 'RelWithDebInfo',
    [string]$BuildDir = '',
    [switch]$Build,
    [switch]$NoCrt
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3

$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if (-not $BuildDir) { $BuildDir = Join-Path $repo 'build\vs2026' }
if (-not $Destination) { $Destination = Join-Path $repo 'build\softpn-testkit' }
$bin = Join-Path $BuildDir "bin\$Configuration"

if ($Configuration -eq 'Debug') {
    Write-Warning 'Debug builds need the debug C++ runtime (ucrtbased.dll...), which is not installed on normal PCs. Prefer RelWithDebInfo.'
}

# ---- Build -----------------------------------------------------------------------
if ($Build) {
    if (-not (Test-Path (Join-Path $BuildDir 'CMakeCache.txt'))) {
        throw "$BuildDir is not configured: run 'cmake --preset vs2026' (with NPCAP_SDK set) first"
    }
    $targets = @('pn_device_demo', 'pn_probe', 'pn_selftest', 'pn_controller_sim', 'fb_device_demo', 'SoftFieldbus.Net', 'SoftFieldbusSample') # C# sample needs the wrapper built first
    $cmakeArgs = @('--build', $BuildDir, '--config', $Configuration, '--target') + $targets + @('--', '/restore', '/v:minimal', '/nologo')
    Write-Host "cmake $($cmakeArgs -join ' ')"
    & cmake @cmakeArgs
    if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }
}

# ---- Collect -----------------------------------------------------------------------
$native = @('pn_device_demo', 'pn_probe', 'pn_selftest', 'pn_controller_sim', 'fb_device_demo')
foreach ($n in $native) {
    if (-not (Test-Path (Join-Path $bin "$n.exe"))) {
        throw "$bin\$n.exe missing: build with -Build (PROFINET targets need NPCAP_SDK at configure time)"
    }
}
New-Item -ItemType Directory -Force -Path $Destination | Out-Null

$files = @()
foreach ($n in $native) { $files += "$n.exe", "$n.pdb" }
$files += 'SoftFieldbus.Net.dll', 'SoftFieldbus.Net.pdb', 'Ijwhost.dll',
          'SoftFieldbusSample.exe', 'SoftFieldbusSample.dll', 'SoftFieldbusSample.pdb',
          'SoftFieldbusSample.runtimeconfig.json', 'SoftFieldbusSample.deps.json'
$copied = 0
foreach ($f in $files) {
    $src = Join-Path $bin $f
    if (Test-Path $src) {
        Copy-Item -LiteralPath $src -Destination $Destination -Force
        $copied++
    } elseif ($f -notlike '*.pdb') {
        Write-Warning "not found (skipped): $f"
    }
}

# C++ runtime, app-local: the v145 runtime may be newer than the redistributable installed on the target.
if (-not $NoCrt -and $Configuration -ne 'Debug') {
    $vsRoots = @("${env:ProgramFiles}\Microsoft Visual Studio\18")
    $crt = Get-ChildItem -Path $vsRoots -Recurse -Directory -Filter 'Microsoft.VC145.CRT' -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match '\\x64\\Microsoft\.VC145\.CRT$' -and $_.FullName -notmatch 'onecore' } |
        Sort-Object FullName -Descending | Select-Object -First 1
    if ($crt) {
        Get-ChildItem -LiteralPath $crt.FullName -Filter '*.dll' | Copy-Item -Destination $Destination -Force
        Write-Host "C++ runtime from $($crt.FullName)"
    } else {
        Write-Warning 'Microsoft.VC145.CRT not found: install the latest VC++ x64 redistributable on the target PC.'
    }
}

$gsdml = @(Get-ChildItem -Path (Join-Path $repo 'gsdml') -Filter 'GSDML-*.xml')
$gsdml | Copy-Item -Destination $Destination -Force
Copy-Item -LiteralPath (Join-Path $repo 'docs\profinet-test-runbook.md') -Destination $Destination -Force
Copy-Item -LiteralPath (Join-Path $repo 'docs\softpn-siemens-setup.md') -Destination $Destination -Force

$commit = (& git -C $repo describe --always --dirty --tags).Trim()
$branch = (& git -C $repo rev-parse --abbrev-ref HEAD).Trim()
@(
    "softpn PROFINET test kit",
    "commit        $commit ($branch)",
    "configuration $Configuration",
    "built         $((Get-Item (Join-Path $bin 'pn_device_demo.exe')).LastWriteTime.ToString('yyyy-MM-dd HH:mm'))",
    "packed        $((Get-Date).ToString('yyyy-MM-dd HH:mm')) on $env:COMPUTERNAME",
    "",
    "Needs on the target: Npcap (npcap.com, free edition: lab use only) or the softpn_ndis driver; .NET 8 runtime for SoftFieldbusSample.",
    "Self-test (no NIC needed):  pn_selftest --gsdml $($gsdml[0].Name)"
) | Set-Content -LiteralPath (Join-Path $Destination 'VERSION.txt') -Encoding utf8

Write-Host "Test kit: $Destination ($copied program files + runtime + $($gsdml.Count) GSDML + runbook + Siemens setup)"
Write-Host "First step on the target: pn_selftest --gsdml $($gsdml[0].Name)"

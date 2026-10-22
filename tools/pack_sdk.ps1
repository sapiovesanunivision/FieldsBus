<#
.SYNOPSIS
    Builds the SoftFieldbus SDK for Windows x64 and Linux x64 and packs it into one folder (+ .zip and .tar.gz).

.DESCRIPTION
    One package for both platforms (layout and use: sdk/README.md):

      include/                          headers (softeip, softmb, softfb, softpn)
      lib/windows-x64/Debug|Release/    static libs                                           VS 2026, /MDd /MD
      lib/windows-x64/shared/Debug|Release/  SoftFieldbus.lib (import library of the DLL)
      bin/windows-x64/Debug|Release/    SoftFieldbus.dll; Release: tools + C++ runtime
      lib/linux-x64/                    static libs (-fPIC) + libSoftFieldbus.so               WSL distro, GCC
      bin/linux-x64/                    tools
      cmake/SoftFieldbusConfig.cmake    find_package(SoftFieldbus)
      examples/                         sample sources + CMakeLists.txt + CMakePresets.json
      gsdml/, docs/, README.md, VERSION.txt

    Steps: export the sources (committed HEAD, or -WorkingTree), build on Windows (cmake, Visual Studio 18 2026)
    and in WSL (cmake, Unix Makefiles), assemble, then verify by building the samples FROM THE PACKAGE (shared and
    static) on both platforms and running them briefly. The work folder is build/sdk-work (deleted and recreated).

    Needs: VS 2026 with its CMake (4.2+); NPCAP_SDK (Windows libraries contain the Npcap backend); a WSL distro with
    g++ and cmake 3.21+ (for Linux).

.EXAMPLE
    pwsh tools\pack_sdk.ps1
    pwsh tools\pack_sdk.ps1 -WorkingTree -Distro Ubuntu
    pwsh tools\pack_sdk.ps1 -SkipLinux
#>
[CmdletBinding()]
param(
    [string]$Destination = '',          # default: build\sdk
    [string]$Distro = 'Geti',           # WSL distribution for the Linux build
    [switch]$WorkingTree,               # pack the working tree (uncommitted files included) instead of HEAD
    [switch]$SkipWindows,
    [switch]$SkipLinux,
    [switch]$SkipVerify
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3

$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if (-not $Destination) { $Destination = Join-Path $repo 'build\sdk' }
$work = Join-Path $repo 'build\sdk-work'
$generator = 'Visual Studio 18 2026'
$tools = @('pn_probe', 'pn_device_demo', 'fb_device_demo', 'eip_adapter_demo', 'mb_server_demo',
           'pn_controller_sim', 'eip_scanner_sim', 'mb_client')
$staticLibs = @('softeip', 'softmb', 'softpn', 'softfieldbus')
$samples = @('eip_adapter_demo', 'mb_server_demo', 'fb_device_demo', 'pn_device_demo')

function Invoke-Checked([string]$Exe, [string[]]$Arguments, [string]$What) {
    Write-Host "> $What"
    & $Exe @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$What failed (exit $LASTEXITCODE)" }
}

function ConvertTo-WslPath([string]$Path) {
    $p = (& wsl -d $Distro -- wslpath -u ($Path -replace '\\', '/'))
    if ($LASTEXITCODE -ne 0) { throw "WSL distro '$Distro' not available (wsl -l -v)" }
    return $p.Trim()
}

function Invoke-Wsl([string]$Script, [string]$What) {
    $file = Join-Path $work ("wsl-" + [guid]::NewGuid().ToString('N') + '.sh')
    [System.IO.File]::WriteAllText($file, "set -euo pipefail`n" + $Script.Replace("`r`n", "`n"), (New-Object System.Text.UTF8Encoding $false))
    Invoke-Checked 'wsl' @('-d', $Distro, '--', 'bash', (ConvertTo-WslPath $file)) $What
}

# ---- Version -------------------------------------------------------------------------------------------
$commit = (& git -C $repo rev-parse --short HEAD).Trim()
$branch = (& git -C $repo rev-parse --abbrev-ref HEAD).Trim()
$dirty = & git -C $repo status --porcelain -- include src sdk examples gsdml CMakeLists.txt drivers/softpn_ndis/inc
if ($WorkingTree -and $dirty) { $commit += '-dirty' }
elseif ($dirty) { Write-Warning "uncommitted changes in the packed folders are NOT in the package (HEAD is packed; -WorkingTree packs them):`n$($dirty -join "`n")" }
$name = "SoftFieldbus-SDK-$((Get-Date).ToString('yyyyMMdd'))-$commit"
$stage = Join-Path $Destination $name

# ---- Sources -------------------------------------------------------------------------------------------
if (Test-Path $work) { Remove-Item -Recurse -Force $work }
New-Item -ItemType Directory -Force -Path $work | Out-Null
$srcTar = Join-Path $work 'src.tar'
if ($WorkingTree) {
    $list = Join-Path $work 'files.txt'
    & git -C $repo ls-files -co --exclude-standard | Where-Object { Test-Path (Join-Path $repo $_) } | Set-Content $list -Encoding ascii
    Invoke-Checked 'tar' @('-cf', $srcTar, '-C', $repo, '-T', $list) 'tar of the working tree'
} else {
    Invoke-Checked 'git' @('-C', $repo, 'archive', '--format=tar', "--output=$srcTar", 'HEAD') "git archive HEAD ($commit)"
}
$src = Join-Path $work 'src'
New-Item -ItemType Directory -Force -Path $src | Out-Null
Invoke-Checked 'tar' @('-xf', $srcTar, '-C', $src) 'extract sources'

if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force -Path $stage | Out-Null

# ---- Common files --------------------------------------------------------------------------------------
Copy-Item -Recurse (Join-Path $src 'include') $stage
New-Item -ItemType Directory -Force -Path (Join-Path $stage 'cmake'), (Join-Path $stage 'examples'), (Join-Path $stage 'gsdml'), (Join-Path $stage 'docs\plc') | Out-Null
Copy-Item (Join-Path $src 'sdk\cmake\SoftFieldbusConfig.cmake') (Join-Path $stage 'cmake')
Copy-Item (Join-Path $src 'sdk\README.md') $stage
Copy-Item (Join-Path $src 'sdk\examples\*') (Join-Path $stage 'examples')
foreach ($s in $samples) { Copy-Item (Join-Path $src "examples\$s.cpp") (Join-Path $stage 'examples') }
Copy-Item (Join-Path $src 'gsdml\*.xml') (Join-Path $stage 'gsdml')
foreach ($d in 'softpn-l2-backend.md', 'softpn-siemens-setup.md', 'SoftFieldbusPnPLCSetup.md', 'SoftFieldbusEipPLCSetup.md') {
    Copy-Item (Join-Path $src "docs\$d") (Join-Path $stage 'docs')
}
Copy-Item (Join-Path $src 'docs\plc\*') (Join-Path $stage 'docs\plc')

# ---- Windows --------------------------------------------------------------------------------------------
$winInfo = 'not built'
if (-not $SkipWindows) {
    if (-not $env:NPCAP_SDK -or -not (Test-Path (Join-Path $env:NPCAP_SDK 'Include\pcap.h'))) {
        throw 'NPCAP_SDK is not set: the Windows libraries must contain the Npcap backend (or use -SkipWindows)'
    }
    $winB = Join-Path $work 'win'
    Invoke-Checked 'cmake' @('-S', $src, '-B', $winB, '-G', $generator, '-A', 'x64', '-DSOFTFIELDBUS_SHARED=ON',
        '-DSOFTEIP_BUILD_DOTNET=OFF', "-DSOFTPN_NPCAP_SDK=$env:NPCAP_SDK",
        # debug info inside the static libs (/Z7): users get no LNK4099 "PDB not found"
        '-DCMAKE_CXX_FLAGS_DEBUG=/Z7 /Ob0 /Od /RTC1') 'configure Windows'
    foreach ($cfg in 'Debug', 'Release') {
        Invoke-Checked 'cmake' @('--build', $winB, '--config', $cfg, '--', '/m', '/v:minimal', '/nologo') "build Windows $cfg"
        $libDir = Join-Path $stage "lib\windows-x64\$cfg"
        $binDir = Join-Path $stage "bin\windows-x64\$cfg"
        New-Item -ItemType Directory -Force -Path $libDir, $binDir | Out-Null
        foreach ($l in $staticLibs) { Copy-Item (Join-Path $winB "$cfg\$l.lib") $libDir }
        # import library in its own folder: Windows file names ignore case, SoftFieldbus.lib = softfieldbus.lib
        $impDir = Join-Path $stage "lib\windows-x64\shared\$cfg"
        New-Item -ItemType Directory -Force -Path $impDir | Out-Null
        Copy-Item (Join-Path $winB "shared-lib\$cfg\SoftFieldbus.lib") $impDir
        Copy-Item (Join-Path $winB "bin\$cfg\SoftFieldbus.dll") $binDir
        $pdb = Join-Path $winB "bin\$cfg\SoftFieldbus.pdb" # Debug only
        if (Test-Path $pdb) { Copy-Item $pdb $binDir }
        if ($cfg -eq 'Release') {
            foreach ($t in $tools) { Copy-Item (Join-Path $winB "bin\$cfg\$t.exe") $binDir }
            $crt = Get-ChildItem -Path "${env:ProgramFiles}\Microsoft Visual Studio\18" -Recurse -Directory -Filter 'Microsoft.VC145.CRT' -ErrorAction SilentlyContinue |
                Where-Object { $_.FullName -match '\\x64\\Microsoft\.VC145\.CRT$' -and $_.FullName -notmatch 'onecore' } |
                Sort-Object FullName -Descending | Select-Object -First 1
            if ($crt) { Get-ChildItem -LiteralPath $crt.FullName -Filter '*.dll' | Copy-Item -Destination $binDir }
            else { Write-Warning 'Microsoft.VC145.CRT not found: the tools need the VC++ x64 redistributable on the target' }
        }
    }
    $msvc = '?'
    $ver = Get-ChildItem -Path (Join-Path $winB 'CMakeFiles') -Recurse -Filter 'CMakeCXXCompiler.cmake' | Select-Object -First 1 |
        Select-String -Pattern 'set\(CMAKE_CXX_COMPILER_VERSION "([^"]+)"\)' | Select-Object -First 1
    if ($ver) { $msvc = $ver.Matches[0].Groups[1].Value }
    $winInfo = "Visual Studio 2026, MSVC $msvc, Npcap SDK $(Split-Path $env:NPCAP_SDK -Leaf)"
}

# ---- Linux (WSL) ------------------------------------------------------------------------------------------
$linuxInfo = 'not built'
if (-not $SkipLinux) {
    $wTar = ConvertTo-WslPath $srcTar
    $wStage = ConvertTo-WslPath $stage
    $toolList = $tools -join ' '
    Invoke-Wsl @"
W=~/softfieldbus-sdk-build
rm -rf `$W && mkdir -p `$W/src && tar -xf '$wTar' -C `$W/src
cmake -S `$W/src -B `$W/build -DCMAKE_BUILD_TYPE=Release -DSOFTFIELDBUS_SHARED=ON -DCMAKE_POSITION_INDEPENDENT_CODE=ON > `$W/configure.log
cmake --build `$W/build -j`$(nproc) > `$W/build.log 2>&1 || { tail -40 `$W/build.log; exit 1; }
grep -E 'warning|error' `$W/build.log || true
mkdir -p '$wStage/lib/linux-x64' '$wStage/bin/linux-x64'
for l in $($staticLibs -join ' '); do cp "`$(find `$W/build -name lib`$l.a | head -1)" '$wStage/lib/linux-x64/'; done
cp "`$(find `$W/build -name libSoftFieldbus.so | head -1)" '$wStage/lib/linux-x64/'
for t in $toolList; do cp `$W/build/bin/`$t '$wStage/bin/linux-x64/'; done
strip --strip-unneeded '$wStage'/lib/linux-x64/libSoftFieldbus.so '$wStage'/bin/linux-x64/*
echo "`$(g++ --version | head -1)" > `$W/gcc.txt
"@ 'build Linux (WSL)'
    $linuxInfo = (& wsl -d $Distro -- bash -lc 'cat ~/softfieldbus-sdk-build/gcc.txt').Trim() + " (WSL $Distro)"
}

# ---- VERSION ------------------------------------------------------------------------------------------------
@(
    'SoftFieldbus SDK',
    "commit   $commit ($branch)$(if ($WorkingTree) { ', working tree' })",
    "packed   $((Get-Date).ToString('yyyy-MM-dd HH:mm')) on $env:COMPUTERNAME",
    "windows  $winInfo",
    "linux    $linuxInfo"
) | Set-Content -LiteralPath (Join-Path $stage 'VERSION.txt') -Encoding utf8

# ---- Verify: build the samples from the package -----------------------------------------------------------
if (-not $SkipVerify) {
    if (-not $SkipWindows) {
        foreach ($shared in 'ON', 'OFF') {
            $exB = Join-Path $work "verify-win-$shared"
            Invoke-Checked 'cmake' @('-S', (Join-Path $stage 'examples'), '-B', $exB, '-G', $generator, '-A', 'x64',
                "-DSOFTFIELDBUS_USE_SHARED=$shared") "verify Windows: configure samples (shared $shared)"
            foreach ($cfg in 'Debug', 'Release') {
                Invoke-Checked 'cmake' @('--build', $exB, '--config', $cfg, '--', '/m', '/v:minimal', '/nologo') "verify Windows: build samples $cfg (shared $shared)"
                $out = & (Join-Path $exB "bin\$cfg\fb_device_demo.exe") --transport nope 2>&1
                if (-not ($out -match 'unknown transport')) { throw "verify Windows: fb_device_demo ($cfg, shared $shared) did not run: $out" }
            }
        }
        Write-Host 'verify Windows: samples build and run (shared and static, Debug and Release)'
    }
    if (-not $SkipLinux) {
        $wStage = ConvertTo-WslPath $stage
        Invoke-Wsl @"
for shared in ON OFF; do
  B=~/softfieldbus-sdk-build/verify-`$shared
  rm -rf `$B
  cmake -S '$wStage/examples' -B `$B -DCMAKE_BUILD_TYPE=Release -DSOFTFIELDBUS_USE_SHARED=`$shared > /dev/null
  cmake --build `$B -j`$(nproc) > `$B.log 2>&1 || { tail -40 `$B.log; exit 1; }
  grep -E 'warning|error' `$B.log || true
  out=`$(`$B/bin/fb_device_demo --transport nope 2>&1 || true)   # exits 2 by design
  echo "`$out" | grep -q 'unknown transport' || { echo "fb_device_demo (shared `$shared) did not run: `$out"; exit 1; }
  `$B/bin/pn_device_demo --list > /dev/null
done
echo 'verify Linux: samples build and run (shared and static)'
"@ 'verify Linux'
    }
}

# ---- Archives ------------------------------------------------------------------------------------------------
$zip = Join-Path $Destination "$name.zip"
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path $stage -DestinationPath $zip
$tgz = Join-Path $Destination "$name.tar.gz"
Invoke-Checked 'tar' @('-czf', $tgz, '-C', $Destination, $name) 'tar.gz (keeps the Linux executable bits)'
if (-not $SkipLinux) {
    # tar on Windows stores no execute bits: rewrite the tar.gz in WSL, where /mnt files are executable
    $wDest = ConvertTo-WslPath $Destination
    # /mnt files all look rwxrwxrwx: copy to the Linux file system, 644 for files, 755 for folders, Linux programs
    # and the .so, then pack
    Invoke-Wsl @"
P=~/softfieldbus-sdk-build/pack
rm -rf `$P && mkdir -p `$P && cp -r '$wDest/$name' `$P/
cd `$P/$name
find . -type d -exec chmod 755 {} + && find . -type f -exec chmod 644 {} +
chmod 755 bin/linux-x64/* lib/linux-x64/libSoftFieldbus.so
cd `$P && tar --owner=0 --group=0 -czf '$wDest/$name.tar.gz' '$name'
"@ 'tar.gz in WSL'
}

Write-Host ''
Write-Host "SDK folder: $stage"
Write-Host "Archives:   $zip"
Write-Host "            $tgz"

# Packs a test kit for the test VM: softpn_ndis driver package (sys/inf/cat), test certificate,
# softpn_ndis_test.exe and install / update / uninstall scripts. Run build.ps1 first.
#   .\pack_kit.ps1                                  # -> ..\..\build\softpn_ndis_kit
#   .\pack_kit.ps1 -Dest \\10.0.0.128\D\softpn_ndis_kit
# The VM needs test signing (bcdedit /set testsigning on, Secure Boot off).
param(
    [ValidateSet('Debug', 'Release')] [string] $Configuration = 'Debug',
    [string] $Dest = (Join-Path $PSScriptRoot '..\..\build\softpn_ndis_kit')
)
$ErrorActionPreference = 'Stop'
$sys = Join-Path $PSScriptRoot "sys\x64\$Configuration"
$pkg = Join-Path $sys 'softpn_ndis'
$tool = Join-Path $PSScriptRoot "test\x64\$Configuration\softpn_ndis_test.exe"
$files = @("$pkg\softpn_ndis.sys", "$pkg\softpn_ndis.inf", "$pkg\softpn_ndis.cat", "$sys\softpn_ndis.cer", $tool)
foreach ($f in $files) {
    if (-not (Test-Path $f)) { throw "missing $f (run build.ps1 -Configuration $Configuration)" }
}

New-Item -ItemType Directory -Force $Dest | Out-Null
Copy-Item $files $Dest -Force
# Symbols for WinDbg on the debugger host (Driver Verifier runs): sym\softpn_ndis.pdb matches this .sys.
New-Item -ItemType Directory -Force (Join-Path $Dest 'sym') | Out-Null
Copy-Item "$sys\softpn_ndis.pdb" (Join-Path $Dest 'sym') -Force

# The scripts are ASCII and use CRLF so cmd.exe reads them correctly.
$install = @'
@echo off
rem Installs softpn_ndis (test-signed). Run as administrator in the test VM only.
cd /d "%~dp0"
rem bcdedit localizes Yes/No (Si/Ja, No/Nein): require the entry and reject the "No" values.
bcdedit /enum {current} | find /i "testsigning" >nul || (echo Test signing is off: bcdedit /set testsigning on, reboot & exit /b 1)
bcdedit /enum {current} | find /i "testsigning" | findstr /i /r /c:" No *$" /c:" Nein *$" /c:" Non *$" >nul && (echo Test signing is off: bcdedit /set testsigning on, reboot & exit /b 1)
certutil -f -addstore Root softpn_ndis.cer >nul || exit /b 1
certutil -f -addstore TrustedPublisher softpn_ndis.cer >nul || exit /b 1
netcfg -v -l softpn_ndis.inf -c p -i UV_SoftPnNdis || exit /b 1
net start SoftPnNdis
sc query SoftPnNdis | find "STATE"
echo.
echo Next: softpn_ndis_test list   /   softpn_ndis_test selftest 0
'@
$uninstall = @'
@echo off
rem Removes softpn_ndis. Run as administrator.
net stop SoftPnNdis
netcfg -v -u UV_SoftPnNdis
'@
$update = @'
@echo off
rem Replaces an installed softpn_ndis with the files in this folder. Run as administrator.
cd /d "%~dp0"
call uninstall.cmd
call install.cmd
'@
$verifier = @'
@echo off
rem Enables Driver Verifier (standard checks) for softpn_ndis.sys.
rem Needs a reboot. A driver bug then stops the VM with a bugcheck: attach WinDbg first.
rem Turn off again with: verifier /reset  (and reboot)
verifier /standard /driver softpn_ndis.sys
verifier /querysettings
echo Reboot the VM to activate.
'@
foreach ($s in @(@('install.cmd', $install), @('uninstall.cmd', $uninstall), @('update.cmd', $update), @('verifier.cmd', $verifier))) {
    Set-Content -Path (Join-Path $Dest $s[0]) -Value ($s[1] -replace "`r?`n", "`r`n") -Encoding ascii
}
Get-ChildItem $Dest | Select-Object Name, Length

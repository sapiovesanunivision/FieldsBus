@echo off
rem Generates build\vs2026\SoftFieldbus.sln (+ .vcxproj) for Visual Studio 2026.
rem Run from a "Developer Command Prompt for VS 2026" or with CMake 4.2+ on PATH.
setlocal
cd /d "%~dp0"
cmake --preset vs2026 || goto :error
echo.
echo Solution generated: %~dp0build\vs2026\SoftFieldbus.sln
if /i "%1"=="open" start "" "%~dp0build\vs2026\SoftFieldbus.sln"
exit /b 0
:error
echo.
echo CMake failed. "Visual Studio 18 2026" generator needs CMake 4.2 or newer:
echo   cmake --version
echo Fallback for VS 2022:  cmake --preset vs2022
exit /b 1

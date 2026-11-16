@echo off
rem This Source Code Form is subject to the terms of the Mozilla Public
rem License, v. 2.0. If a copy of the MPL was not distributed with this
rem file, You can obtain one at https://mozilla.org/MPL/2.0/.
rem
rem Builds Prism.exe into dist\ using Visual Studio's C++ tools.
rem Run it from anywhere: scripts\build.cmd

setlocal
cd /d "%~dp0.."

rem Find Visual Studio (or the Build Tools) with the C++ workload.
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo Visual Studio wasn't found. Install "Visual Studio Build Tools" with the
    echo "Desktop development with C++" workload: https://visualstudio.microsoft.com/downloads/
    exit /b 1
)
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH (
    echo Visual Studio is installed, but without C++ support. Open the Visual Studio
    echo Installer and add the "Desktop development with C++" workload.
    exit /b 1
)

rem Set up the 64-bit C++ compiler (and Visual Studio's bundled CMake and Ninja).
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release || exit /b 1
cmake --build build || exit /b 1

echo.
echo Prism.exe is in dist\
if not exist "dist\WebKit2.dll" (
    echo The WebKit engine isn't there yet. Get it with:
    echo     powershell -ExecutionPolicy Bypass -File scripts\fetch-webkit.ps1
) else (
    echo Run dist\Prism.exe
)

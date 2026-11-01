@echo off
rem Builds WT:VR's 32-bit d3d9.dll with MSVC.
rem Run from "x86 Native Tools Command Prompt for VS" (GHWT is 32-bit). Needs git for the OpenXR headers,
rem or set OPENXR_INCLUDE to an OpenXR-SDK "include" folder.
cd /d "%~dp0"
if "%OPENXR_VERSION%"=="" set OPENXR_VERSION=release-1.1.65
if "%OPENXR_INCLUDE%"=="" set OPENXR_INCLUDE=openxr-sdk\include
if "%WTVR_VERSION%"=="" set WTVR_VERSION=dev
if not exist "%OPENXR_INCLUDE%\openxr\openxr.h" (
    echo Fetching the OpenXR headers into openxr-sdk
    git clone --depth 1 --branch %OPENXR_VERSION% https://github.com/KhronosGroup/OpenXR-SDK.git openxr-sdk || exit /b 1
    set OPENXR_INCLUDE=openxr-sdk\include
)
if not exist dist mkdir dist
cl /nologo /LD /O2 /W3 /EHsc /std:c++17 /DWTVR_VERSION=\"%WTVR_VERSION%\" /I"%OPENXR_INCLUDE%" src\*.cpp /Fo:dist\ ^
   /link /DEF:src\d3d9.def /OUT:dist\d3d9.dll user32.lib
if errorlevel 1 exit /b 1
echo Built dist\d3d9.dll
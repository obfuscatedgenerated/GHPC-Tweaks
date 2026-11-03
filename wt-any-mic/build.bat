@echo off
rem Builds the 32-bit dsound.dll proxy with MSVC.
rem Run from "x86 Native Tools Command Prompt for VS" (GHWT is 32-bit).
cd /d "%~dp0"
if not exist dist mkdir dist
cl /nologo /LD /O2 /W4 /EHsc src\dsound_proxy.cpp /Fo:dist\ /link /DEF:src\dsound_proxy_msvc.def /OUT:dist\dsound.dll
if errorlevel 1 exit /b 1
echo Built dist\dsound.dll

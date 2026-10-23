@echo off
rem Builds the 32-bit proxy with MSVC.
rem Run from "x86 Native Tools Command Prompt for VS" (GH3 is 32-bit).
cd /d "%~dp0"
if not exist dist mkdir dist
cl /nologo /LD /O2 /W4 /EHsc src\xinput_proxy.cpp /Fo:dist\ /link /DEF:src\xinput_proxy_msvc.def /OUT:dist\xinput1_3.dll user32.lib
if errorlevel 1 exit /b 1
copy /Y dist\xinput1_3.dll dist\xinput1_2.dll >nul
copy /Y dist\xinput1_3.dll dist\xinput1_1.dll >nul
echo Built dist\xinput1_3.dll, xinput1_2.dll, xinput1_1.dll

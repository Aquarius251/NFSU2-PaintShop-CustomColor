@echo off
rem Builds the release plugin (needs Visual Studio 2019 Build Tools, x86)
call "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul
cd /d "%~dp0"
if not exist build mkdir build
cl /nologo /O2 /W3 /MT /LD /DNFSU2CC_SHIP src\customcolor.c /Fo:build\ /Fe:build\NFSU2_CustomColor.asi /link user32.lib kernel32.lib gdi32.lib

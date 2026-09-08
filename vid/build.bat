@echo off
setlocal
cd /d "%~dp0" || exit /b 1

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH (
    echo Visual Studio not found.
    exit /b 1
)
rem x86: gta2.exe is a 32-bit process and every Vid_ export is __stdcall.
call "%VSPATH%\VC\Auxiliary\Build\vcvars32.bat" >nul || exit /b 1

if not exist build mkdir build
rem The .def exports the plain names: __stdcall would otherwise decorate them as
rem _Vid_CheckMode@16 and GTA2 looks them up by the undecorated name.
cl /nologo /std:c++17 /EHsc /O2 /W3 /MT /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS ^
   /LD /Fe:build\gta2dx9_vid.dll /Fo:build\ ^
   src\vid_proxy.cpp ^
   /link /DEF:dmavideo.def || exit /b 1

echo Built build\gta2dx9_vid.dll

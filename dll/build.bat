@echo off
setlocal
rem Paths below are relative, so run from this script's own directory whatever
rem the caller's happens to be.
cd /d "%~dp0" || exit /b 1

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH (
    echo Visual Studio not found.
    exit /b 1
)
rem x86: gta2.exe is a 32-bit process, and the naked thunks are x86 inline asm.
call "%VSPATH%\VC\Auxiliary\Build\vcvars32.bat" >nul || exit /b 1

if not exist build mkdir build
rem The world renderer, mesh builder and file loaders are shared verbatim with the
rem standalone viewer in ..\src.
rem No d3d9.lib on the link line: the renderer resolves Direct3DCreate9 with
rem LoadLibrary so the RTX Remix bridge client does not start under the loader lock.
cl /nologo /std:c++17 /EHsc /O2 /W3 /MT /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS ^
   /LD /Fe:build\gta2dx9.dll /Fo:build\ ^
   src\dllmain.cpp src\world_view.cpp src\log.cpp src\texture_store.cpp src\overlay.cpp ^
   src\live_geometry.cpp ^
   ..\src\camera.cpp ..\src\gta2_map.cpp ..\src\gta2_style.cpp ..\src\renderer.cpp ..\src\world_mesh.cpp ^
   /link /DEF:gta2dx9.def user32.lib psapi.lib || exit /b 1

echo Built build\gta2dx9.dll

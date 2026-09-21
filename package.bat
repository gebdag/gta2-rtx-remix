@echo off
rem Builds both DLLs and assembles release\ - exactly what a player unzips.
rem
rem Nothing in the release shares a name with a GTA2 file except the optional
rem d3ddll.dll, which is the same renderer under the one name the GTA2 manager
rem can be made to pick. See package\README.txt.
setlocal
cd /d "%~dp0" || exit /b 1

call "%~dp0dll\build.bat" || exit /b 1
call "%~dp0vid\build.bat" || exit /b 1

set "OUT=%~dp0release"
if exist "%OUT%" rmdir /s /q "%OUT%"
mkdir "%OUT%" || exit /b 1
mkdir "%OUT%\optional" || exit /b 1

copy /y "%~dp0dll\build\gta2dx9.dll"     "%OUT%\gta2dx9.dll"          >nul || exit /b 1
copy /y "%~dp0vid\build\gta2dx9_vid.dll" "%OUT%\gta2dx9_vid.dll"      >nul || exit /b 1
rem The same renderer again, under the name the manager can pick. Opt-in,
rem because it is the only file here that overwrites one of GTA2's.
copy /y "%~dp0dll\build\gta2dx9.dll"     "%OUT%\optional\d3ddll.dll"  >nul || exit /b 1

copy /y "%~dp0package\gta2dx9.ini"   "%OUT%\" >nul || exit /b 1
copy /y "%~dp0package\install.reg"   "%OUT%\" >nul || exit /b 1
copy /y "%~dp0package\uninstall.reg" "%OUT%\" >nul || exit /b 1
rem Not README.txt: GTA2 ships a readme.txt and Windows would treat the two as
rem the same file, so unzipping would overwrite the game's.
copy /y "%~dp0package\gta2dx9_README.txt" "%OUT%\" >nul || exit /b 1
rem What passing the DLLs on requires: our own licence, and the notices of the
rem code compiled into them. Prefixed for the same reason as the readme.
copy /y "%~dp0LICENSE"                         "%OUT%\gta2dx9_LICENSE.txt" >nul || exit /b 1
copy /y "%~dp0package\gta2dx9_THIRD_PARTY.txt" "%OUT%\"                    >nul || exit /b 1

rem The particle-type bindings are hand-found - which id is fire, which is a
rem muzzle flash - and a player cannot reasonably rediscover them, so the tuned
rem file ships. The light tuning goes with it; both are overwritten the moment
rem anyone presses Save in the F4 menu.
if exist "%~dp0package\tuning\gta2dx9_effects.ini" (
    copy /y "%~dp0package\tuning\gta2dx9_effects.ini" "%OUT%\" >nul || exit /b 1
)
if exist "%~dp0package\tuning\gta2dx9_settings.ini" (
    copy /y "%~dp0package\tuning\gta2dx9_settings.ini" "%OUT%\" >nul || exit /b 1
)

echo.
echo Release assembled in %OUT%
dir /b "%OUT%"
echo   optional\
dir /b "%OUT%\optional"

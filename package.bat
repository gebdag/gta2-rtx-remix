@echo off
rem Builds both DLLs and assembles release\ - exactly what a player unzips.
rem
rem Three files in the release share a name with a GTA2 file, deliberately:
rem d3ddll.dll, Dmavideo.dll and DMAGlide.dll, the renderer and the video device
rem under the names GTA2 loads out of the box. Unzipping is then the whole
rem install, and nothing the GTA2 manager writes to the registry can undo it. See
rem package\gta2dx9_README.txt.
setlocal
cd /d "%~dp0" || exit /b 1

call "%~dp0dll\build.bat" || exit /b 1
call "%~dp0vid\build.bat" || exit /b 1

set "OUT=%~dp0release"
if exist "%OUT%" rmdir /s /q "%OUT%"
mkdir "%OUT%" || exit /b 1

copy /y "%~dp0dll\build\gta2dx9.dll"     "%OUT%\gta2dx9.dll"          >nul || exit /b 1
copy /y "%~dp0vid\build\gta2dx9_vid.dll" "%OUT%\gta2dx9_vid.dll"      >nul || exit /b 1
rem The same two again under GTA2's own names. Neither needs the file it
rem replaces: the renderer's takeover mode never loads the original, and the
rem video device is standalone. Without these a player who skips install.reg -
rem or runs the GTA2 manager afterwards - gets "Videomode 16x16x16 is not
rem available" from the game's own video device.
copy /y "%~dp0dll\build\gta2dx9.dll"     "%OUT%\d3ddll.dll"          >nul || exit /b 1
copy /y "%~dp0vid\build\gta2dx9_vid.dll" "%OUT%\Dmavideo.dll"        >nul || exit /b 1
rem And the video device once more, as the game's Glide one. The 11.44 GTA2
rem manager picks DMAGlide.dll whenever it finds a glide2x.dll, which any Glide
rem wrapper provides; the wrapper then draws through Direct3D 9, so Remix gets a
rem second full-screen device beside ours - a hang, or tearing and a flickering
rem image with frame generation on. Ours under that name too leaves one device
rem whatever the manager chose.
copy /y "%~dp0vid\build\gta2dx9_vid.dll" "%OUT%\DMAGlide.dll"        >nul || exit /b 1

copy /y "%~dp0package\gta2dx9.ini"   "%OUT%\" >nul || exit /b 1
copy /y "%~dp0package\install.reg"   "%OUT%\" >nul || exit /b 1
copy /y "%~dp0package\uninstall.reg" "%OUT%\" >nul || exit /b 1
rem The bridge keeps the Remix API switched off unless .trex\bridge.conf turns
rem it on, and no Remix release ships that file - so without this the renderer
rem gets NOT_INITIALIZED and the city has no lights of its own.
mkdir "%OUT%\.trex" || exit /b 1
copy /y "%~dp0package\bridge.conf" "%OUT%\.trex\" >nul || exit /b 1
rem The Remix settings GTA2 needs. Kept in package\ like everything else here:
rem release\ is wiped at the top of this script, so a file only placed there
rem does not survive the next build.
copy /y "%~dp0package\rtx.conf" "%OUT%\" >nul || exit /b 1
rem The Remix mods: gta2rtx, emissive maps for the game's own textures, as one
rem replacement layer (mod.usd) and the ingested masks beside it; gta2water, the
rem animated water. The water's normal map is over the 1 MB the repository
rem takes, so it is not checked in - tools\make_water_mod.py builds it - and a
rem release without it would reference a texture that is not there.
if not exist "%~dp0package\rtx-remix\mods\gta2water\SubUSDs\water_normal.n.dds" (
    echo package\rtx-remix\mods\gta2water\SubUSDs\water_normal.n.dds is missing.
    echo Build it with tools\make_water_mod.py before packaging.
    exit /b 1
)
xcopy "%~dp0package\rtx-remix" "%OUT%\rtx-remix\" /e /i /q /y >nul || exit /b 1
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

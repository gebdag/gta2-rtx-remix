GTA2 in Direct3D 9, for RTX Remix
=================================

A DX9 renderer for GTA2 that allows for path tracing via RTX Remix. 

Features:
- translation of game lights
- generating new lights off of game events
- dynamic time of day (Remix Plus required) 

The mod also comes with emissive maps for some of the games textures.

What you need
-------------

* GTA2 v9.6 - available for free online
* RTX Remix for x86 games, installed in the GTA2 folder. Remix Plus 1.5 or newer
  is recommended - get Remix_Plus_v1.5.1_x86_games_release.zip from
  https://github.com/RemixProjGroup/dxvk-remix/releases/tag/remix-plus-1.5.1
  NVIDIA's own Remix and Remix Plus 1.4 also work, with limits: NVIDIA's has
  no day/night cycle, and on any version other than Remix Plus 1.5.1 the menus
  and HUD may not display correctly.
* Windows 10 or 11, 64-bit.


Installing
----------

1. Back up d3ddll.dll and Dmavideo.dll from your GTA2 folder. The next step
   replaces them, and you need the originals to uninstall.

2. Unzip everything into your GTA2 folder (the one with gta2.exe) and let it
   replace those two files.

3. Start gta2.exe. Press F4 in game for the settings panel.


Without replacing GTA2's files
------------------------------

Skip the two files in step 2 and run install.reg instead (needs administrator
rights). The GTA2 manager and the in-game options screen undo this whenever you
change the resolution; run install.reg again when that happens.


Uninstalling
------------

1. Put your backed-up d3ddll.dll and Dmavideo.dll back.
2. Run uninstall.reg.
3. Delete the gta2dx9* files and the rtx-remix\mods\gta2rtx folder.
4. If you are removing RTX Remix as well, delete rtx.conf and .trex\bridge.conf.


Troubleshooting
---------------

gta2dx9.log and gta2dx9_vid.log beside gta2.exe say what happened.

* "Videomode 16x16x16 is not available": GTA2's own Dmavideo.dll is still in
  place. Copy the one from this zip over it.
* No gta2dx9.log at all: GTA2's own d3ddll.dll is still in place. Copy the one
  from this zip over it.
* "Remix API is switched off" in the F4 menu: .trex\bridge.conf is missing.
  Copy it back from this zip.
* "unrecognised Remix API table" in the F4 menu: this Remix build is not
  supported. The game runs without street lights. Use Remix Plus 1.5 or newer.
* Menus or HUD missing or drawn wrong: use Remix Plus 1.5.1.


Files
-----

    d3ddll.dll, gta2dx9.dll        the renderer
    Dmavideo.dll, gta2dx9_vid.dll  the video device
    gta2dx9.ini                    settings (required)
    gta2dx9_effects.ini            particle effect lights
    gta2dx9_settings.ini           light tuning, written by the F4 menu
    rtx.conf                       Remix settings for GTA2
    .trex\bridge.conf              turns on the Remix API
    rtx-remix\mods\gta2rtx         the emissive maps
    install.reg, uninstall.reg     registry setup, see above
    gta2dx9_LICENSE.txt            MIT licence
    gta2dx9_THIRD_PARTY.txt        third-party notices

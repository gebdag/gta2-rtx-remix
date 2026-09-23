GTA2 in Direct3D 9, for RTX Remix
=================================

GTA2 draws in software or through 3dfx, and neither gives RTX Remix anything to
work with. This replaces the game's renderer with one that builds the city as
real 3D geometry in Direct3D 9 - so Remix sees a world it can path trace, with
the game's own street lights in it - and its video device with a shim that stops
DirectDraw taking the display over.

It does not include RTX Remix. You supply that.


What you need
-------------

* GTA2. The free v9.6 release is what this was built and tested against.
* The RTX Remix runtime, installed into the GTA2 folder as its instructions say.
* Windows 10 or 11, 64-bit.


Installing
----------

1. Unzip everything in this folder into your GTA2 folder - the one with
   gta2.exe. Nothing here shares a name with a GTA2 file, so nothing of the
   game's is overwritten. (This file is gta2dx9_README.txt rather than
   README.txt for exactly that reason - GTA2 has a readme.txt of its own.)

2. Run install.reg and accept the prompt. It needs administrator rights,
   because GTA2 keeps its settings under HKEY_LOCAL_MACHINE.

3. Rename Remix's d3d9.dll to d3d9_remix.dll.

   This is worth doing even though Remix's own instructions do not mention it.
   Under the plain name it gets in front of everything in the process that asks
   Windows for Direct3D 9 - GTA2's own startup included - so Remix comes up on a
   device the game never draws through, and the renderer's device becomes a
   second one Remix cannot drive. Renamed, only the renderer finds it. The
   renderer looks for d3d9_remix.dll first and falls back to d3d9.dll, so the
   plain name still works if you would rather not.

4. Start gta2.exe.

Press F4 in game for the settings panel: brightness, the day/night clock, the
frame cap, how sprites sit on the road. Tick "Advanced" in its title bar for the
diagnostics.


Optional: d3ddll.dll and Dmavideo.dll
-------------------------------------

The GTA2 manager and the in-game options screen rewrite the renderer and video
device settings whenever you touch the resolution, and they can only write names
GTA2 has hardcoded. If that happens the mod stops loading - the renderer quietly,
and the video device loudly: GTA2's own asks DirectDraw for 16-bit colour, which
modern displays no longer offer, and the game will not start at all:

    Videomode 16x16x16 is not available

The two files in the "optional" folder guard against that. They are the same
renderer and video device under the names GTA2 falls back to, so whatever the
manager writes, the game still loads this mod and still boots. The video device
does not need GTA2's own for anything, so replacing it costs nothing.

Both OVERWRITE a GTA2 file. Back the originals up first - there is no other copy
of them, and getting them back otherwise means reinstalling the game.

If you skip them and the mod does stop loading after a visit to the manager,
just run install.reg again.


Uninstalling
------------

Run uninstall.reg, then delete the files this added:

    gta2dx9.dll  gta2dx9_vid.dll  gta2dx9.ini  gta2dx9_effects.ini
    gta2dx9_settings.ini  gta2dx9.log  gta2dx9_vid.log
    gta2dx9_README.txt  gta2dx9_LICENSE.txt  gta2dx9_THIRD_PARTY.txt

If you copied either optional file, delete it and put GTA2's own back.


If it does not start
--------------------

gta2dx9.log and gta2dx9_vid.log are written beside gta2.exe every run and say
what happened. A few things they tend to say:

* "mode=proxy" - gta2dx9.ini is missing. It is not optional; the built-in
  defaults are the old behaviour.

* "Remix API unavailable" in the F4 menu, and the game looks like plain GTA2 -
  Remix is not loading. Check step 3.

* Nothing in the log at all - the registry did not take. install.reg needs
  administrator rights; check that rendername under
  HKLM\SOFTWARE\WOW6432Node\DMA Design Ltd\GTA2\Screen really says gta2dx9.dll.

* "Videomode 16x16x16 is not available" - GTA2's own video device is loaded,
  not this one, and it wants a 16-bit display mode your display does not have.
  Copy optional\Dmavideo.dll into the GTA2 folder (see "Optional" above), or run
  install.reg again. gta2dx9_vid.log is not written in this case, because the
  file that writes it was never loaded.

* It worked, then you used the GTA2 manager or the in-game options screen, and
  now it does not - or the game will not start at all. Those screens rewrite the
  renderer and video device settings to GTA2's own. Run install.reg again, or
  use the optional files.

* A previous install of dxwrapper (a ddraw.dll in the GTA2 folder) is not needed
  and gets in the way. Rename or delete it; the video device here does the job it
  was there for.


What the files are
------------------

    gta2dx9.dll           the renderer
    gta2dx9_vid.dll       the video device shim
    gta2dx9.ini           settings, required, commented
    gta2dx9_effects.ini   which particle type is fire, sparks, gunfire and so on
    gta2dx9_settings.ini  light tuning; delete it for the built-in defaults
    gta2dx9_README.txt    this file
    gta2dx9_LICENSE.txt   the licence this is shared under (MIT)
    gta2dx9_THIRD_PARTY.txt  notices for the code built into gta2dx9.dll
    install.reg           points GTA2 at the two DLLs
    uninstall.reg         points it back at its own
    optional/d3ddll.dll   see above
    optional/Dmavideo.dll see above

The two ini files the F4 menu writes are yours to keep or delete - the Save
button in the menu writes them, and nothing is lost by starting again without
them.

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
* The RTX Remix runtime for x86 games, installed into the GTA2 folder as its
  instructions say. It has to speak Remix API 0.1000: Remix Plus 1.5.0 or
  newer. Remix Plus 1.4.x (API 0.6) runs, but the renderer cannot place a
  single light through it, and says so in the F4 menu.
* Windows 10 or 11, 64-bit.


Installing
----------

1. Back up two files from your GTA2 folder - the one with gta2.exe:

       d3ddll.dll     GTA2's Direct3D renderer
       Dmavideo.dll   GTA2's video device

   Copy them somewhere safe. Step 2 replaces both, and there is no other copy of
   them short of reinstalling the game. You only need them to uninstall.

2. Unzip everything in this folder into your GTA2 folder, and let it replace
   d3ddll.dll and Dmavideo.dll when Windows asks. Nothing else here shares a name
   with a GTA2 file. (This file is gta2dx9_README.txt rather than README.txt for
   that reason - GTA2 has a readme.txt of its own.)

   Those two are the whole install. They are this renderer and this video device
   under the names GTA2 loads out of the box, so the game picks them up without
   any setup - and keeps picking them up whatever the GTA2 manager or the
   in-game options screen do to its settings, because those can only ever write
   GTA2's own names back.

   The video device is the one that matters. GTA2's own asks DirectDraw for
   16-bit colour, which modern displays no longer offer, and refuses to start:

       Videomode 16x16x16 is not available

   This one never asks for a display mode at all, and needs nothing of the
   original's.

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


If you would rather not replace GTA2's files
--------------------------------------------

Skip them in step 2 and run install.reg instead, accepting the prompt. It points
GTA2 at gta2dx9.dll and gta2dx9_vid.dll, which are the same two files under names
of their own. It needs administrator rights, because GTA2 keeps its settings
under HKEY_LOCAL_MACHINE.

The catch is that the GTA2 manager and the in-game options screen write GTA2's
own names back whenever you touch the resolution, and then the game stops at
"Videomode 16x16x16 is not available" again. Run install.reg again when that
happens.


Uninstalling
------------

Put your backed-up d3ddll.dll and Dmavideo.dll back, run uninstall.reg, then
delete the files this added:

    gta2dx9.dll  gta2dx9_vid.dll  gta2dx9.ini  gta2dx9_effects.ini
    gta2dx9_settings.ini  gta2dx9.log  gta2dx9_vid.log
    gta2dx9_README.txt  gta2dx9_LICENSE.txt  gta2dx9_THIRD_PARTY.txt

Without the two originals GTA2 still runs this mod, because the replacements are
this mod. Reinstalling the game is the other way back.


If it does not start
--------------------

gta2dx9.log and gta2dx9_vid.log are written beside gta2.exe every run and say
what happened. A few things they tend to say:

* "mode=proxy" - gta2dx9.ini is missing. It is not optional; the built-in
  defaults are the old behaviour.

* "Remix API unavailable" in the F4 menu, and the game looks like plain GTA2 -
  Remix is not loading. Check step 3.

* "Remix API is switched off" in the F4 menu - .trex\bridge.conf is missing or
  does not say exposeRemixApi = True. The zip ships one; if you installed Remix
  after this mod and it replaced the .trex folder, copy it back.

* "built for an older Remix API" in the F4 menu - the Remix install is Remix
  Plus 1.4.x or older. Install 1.5.0 or newer.

* Nothing in the log at all - this renderer is not the one being loaded.
  Check that d3ddll.dll in the GTA2 folder is the one from this zip (it is
  about 900 KB; GTA2's own is 80 KB), or, if you went the install.reg way, run
  it again.

* "Videomode 16x16x16 is not available" - GTA2's own video device is loaded,
  not this one, and it wants a 16-bit display mode your display does not have.
  Dmavideo.dll in the GTA2 folder is still the game's (60 KB; this one is about
  135 KB) - copy the one from this zip over it. gta2dx9_vid.log is not written
  in this case, because the file that writes it was never loaded.

* It worked, then you used the GTA2 manager or the in-game options screen, and
  now it does not. Those screens write GTA2's own renderer and video device
  names back. With the two files from step 2 in place that is harmless; if you
  went the install.reg way instead, run it again.

* A previous install of dxwrapper (a ddraw.dll in the GTA2 folder) is not needed
  and gets in the way. Rename or delete it; the video device here does the job it
  was there for.


What the files are
------------------

    d3ddll.dll            the renderer, under the name GTA2 loads
    Dmavideo.dll          the video device, under the name GTA2 loads
    gta2dx9.dll           the renderer again, for install.reg
    gta2dx9_vid.dll       the video device again, for install.reg
    gta2dx9.ini           settings, required, commented
    gta2dx9_effects.ini   which particle type is fire, sparks, gunfire and so on
    gta2dx9_settings.ini  light tuning; delete it for the built-in defaults
    gta2dx9_README.txt    this file
    gta2dx9_LICENSE.txt   the licence this is shared under (MIT)
    gta2dx9_THIRD_PARTY.txt  notices for the code built into gta2dx9.dll
    .trex/bridge.conf     switches on the Remix API the lights go through
    install.reg           points GTA2 at gta2dx9.dll and gta2dx9_vid.dll
    uninstall.reg         points it back at its own names

The two ini files the F4 menu writes are yours to keep or delete - the Save
button in the menu writes them, and nothing is lost by starting again without
them.

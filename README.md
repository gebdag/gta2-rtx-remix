# GTA2 RTX Remix

A Direct3D 9 renderer for GTA2, built for RTX Remix.

GTA2 draws in software or through 3dfx, and neither gives Remix anything to path trace.
This replaces the game's renderer DLL with one that runs inside `gta2.exe` and builds the
city as real 3D world-space geometry from the game's live map, and replaces its video
device with a shim that stops DirectDraw taking over the display.

## Features

- World-space map geometry, including slopes, partial and corner blocks
- Cars, pedestrians and objects as 3D quads with per-sprite transforms, so Remix gets
  stable geometry and motion vectors
- HUD and menus drawn as a 2D pass on top
- GTA2's own map, vehicle and effect lights injected through the Remix API
- Day/night cycle driving Remix's sun, with lights that switch on at dusk
- Additive fire and explosion sprites
- Frame rate cap (GTA2 advances one simulation step per frame, so this also sets game speed)
- F4 settings panel

## Requirements

- GTA2 (built and tested against the free v9.6 release)
- RTX Remix runtime
- Windows 10 or 11, 64-bit

## Installing

Download a release and follow `gta2dx9_README.txt` inside it. In short: unzip into the
GTA2 folder, run `install.reg`, rename Remix's `d3d9.dll` to `d3d9_remix.dll`, and start
`gta2.exe`.

## Building

Needs Visual Studio with the C++ x86 toolset.

```bat
package.bat
```

Builds `dll\build\gta2dx9.dll` and `vid\build\gta2dx9_vid.dll` and assembles the release
in `release\`. The two DLLs can also be built on their own with `dll\build.bat` and
`vid\build.bat`.

The RTX Remix API headers in `dll\deps\bridge_api` must match the Remix runtime the game
uses. See `dll\deps\README.md`.

## Repository layout

```
dll/        gta2dx9.dll, the renderer
vid/        gta2dx9_vid.dll, the video device shim
src/        map, style, mesh, camera and D3D9 renderer code used by the DLL
package/    release files: settings, registry files, player README, tuned ini files
tools/      development tools
docs/       research notes
```

Technical notes on the renderer are in `dll/README.md`.

## License

MIT, see [LICENSE](LICENSE). Third-party components are listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

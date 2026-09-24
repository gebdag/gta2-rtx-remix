# GTA2 RTX Remix

A Direct3D 9 renderer for GTA2, built for RTX Remix.

<img width="1920" height="1080" alt="Grand_Theft_Auto_2_Screenshot_2026 09 23_-_16 50 32 95" src="https://github.com/user-attachments/assets/7b79266b-ea32-4c36-9f7f-27b3fe5c1b00" />
<img width="1920" height="1080" alt="Grand Theft Auto 2 Screenshot 2026 09 24 - 11 43 30 47" src="https://github.com/user-attachments/assets/3383eb7b-310b-4c80-b5ed-2532902d1fb6" />
<img width="1920" height="1080" alt="Grand_Theft_Auto_2_Screenshot_2026 09 23_-_17 15 53 02" src="https://github.com/user-attachments/assets/85a7703d-f0af-4046-a4b4-790039f1cd68" />
<img width="1920" height="1080" alt="Grand_Theft_Auto_2_Screenshot_2026 09 23_-_17 07 52 11" src="https://github.com/user-attachments/assets/02661354-ce62-465c-9067-1e131fc70fe3" />
<img width="1920" height="1080" alt="Grand_Theft_Auto_2_Screenshot_2026 09 23_-_17 18 43 07" src="https://github.com/user-attachments/assets/a8848200-f95c-4827-9b54-4ce4013b5357" />



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

# GTA2 RTX Remix

<p align="center">
<img width="1920" alt="GTA2_RTX_logo" src="https://github.com/user-attachments/assets/6c8b00d6-1f01-4eb0-a44e-089d7aca9ed0" />
</p>

A Direct3D 9 renderer for GTA2, built for RTX Remix.

<img width="1920" height="1080" alt="Grand_Theft_Auto_2_Screenshot_2026 09 23_-_16 50 32 95" src="https://github.com/user-attachments/assets/7b79266b-ea32-4c36-9f7f-27b3fe5c1b00" />
<img width="1920" height="1080" alt="Grand Theft Auto 2 Screenshot 2026 09 24 - 11 43 30 47" src="https://github.com/user-attachments/assets/3383eb7b-310b-4c80-b5ed-2532902d1fb6" />
<img width="1920" height="1080" alt="Grand_Theft_Auto_2_Screenshot_2026 09 23_-_17 15 53 02" src="https://github.com/user-attachments/assets/85a7703d-f0af-4046-a4b4-790039f1cd68" />
<img width="1920" height="1080" alt="Grand_Theft_Auto_2_Screenshot_2026 09 23_-_17 07 52 11" src="https://github.com/user-attachments/assets/02661354-ce62-465c-9067-1e131fc70fe3" />
<img width="1920" height="1080" alt="Grand_Theft_Auto_2_Screenshot_2026 09 23_-_17 18 43 07" src="https://github.com/user-attachments/assets/a8848200-f95c-4827-9b54-4ce4013b5357" />


## Features

- Full RTX Remix compatible rendering of the game
- Translation of the games lights plus generating of lights from game events such as explosions, fires, gunfire
- Dynamic time of day (Remix Plus only)
- Emissive maps for some of the game's textures, shipped as a Remix mod
- Full 16:9: cars and pedestrians no longer pop in and out at the sides of the screen
- 60fps possible via FG
- F4 in game settings panel for adjusting lighting and time of day

## Requirements

- GTA2 (built and tested against the freely available v9.6 release)
- RTX Remix runtime
- Windows 10 or 11, 64-bit

## Installing

Download a release and follow `gta2dx9_README.txt` inside it. In short: unzip into the
GTA2 folder, run `install.reg` and start `gta2.exe`.

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

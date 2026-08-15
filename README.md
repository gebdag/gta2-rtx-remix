# gta2_dx9

A world-space D3D9 renderer for GTA2 geometry, built so RTX Remix can path trace it.

Loads the game's own `.gmp` map and `.sty` style files, turns the block map into true
3D world-space triangles, and submits them through the D3D9 fixed-function pipeline.

## Why this is not a port of the original renderer

GTA2 loads its renderer as a DLL (`d3ddll.dll`, `3dfx.dll`, `softdll.dll`) and resolves a
flat `gbh_*` C interface by name — no exe patching is needed to replace it. That makes the
`gbh_*` layer look like the natural place to hook, but it is the wrong layer for ray tracing.

Decompiling `d3ddll.dll` shows every draw path ends at:

```
DrawPrimitive(D3DPT_TRIANGLEFAN, 0x1C4, verts, 4, 8)   // gbh_DrawQuad
DrawPrimitive(D3DPT_TRIANGLELIST, 0x1C4, verts, 3, 8)  // gbh_DrawTriangle
```

`0x1C4` is `D3DFVF_XYZRHW | DIFFUSE | SPECULAR | TEX1`. The game does its own transform and
lighting and hands the renderer **pre-transformed screen-space vertices**; `gbh_SetCamera`
only stores four scalars into globals. Remix explicitly skips such draws:

> `[RTX-Compatibility-Info] Skipped drawcall, using pre-transformed vertices which isn't currently supported.`

So geometry has to be rebuilt from the game's real 3D world data, which is what this
renderer does. The block map is genuinely 3D (256x256x8 blocks with per-face tiles and
slope types), so no unprojection is involved anywhere.

## Remix compatibility

The conditions Remix checks, and how they are met:

| Remix requirement | How it is satisfied |
|---|---|
| Not pre-transformed | FVF is `XYZ \| NORMAL \| TEX1`, untransformed |
| Not orthographic | `PerspectiveFovLH`, so `proj[3][3] == 0` |
| Z-write on | `D3DRS_ZWRITEENABLE = TRUE` |
| No programmable shaders | Pure fixed-function |
| World transform set | `D3DTS_WORLD` / `VIEW` / `PROJECTION` all set each frame |

`rtx.orthographicIsUI` and `rtx.preTransformedVerticesIsUI` are pinned off in `rtx.conf` so
no frame can be misclassified as UI.

Lighting and shadows are left entirely to Remix; the renderer submits no lights and runs
with `D3DRS_LIGHTING = FALSE`.

One texture is created per tile rather than a shared atlas, so Remix gets a stable
per-tile hash to key asset replacements off.

## Coordinate frame

Left-handed, one block = one unit: **+X east, +Y up, +Z north**.

Map rows run north to south, so the row index is mirrored (`z = 256 - y - 1`) when building
geometry. Sharing the sign mirrors the entire city — visible as backwards road text.

## Build and run

Needs Visual Studio 2022 with the C++ workload. Builds x86, because `gta2.exe` is a 32-bit
process and the Remix bridge expects an x86 client.

```bash
build.bat
```

```bash
build\gta2_dx9.exe --map <GTA2 folder>\data\ste.gmp --style <GTA2 folder>\data\ste.sty
```

Options: `--width`, `--height`, `--cam X Y Z`, `--yaw`, `--pitch`, `--screenshot out.bmp`.

Controls: `WASD` move, `Q`/`E` down/up, hold right mouse to look, `Shift` to move faster,
`F1` wireframe, `F2` cycle cull mode, `Esc` quit.

To run under Remix, copy a matching Remix client `d3d9.dll` next to the exe and its runtime
into `.trex\`. Mismatched client/runtime versions abort with a version error in
`rtx-remix\logs\bridge32.log`.

## Game link

With `--attach` the renderer reads live world state out of a running `gta2.exe`
instead of a `.gmp` on disk, and follows the game's camera.

Addresses were recovered by decompiling the game's world render loop
(`FUN_00472110`) and its per-cell block draw (`FUN_00471f20`):

| What | Address | Notes |
|---|---|---|
| Camera struct pointer | `0x005E3CC4` | `+0x98` / `+0x9C` are camera X/Y |
| Camera scale factors | same struct | `+0xA0` / `+0xA4`, ~`1.0` and `0.8875` |
| Map object holder | `0x00662C08` | `*holder` is the map object; `0` in menus |
| Column pool pointer | map object `+0x40008` | |
| Block array pointer | map object `+0x4000C` | 12 bytes per block |
| Current level | `0x006633A0` | 0-7 |

World coordinates are **16.14 fixed point**, one unit per map block.

The game holds the map in exactly the DMAP file layout, so live memory and a
`.gmp` feed the same parser. This was verified by diffing the live block array
against every map file: 4000/4000 records matched `wil.gmp` byte-for-byte, and
the live read produces the same mesh statistics as loading the file.

The map object pointer changes when the player moves between districts, so it is
polled and the world is rebuilt when it changes. The district's style file is
identified by matching the live blocks against the `.gmp` files on disk, since
the game does not keep the style filename anywhere obvious.

### Running the link

Start GTA2 first, get into a level, then:

```bash
build\gta2_dx9.exe --attach
```

The window waits for the game to load a world and picks it up automatically.
`--distance` controls how far the camera sits from the game's camera point;
`--yaw` and `--pitch` set the viewing angle.

### Host setup this needed

Two changes were made to run the game alongside a second D3D device. Both are
reversible and backed up in the session scratchpad:

- **Registry ACL.** GTA2 creates keys under
  `HKLM\SOFTWARE\WOW6432Node\DMA Design Ltd` at startup and aborts if it cannot;
  that subtree is read-only for non-admins on modern Windows, so the game only
  ran elevated. The interactive user was granted write access on that one key.
  Prior ACL saved to `dma_key_acl_backup.sddl`.
- **Windowed mode.** `HKCU\...\GTA2\Screen\start_mode` was `1` (fullscreen);
  exclusive fullscreen makes any second `CreateDevice` fail with
  `D3DERR_DEVICELOST`. Set to `0`. Prior values in `gta2_screen_backup.txt`.

## Current scope

Implemented: block lids and walls, tile rotation/flip flags, flat-face decal offset, ramp
slope families (26 degree, 7.5 degree, 45 degree), per-tile alpha test from palette index 0.

Not yet implemented:
- Diagonal and partial-block slope types (45-63) fall back to full cubes. The game
  dispatches these to dedicated routines, which map the remaining families:
  `FUN_0046edd0` (45-48), `FUN_0046ee40` (49-52), `FUN_00471c30` (53-61).
- Sprites: cars, pedestrians, objects. These live in `SPRG`/`SPRX` in the style file and
  their placement comes from live game state, not the map file.
- Camera height and angle are fixed offsets from the game's camera point; the game's own
  zoom factors (camera struct `+0xA0`/`+0xA4`) are read but not yet applied.
- The camera Z/level follows `0x006633A0`, which is the level being drawn rather than the
  player's own level. Needs confirming against real gameplay.
- Mip chains; textures are single-level.
- Single-window operation. The renderer is a second process beside the game; folding it
  into an in-process `gbh_*` replacement DLL would remove the second window entirely.

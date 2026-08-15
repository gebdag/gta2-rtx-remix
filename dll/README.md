# gta2dx9.dll — in-process renderer

Replaces GTA2's `gbh_*` renderer DLL so rendering happens inside `gta2.exe`. This
removes the second-window problem: GTA2 stops simulating when it loses focus, so a
separate viewer window can never show live motion while you look at it.

## Modes

`gta2dx9.ini`, beside `gta2.exe`:

```ini
[renderer]
backend=3dfx.dll
mode=takeover        ; or "proxy"

[camera]
; tenths of a degree, because the ini API only reads integers
pitch_tenths=-900    ; -900 = straight down, matching GTA2's own view
fov_tenths=400
```

`proxy` is the default so a bad build can never leave the game unbootable.

## Takeover

Owns a D3D9 device on the game's own window and draws world-space geometry built
from the game's live map. The game's `gbh_DrawTile` / `gbh_DrawQuad` /
`gbh_DrawTriangle` stream is deliberately ignored — it is pre-transformed screen
space, which Remix skips.

The camera comes from two sources: `gbh_SetCamera` hands us the **visible tile
rectangle** every frame, which is exactly the game's zoom and centre, and the
camera struct gives sub-tile position. Nothing is reverse-projected.

Two camera problems worth remembering:

- **Vertical jitter** came from following the *top* of the column under the
  camera. The topmost block is a building roof, so the camera leapt whenever it
  crossed one. It now follows the bottom of the column (street level) and eases
  toward it, since ground height is a per-tile step function.
- **Looking straight down** makes a `+Y` up vector degenerate. `Camera::ViewMatrix`
  switches to a north up vector past 89.9°, so `pitch_tenths=-900` is valid.

## Earlier stage: transparent proxy

Every export forwards to the original renderer, so the game behaves exactly as before
while the call stream is logged to `gta2dx9.log` next to `gta2.exe`. This proves the hook
and shows which entry points carry the data the world-space renderer needs.

Verified working: the game boots and runs with the proxy in the chain, producing the real
per-frame sequence — `gbh_BeginScene` → `gbh_DrawTile` / `gbh_DrawTriangle` →
`gbh_EndScene`, alongside `gbh_SetCamera`, `gbh_AddLight`, and texture lock/unlock.

## Deployment

```bash
build.bat
```

Copy `build\gta2dx9.dll` and a `gta2dx9.ini` next to `gta2.exe`:

```ini
[renderer]
backend=3dfx.dll
```

Then point the game at it. The value that matters is under **HKLM** — the HKCU copy is
read for some settings but the renderer name is taken from HKLM, and setting only HKCU
silently does nothing:

```
HKLM\SOFTWARE\WOW6432Node\DMA Design Ltd\GTA2\Screen\rendername = gta2dx9.dll
```

`do_play_movie` must also be `0` under **HKLM**; with the intro enabled the game crashes
in `binkw32.dll` with an access violation before reaching gameplay.

## Things that cost a debugging cycle

- **The interface is `__stdcall`**, not `__cdecl` — the originals end in `ret 4`. Calling
  one as cdecl cleans the argument twice, trips the `/GS` cookie check on return and
  raises `__fastfail`, which exits with `0xC0000409` and **cannot** be caught by an
  exception filter, so the log just stops with no crash record. Naked `jmp` thunks are
  immune, since a jump forwards the frame either way; only typed wrappers need annotating.
- **All 40 resolved exports must exist** or startup aborts with "Can't Find Function
  Called ...". The list lives in `src/gbh_exports.h`.
- An exception filter installed in `DllMain` is replaced by the game's own during startup.
  Installing it from `gbh_InitDLL` runs later and wins.

## The screen-space pass

The world is not the whole picture. GTA2 transforms the HUD, the menus and **every
sprite** itself and pushes them through `gbh_DrawQuad`, `gbh_DrawTriangle`,
`gbh_DrawFlatRect` and `gbh_BlitImage`. Dropping that stream — which is what this DLL
originally did — is precisely why the menus, the HUD, and every car and pedestrian were
absent. `overlay.cpp` replays it as a 2D pass drawn over the world.

The vertex layout is not guesswork; it comes from `d3ddll.dll!FUN_00e02cc0`, the shared
body behind all of those entry points. Eight floats per vertex, `u`/`v` in texels, four
vertices as a triangle fan. Three details cost real debugging if missed:

- `flags & 0x10000` means **only vertex 0 is filled in** and the renderer lays the quad
  out from there at the texture's own size.
- `gbh_LoadImage` returns an **index**, not a handle — the game hands it straight back to
  `gbh_BlitImage`. Its argument is an uncompressed 16-bit A1R5G5B5 TGA, rows bottom-up.
- Screen coordinates are in the **game's** resolution (640x480, at camera `+0x68`/`+0x6C`),
  not the back buffer's, so everything is scaled on the way out.

Remix classifies this pass as UI and will not path trace it. That is right for the HUD and
a known limitation for sprites — see below.

## The world coordinates the game keeps anyway

The screen-space vertices are only half the story. GTA2 also writes each vertex's
**absolute** world position four slots further along the same array — `FUN_0046bbf0` for
map faces, `FUN_004b9990` for sprites:

| screen vertices | world shadow |
| --- | --- |
| map faces `0x006632A0` | `0x00663320` |
| sprites `0x0066FE18` | `0x0066FE98` |

`FUN_0046bbf0` adds the camera position back in before storing, so these are absolute map
coordinates. They are the *input* to the game's transform, not its output, so reading them
is not an unprojection — the world-space-only rule holds.

`live_geometry.cpp` uses this for two things:

- **Sprites** — cars, pedestrians, powerups — become real 3D quads, depth tested against
  the map instead of painted over it, and path traced by Remix like everything else. They
  are told apart from the UI by `_ReturnAddress()`: the object draw `FUN_004be060` is the
  only producer of world sprites on `gbh_DrawQuad`.
- **Partial and corner blocks** (slope types 49-61) are *not* built by the static mesh at
  all. Their shapes are awkward — a triangular half-wall plus a rotated triangular lid,
  spread across `FUN_0046ee40` and `FUN_00471c30` — so instead each face is captured from
  the game's own draw stream the first time it is seen, with texture coordinates produced
  by replaying `gbh_DrawTile`'s own algorithm. Exact, and no model of the shape is needed.

The one cost of capture is that a block has to be drawn once before it is kept, so those
few hundred blocks appear as the camera first reaches them and then stay.

## Not yet implemented

Mip chains, and the view-rotation states other than the default `0xFF`.
